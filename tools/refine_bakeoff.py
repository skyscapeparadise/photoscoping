#!/usr/bin/env python3
"""Hair refinement experiment: what each stage adds on top of BiRefNet.

Stages, each building on the last:

  coarse    BiRefNet HR-matting on the whole frame at 2048² (the current pick),
            composited with matte_clip's push-pull decontamination
  tiles     + BiRefNet again on 2048² tiles at the frame's own resolution, so
            strands keep their real width, blended in where the edge is soft
  matting   + closed-form matting (Levin et al.) in a band around the edge at
            full resolution, from a trimap built out of the tiled matte --
            the same shape as Photoshop's Refine Hair: a model decides what is
            subject, a colour-driven solver decides the edge
  fg        + multilevel foreground estimation (Germer et al.) in place of the
            push-pull decontamination

The solver only takes over where subject and wall differ enough in colour for
it to mean anything; a light grey sleeve against white keeps the model's alpha,
which is what preserves its motion blur.

All MIT (BiRefNet, pymatting) or BSD (numba). Not part of the app.

usage: .venv/bin/python tools/refine_bakeoff.py --out DIR FRAME.pfm:x,y[;x,y] ...
"""

from __future__ import annotations

import argparse
import html
import pathlib
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F
from PIL import Image
from scipy import ndimage

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from matte_clip import fill_plate, premultiplied_foreground  # noqa: E402
from matting_bakeoff import REC2020_TO_709, BiRefNet, read_pfm, srgb_encode  # noqa: E402

TILE = 2048
CROP = 560


