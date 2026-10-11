#!/usr/bin/env python3
"""Temporal stability of a time-blind matting method, without any motion.

A method that sees each frame on its own can only flicker by reacting to the
small differences between consecutive frames that ought not to matter. So
stability is measured directly: take one frame, perturb it imperceptibly, and
see how much the output moves.

  noise     fresh sensor-like grain (what changes between any two frames)
  shift     a quarter-pixel translation, undone afterwards (slight motion)
  exposure  +-5% gain (dynamic lighting; what strobe footage does)

For each method and perturbation: mean |change| of the matte and of the
composite over black, in the region where either is not trivially 0 or 1.
Lower is steadier. Values are on the 0..1 scale.

usage: .venv/bin/python tools/stability_bench.py FRAME.pfm [...] --crop x,y,w,h
"""

from __future__ import annotations

import argparse
import pathlib
import sys

import numpy as np
import torch
from scipy import ndimage

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from matting_bakeoff import REC2020_TO_709, BiRefNet, read_pfm, srgb_encode  # noqa: E402
from matting_stack import premultiplied_foreground, tiled_alpha  # noqa: E402


def perturbations(linear: np.ndarray, rng: np.random.Generator):
    """(name, perturbed linear frame, function mapping an output back onto
    the original frame's pixel grid)."""
    identity = lambda x: x  # noqa: E731
    # Noise scaled like photon noise: stronger in brighter areas. Sized to
    # the clip's own grain, which is around 1% of the signal on the wall.
    sigma = 0.004 + 0.008 * np.sqrt(np.clip(linear, 0, None))
    yield "noise", linear + rng.normal(size=linear.shape).astype(np.float32) * sigma, identity
    shift = 0.25
    shifted = ndimage.shift(linear, (shift, shift, 0), order=3, mode="nearest")
    back = lambda x: ndimage.shift(x, (-shift, -shift) + (0,) * (x.ndim - 2),  # noqa: E731
                                   order=3, mode="nearest")
    yield "shift", shifted, back
    yield "exposure+5%", linear * 1.05, identity
    yield "exposure-5%", linear * 0.95, identity


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("frames", nargs="+", type=pathlib.Path)
    parser.add_argument("--crop", default="", help="x,y,w,h to measure in (default: all)")
    parser.add_argument("--methods", default="", help="comma-separated subset to run")
    parser.add_argument("--perturb", default="noise,shift", help="which perturbations")
    args = parser.parse_args()

    device = "mps" if torch.backends.mps.is_available() else "cpu"
    model = BiRefNet("BiRefNet HR-matting", "ZhengPeng7/BiRefNet_HR-matting", (2048, 2048))
    model.load(device)

    def coarse(srgb, linear):
        return model(srgb, linear, device)

    def tiles(srgb, linear):
        return tiled_alpha(model, srgb, coarse(srgb, linear), device)

    def flip(fn):
        def run(srgb, linear):
            a = fn(srgb, linear)
            b = fn(np.ascontiguousarray(srgb[:, ::-1]), np.ascontiguousarray(linear[:, ::-1]))
            return 0.5 * (a + b[:, ::-1])
        return run

    def denoised(fn, sigma):
        # Gaussian pre-filter on what the model sees; the composite still
        # uses the untouched frame.
        def run(srgb, linear):
            soft = ndimage.gaussian_filter(srgb, (sigma, sigma, 0))
            return fn(soft, linear)
        return run

    def nlm(fn):
        import cv2
        def run(srgb, linear):
            u8 = (np.clip(srgb, 0, 1) * 255 + 0.5).astype(np.uint8)
            clean = cv2.fastNlMeansDenoisingColored(u8, None, 3, 3, 5, 15)
            return fn(clean.astype(np.float32) / 255, linear)
        return run

    methods = {
        "coarse (v1)": coarse,
        "tiles": tiles,
        "tiles + flip average": flip(tiles),
        "tiles + blur 0.7px": denoised(tiles, 0.7),
        "tiles + blur 1.2px": denoised(tiles, 1.2),
        "tiles + NL-means": nlm(tiles),
        "tiles + blur 0.7 + flip": flip(denoised(tiles, 0.7)),
    }
    only = [m.strip() for m in args.methods.split(",")] if args.methods else list(methods)
    methods = {k: v for k, v in methods.items() if k in only}

    rng = np.random.default_rng(7)
    totals: dict[tuple[str, str], list[tuple[float, float]]] = {}
    for path in args.frames:
        linear = np.clip(read_pfm(path) @ REC2020_TO_709.T, 0.0, None)
        if args.crop:
            x, y, w, h = map(int, args.crop.split(","))
        else:
            x, y, w, h = 0, 0, linear.shape[1], linear.shape[0]
        print(path.stem, flush=True)
        for name, fn in methods.items():
            base_a = fn(srgb_encode(linear).astype(np.float32), linear)
            base_c = srgb_encode(premultiplied_foreground(linear, base_a))
            for pname, plin, back in perturbations(linear, rng):
                if pname not in args.perturb.split(","):
                    continue
                plin = np.clip(plin, 0.0, None).astype(np.float32)
                a = fn(srgb_encode(plin).astype(np.float32), plin)
                c = srgb_encode(premultiplied_foreground(plin, a))
                a, c = back(a), back(c)
                region = (slice(y + 8, y + h - 8), slice(x + 8, x + w - 8))  # skip shift borders
                ra, rb = base_a[region], a[region]
                live = ((ra > 0.01) & (ra < 0.99)) | ((rb > 0.01) & (rb < 0.99))
                da = float(np.abs(ra - rb)[live].mean()) if live.any() else 0.0
                dc = float(np.abs(base_c[region] - c[region]).mean(-1)[live].mean()) if live.any() else 0.0
                totals.setdefault((name, pname), []).append((da, dc))
                print(f"  {name:22s} {pname:12s} matte {da:.4f}  comp {dc:.4f}", flush=True)

    print("\nmean over frames (matte change / composite change; lower is steadier):")
    names = list(methods)
    pnames = sorted({p for _, p in totals}, key=["noise", "shift", "exposure+5%", "exposure-5%"].index)
    print(f"  {'':22s} " + " ".join(f"{p:>20s}" for p in pnames))
    for n in names:
        cells = []
        for p in pnames:
            v = np.mean(totals[(n, p)], axis=0)
            cells.append(f"{v[0]:.4f} / {v[1]:.4f}")
        print(f"  {n:22s} " + " ".join(f"{c:>20s}" for c in cells))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
