#!/usr/bin/env python3
"""Which matting stage makes the hair flicker?

Runs consecutive frames through the stack, keeping every stage's matte, and
measures in a crop (the hair) how much each stage's matte changes from one
frame to the next. All stages see the same motion, so motion cancels out of
the comparison: a stage whose number rises over the stage before it is adding
frame-to-frame change the footage does not have.

Also renders the crop through every stage side by side, so it can be watched.

usage: .venv/bin/python tools/stage_flicker.py CLIP --lut LUT --start N --count N
           --crop x,y,w,h --out DIR
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

import numpy as np
import torch
from scipy import ndimage

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from matting_bakeoff import REC2020_TO_709, BiRefNet, srgb_encode  # noqa: E402
from matting_stack import (band_matting, ml_foreground,  # noqa: E402
                           premultiplied_foreground, tiled_alpha)

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("clip", type=pathlib.Path)
    parser.add_argument("--lut", required=True, type=pathlib.Path)
    parser.add_argument("--start", type=int, required=True)
    parser.add_argument("--count", type=int, required=True)
    parser.add_argument("--crop", required=True, help="x,y,w,h in source pixels")
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--set", choices=["stages", "blur"], default="stages",
                        help="stages: the stack stage by stage; blur: tiles with the "
                             "model's input pre-blurred by 0, 0.7 and 1.2 px")
    args = parser.parse_args()
    cx, cy, cw, ch = map(int, args.crop.split(","))
    args.out.mkdir(parents=True, exist_ok=True)

    source = subprocess.Popen([str(ROOT / "build" / "photoscoping"), str(args.clip), "--lut",
                               str(args.lut), "--frame", str(args.start), "--pipe"],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    header = source.stdout.readline().split()
    w, h, fps = int(header[1]), int(header[2]), float(header[4])

    device = "mps" if torch.backends.mps.is_available() else "cpu"
    model = BiRefNet("BiRefNet HR-matting", "ZhengPeng7/BiRefNet_HR-matting", (2048, 2048))
    model.load(device)

    # Four panels: coarse, tiles, matting (all decontaminated the v1 way, so
    # only the matte differs), then matting with the multilevel foreground,
    # which is what the latest render shows.
    panel_w = cw
    n_panels = 4 if args.set == "stages" else 3
    video = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
         "-s", f"{panel_w * n_panels}x{ch}", "-r", f"{fps:.6f}", "-i", "-",
         "-c:v", "libx264", "-crf", "12", "-pix_fmt", "yuv420p", str(args.out / f"{args.set}.mp4")],
        stdin=subprocess.PIPE)

    previous: dict[str, np.ndarray] = {}
    changes: dict[str, list[float]] = {}
    for i in range(args.count):
        raw = source.stdout.read(w * h * 8)
        rgba = np.frombuffer(raw, dtype=np.float16).reshape(h, w, 4)
        linear = np.clip(rgba[..., :3].astype(np.float32) @ REC2020_TO_709.T, 0.0, None)
        srgb = srgb_encode(linear).astype(np.float32)

        if args.set == "blur":
            alphas = {}
            for sigma in (0.0, 0.7, 1.2):
                seen = ndimage.gaussian_filter(srgb, (sigma, sigma, 0)) if sigma else srgb
                alphas[f"blur {sigma}"] = tiled_alpha(model, seen, model(seen, linear, device),
                                                      device)
        else:
            coarse = model(srgb, linear, device)
            tiled = tiled_alpha(model, srgb, coarse, device)
            matted = band_matting(srgb, linear, tiled)
            alphas = {"coarse": coarse, "tiles": tiled, "matting": matted}
        names = list(alphas)

        crop = (slice(cy, cy + ch), slice(cx, cx + cw))
        for s in names:
            a = alphas[s][crop]
            if s in previous:
                p = previous[s]
                band = ((a > 0.02) & (a < 0.98)) | ((p > 0.02) & (p < 0.98))
                changes.setdefault(s, []).append(float(np.abs(a - p)[band].mean()) if band.any() else 0.0)
            previous[s] = a

        panels = [srgb_encode(premultiplied_foreground(linear, alphas[s]))[crop] for s in names]
        if args.set == "stages":
            panels.append(srgb_encode(ml_foreground(linear, matted))[crop])
        frame = np.concatenate(panels, axis=1)
        video.stdin.write((np.clip(frame, 0, 1) * 255 + 0.5).astype(np.uint8).tobytes())
        print(f"  frame {args.start + i}", flush=True)

    video.stdin.close()
    video.wait()
    source.stdout.close()
    source.wait()

    print("\nmean frame-to-frame matte change in the crop's edge band:")
    for s, c in changes.items():
        c = np.array(c)
        print(f"  {s:8s} mean {c.mean():.4f}   max {c.max():.4f}")
    layout = ("coarse | tiles | matting | matting + multilevel fg" if args.set == "stages"
              else "tiles | tiles + 0.7 px input blur | tiles + 1.2 px input blur")
    print(f"wrote {args.out / (args.set + '.mp4')} ({layout})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
