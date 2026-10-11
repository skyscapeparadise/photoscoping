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

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from matting_bakeoff import REC2020_TO_709, BiRefNet, srgb_encode  # noqa: E402
from matting_stack import (band_matting, ml_foreground,  # noqa: E402
                           premultiplied_foreground, tiled_alpha)

ROOT = pathlib.Path(__file__).resolve().parent.parent
STAGES = ["coarse", "tiles", "matting"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("clip", type=pathlib.Path)
    parser.add_argument("--lut", required=True, type=pathlib.Path)
    parser.add_argument("--start", type=int, required=True)
    parser.add_argument("--count", type=int, required=True)
    parser.add_argument("--crop", required=True, help="x,y,w,h in source pixels")
    parser.add_argument("--out", required=True, type=pathlib.Path)
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
    video = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
         "-s", f"{panel_w * 4}x{ch}", "-r", f"{fps:.6f}", "-i", "-",
         "-c:v", "libx264", "-crf", "12", "-pix_fmt", "yuv420p", str(args.out / "stages.mp4")],
        stdin=subprocess.PIPE)

    previous: dict[str, np.ndarray] = {}
    changes: dict[str, list[float]] = {s: [] for s in STAGES}
    for i in range(args.count):
        raw = source.stdout.read(w * h * 8)
        rgba = np.frombuffer(raw, dtype=np.float16).reshape(h, w, 4)
        linear = np.clip(rgba[..., :3].astype(np.float32) @ REC2020_TO_709.T, 0.0, None)
        srgb = srgb_encode(linear).astype(np.float32)

        coarse = model(srgb, linear, device)
        tiled = tiled_alpha(model, srgb, coarse, device)
        matted = band_matting(srgb, linear, tiled)
        alphas = {"coarse": coarse, "tiles": tiled, "matting": matted}

        crop = (slice(cy, cy + ch), slice(cx, cx + cw))
        for s in STAGES:
            a = alphas[s][crop]
            if s in previous:
                p = previous[s]
                band = ((a > 0.02) & (a < 0.98)) | ((p > 0.02) & (p < 0.98))
                changes[s].append(float(np.abs(a - p)[band].mean()) if band.any() else 0.0)
            previous[s] = a

        panels = [srgb_encode(premultiplied_foreground(linear, alphas[s]))[crop] for s in STAGES]
        panels.append(srgb_encode(ml_foreground(linear, matted))[crop])
        frame = np.concatenate(panels, axis=1)
        video.stdin.write((np.clip(frame, 0, 1) * 255 + 0.5).astype(np.uint8).tobytes())
        print(f"  frame {args.start + i}", flush=True)

    video.stdin.close()
    video.wait()
    source.stdout.close()
    source.wait()

    print("\nmean frame-to-frame matte change in the crop's edge band:")
    for s in STAGES:
        c = np.array(changes[s])
        print(f"  {s:8s} mean {c.mean():.4f}   max {c.max():.4f}")
    print(f"wrote {args.out / 'stages.mp4'} (coarse | tiles | matting | matting + multilevel fg)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
