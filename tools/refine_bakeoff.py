#!/usr/bin/env python3
"""Hair refinement experiment: what each stage adds on top of BiRefNet.

Stages, each building on the last:

  coarse    BiRefNet HR-matting on the whole frame at 2048² (the current pick),
            composited with matte_clip's push-pull decontamination
  tiles     + BiRefNet again on 2048×1152 tiles at the frame's own resolution, so
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
from matting_stack import (band_matting, ml_foreground,  # noqa: E402
                           premultiplied_foreground, tiled_alpha)
from matting_bakeoff import REC2020_TO_709, BiRefNet, read_pfm, srgb_encode  # noqa: E402

CROP = 560


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