def tiled_alpha(model: BiRefNet, srgb: np.ndarray, coarse: np.ndarray, device: str) -> np.ndarray:
    """BiRefNet on overlapping full-resolution tiles, feathered together, then
    used only in the soft band of the coarse matte. A tile sees a part of the
    subject without the whole, so it is trusted for edge detail, not for
    deciding what the subject is."""
    h, w = coarse.shape
    xs = sorted({0, max(w - TILE, 0)} | set(range(0, max(w - TILE, 0), TILE * 3 // 4)))
    ys = sorted({0, max(h - TILE, 0)} | set(range(0, max(h - TILE, 0), TILE * 3 // 4)))
    acc = np.zeros((h, w), np.float32)
    weight = np.zeros((h, w), np.float32)
    ramp = np.minimum(np.arange(TILE) + 1, TILE - np.arange(TILE)).astype(np.float32)
    feather = np.minimum.outer(ramp, ramp) ** 2
    for y in ys:
        for x in xs:
            tile = srgb[y:y + TILE, x:x + TILE]
            th, tw = tile.shape[:2]
            if th < TILE or tw < TILE:  # frames smaller than a tile: pad
                tile = np.pad(tile, ((0, TILE - th), (0, TILE - tw), (0, 0)), mode="edge")
            alpha = model(tile, tile, device)[:th, :tw]
            acc[y:y + th, x:x + tw] += alpha * feather[:th, :tw]
            weight[y:y + th, x:x + tw] += feather[:th, :tw]
    tiles = acc / np.maximum(weight, 1e-6)
    soft = (coarse > 0.02) & (coarse < 0.98)
    band = ndimage.binary_dilation(soft, iterations=24)
    # Feather the band itself so the switch from coarse to tiled is invisible.
    blend = np.clip(ndimage.gaussian_filter(band.astype(np.float32), 8), 0, 1)
    return blend * tiles + (1 - blend) * coarse


def trimap_from(alpha: np.ndarray, radius: int) -> np.ndarray:
    """1 = subject, 0 = wall, 0.5 = unknown: the soft band, widened."""
    unknown = ndimage.binary_dilation((alpha > 0.03) & (alpha < 0.97), iterations=radius)
    trimap = np.where(alpha >= 0.5, 1.0, 0.0)
    trimap[unknown] = 0.5
    return trimap


def band_matting(srgb: np.ndarray, linear: np.ndarray, alpha: np.ndarray) -> np.ndarray:
    """Closed-form matting in the unknown band, solved in padded cells so the
    full-resolution problem stays small, and blended toward the model's alpha
    wherever subject and wall are too alike in colour for the solve to mean
    anything."""
    from pymatting import estimate_alpha_cf

    h, w = alpha.shape
    trimap = trimap_from(alpha, radius=10)
    unknown = trimap == 0.5
    solved = alpha.copy()
    cell, pad = 512, 48
    for y0 in range(0, h, cell):
        for x0 in range(0, w, cell):
            if not unknown[y0:y0 + cell, x0:x0 + cell].any():
                continue
            ya, yb = max(y0 - pad, 0), min(y0 + cell + pad, h)
            xa, xb = max(x0 - pad, 0), min(x0 + cell + pad, w)
            t = trimap[ya:yb, xa:xb]
            if not ((t == 1).any() and (t == 0).any()):
                continue  # no known subject and wall to solve between
            a = estimate_alpha_cf(srgb[ya:yb, xa:xb].astype(np.float64), t.astype(np.float64))
            cy, cx = y0 - ya, x0 - xa
            ch, cw = min(cell, h - y0), min(cell, w - x0)
            solved[y0:y0 + ch, x0:x0 + cw] = a[cy:cy + ch, cx:cx + cw]

    luma = linear @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    background = alpha < 0.02
    floor = 0.35 * np.median(luma[background]) if background.any() else 0.0
    wall = fill_plate(linear, background & (luma > floor))
    subject = fill_plate(linear, alpha > 0.95)
    separation = np.sqrt(((subject - wall) ** 2).sum(-1))
    confidence = np.clip((separation - 0.08) / 0.17, 0.0, 1.0)
    out = np.where(unknown, confidence * solved + (1 - confidence) * alpha, alpha)
    return np.clip(out, 0.0, 1.0).astype(np.float32)


def ml_foreground(linear: np.ndarray, alpha: np.ndarray) -> np.ndarray:
    """aF over black via Germer et al.'s multilevel foreground estimation, in
    linear light where the compositing equation holds."""
    from pymatting import estimate_foreground_ml

    fg = estimate_foreground_ml(np.clip(linear, 0, 1).astype(np.float64), alpha.astype(np.float64))
    return (fg * alpha[..., None]).astype(np.float32)


def save(img: np.ndarray, path: pathlib.Path) -> None:
    Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)).save(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("frames", nargs="+", help="FRAME.pfm:x,y[;x,y] -- crop corners")
    parser.add_argument("--out", required=True, type=pathlib.Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    device = "mps" if torch.backends.mps.is_available() else "cpu"
    model = BiRefNet("BiRefNet HR-matting", "ZhengPeng7/BiRefNet_HR-matting", (2048, 2048))
    model.load(device)

    stages = ["coarse", "tiles", "matting", "fg"]
    rows = []
    timings: dict[str, list[float]] = {s: [] for s in stages}
    for spec in args.frames:
        path, _, crops = spec.partition(":")
        crops = [tuple(map(int, c.split(","))) for c in crops.split(";") if c]
        stem = pathlib.Path(path).stem
        linear = np.clip(read_pfm(pathlib.Path(path)) @ REC2020_TO_709.T, 0.0, None)
        srgb = srgb_encode(linear).astype(np.float32)
        print(stem, flush=True)

        t = time.perf_counter()
        coarse = model(srgb, linear, device)
        timings["coarse"].append(time.perf_counter() - t)
        t = time.perf_counter()
        tiled = tiled_alpha(model, srgb, coarse, device)
        timings["tiles"].append(time.perf_counter() - t)
        t = time.perf_counter()
        matted = band_matting(srgb, linear, tiled)
        timings["matting"].append(time.perf_counter() - t)
        t = time.perf_counter()
        fg_comp = ml_foreground(linear, matted)
        timings["fg"].append(time.perf_counter() - t)
        for s in stages:
            print(f"  {s}: {timings[s][-1]:.1f} s", flush=True)

        results = {
            "coarse": (coarse, premultiplied_foreground(linear, coarse)),
            "tiles": (tiled, premultiplied_foreground(linear, tiled)),
            "matting": (matted, premultiplied_foreground(linear, matted)),
            "fg": (matted, fg_comp),
        }
        d = args.out / stem
        d.mkdir(exist_ok=True)
        for ci, (x, y) in enumerate(crops):
            save(srgb[y:y + CROP, x:x + CROP], d / f"source_{ci}.png")
            for s, (alpha, comp) in results.items():
                save(np.repeat(alpha[y:y + CROP, x:x + CROP, None], 3, -1), d / f"{s}_matte_{ci}.png")
                save(srgb_encode(comp[y:y + CROP, x:x + CROP]), d / f"{s}_comp_{ci}.png")
        rows.append((stem, len(crops)))

    labels = {"coarse": "BiRefNet 2048² (current)", "tiles": "+ full-res tiles",
              "matting": "+ closed-form edge band", "fg": "+ multilevel foreground"}
    e = html.escape
    parts = ["""<!doctype html><meta charset="utf-8"><title>hair refinement</title><style>
body{background:#16171b;color:#d8d8dc;font:14px -apple-system,sans-serif;margin:24px}
td,th{padding:4px 8px;text-align:left;vertical-align:top} img{width:280px;display:block}
th{color:#9a9aa2;font-weight:500} h2{font-weight:500;margin-top:36px}</style>
<h1>Hair refinement, stage by stage</h1><p>Each stage builds on the one before.
Crops are 1:1 source pixels; composites over black.</p><table><tr><th>stage</th>
<th>added seconds / 4K frame (mean)</th></tr>"""]
    for s in stages:
        parts.append(f"<tr><td>{e(labels[s])}</td><td>{np.mean(timings[s]):.1f}</td></tr>")
    parts.append("</table>")
    for stem, n in rows:
        for ci in range(n):
            parts.append(f"<h2>{e(stem)} · crop {ci + 1}</h2><table><tr><th>source</th>")
            parts += [f"<th>{e(labels[s])}</th>" for s in stages]
            parts.append(f"</tr><tr><td><img src='{stem}/source_{ci}.png'></td>")
            parts += [f"<td><img src='{stem}/{s}_comp_{ci}.png'></td>" for s in stages]
            parts.append("</tr><tr><td></td>")
            parts += [f"<td><img src='{stem}/{s}_matte_{ci}.png'></td>" for s in stages]
            parts.append("</tr></table>")
    (args.out / "index.html").write_text("\n".join(parts))
    print(f"wrote {args.out / 'index.html'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
