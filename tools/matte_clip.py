#!/usr/bin/env python3
"""Mattes a whole clip and renders watchable videos.

Frames come straight from photoscoping (--pipe), so they are exactly the
app's working image. Each one goes through the matting stack in
matting_stack.py -- every frame on its own, time-blind -- and comes out as:

  <name>_comp.mp4   the subject over black, with the clip's own audio
  <name>_matte.mp4  the final alpha matte
  <name>_stability.csv / summary on stdout: frame-to-frame matte change

Stability: in the edge band, how much the matte changes between consecutive
frames, overall and on pixels whose image did not change. Treat the flicker
figure with care on low-contrast footage: a light grey edge moving over a
white wall barely changes the image, so real motion can count as flicker.

usage: .venv/bin/python tools/matte_clip.py CLIP --lut LUT.cube --out DIR
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import subprocess
import sys
import time

import numpy as np
import torch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from matting_bakeoff import REC2020_TO_709, BiRefNet, srgb_encode  # noqa: E402
from matting_stack import refine  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent


def encoder(path: pathlib.Path, w: int, h: int, fps: float, pix_fmt: str,
            audio_from: pathlib.Path | None) -> subprocess.Popen:
    cmd = ["ffmpeg", "-v", "error", "-y",
           "-f", "rawvideo", "-pix_fmt", pix_fmt, "-s", f"{w}x{h}", "-r", f"{fps:.6f}", "-i", "-"]
    if audio_from:
        cmd += ["-i", str(audio_from), "-map", "0:v", "-map", "1:a?", "-c:a", "copy", "-shortest"]
    cmd += ["-c:v", "libx264", "-preset", "medium", "-crf", "14", "-pix_fmt", "yuv420p",
            "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
            "-color_range", "tv", "-movflags", "+faststart", str(path)]
    return subprocess.Popen(cmd, stdin=subprocess.PIPE)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("clip", type=pathlib.Path)
    parser.add_argument("--lut", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--limit", type=int, default=0, help="stop after N frames (for tests)")
    parser.add_argument("--app", type=pathlib.Path, default=ROOT / "build" / "photoscoping")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    name = args.clip.stem

    source = subprocess.Popen([str(args.app), str(args.clip), "--lut", str(args.lut), "--pipe"],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    header = source.stdout.readline().split()
    if not header or header[0] != b"PSRAW":
        print("photoscoping did not start a frame stream", file=sys.stderr)
        return 1
    w, h, frames, fps = int(header[1]), int(header[2]), int(header[3]), float(header[4])
    if args.limit:
        frames = min(frames, args.limit)
    frame_bytes = w * h * 4 * 2
    print(f"{args.clip.name}: {w}x{h}, {frames} frames at {fps:.3f} fps", flush=True)

    device = "mps" if torch.backends.mps.is_available() else "cpu"
    model = BiRefNet("BiRefNet HR-matting @2048²", "ZhengPeng7/BiRefNet_HR-matting", (2048, 2048))
    model.load(device)

    comp_out = encoder(args.out / f"{name}_comp.mp4", w, h, fps, "rgb24", args.clip)
    matte_out = encoder(args.out / f"{name}_matte.mp4", w, h, fps, "gray", None)

    stats = []
    prev_alpha = prev_luma = None
    started = time.perf_counter()
    for index in range(frames):
        raw = source.stdout.read(frame_bytes)
        if len(raw) < frame_bytes:
            print(f"stream ended early at frame {index}", file=sys.stderr)
            break
        t0 = time.perf_counter()
        rgba = np.frombuffer(raw, dtype=np.float16).reshape(h, w, 4)
        linear = rgba[..., :3].astype(np.float32) @ REC2020_TO_709.T
        srgb = srgb_encode(linear).astype(np.float32)

        alpha, subject = refine(model, srgb, np.clip(linear, 0.0, None), device)
        sync_s = time.perf_counter()
        comp = srgb_encode(subject)

        comp_out.stdin.write((comp * 255 + 0.5).astype(np.uint8).tobytes())
        matte_out.stdin.write((alpha * 255 + 0.5).astype(np.uint8).tobytes())

        luma = srgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
        row = {"frame": index, "model_s": sync_s - t0, "total_s": time.perf_counter() - t0}
        if prev_alpha is not None:
            band = ((alpha > 0.02) & (alpha < 0.98)) | ((prev_alpha > 0.02) & (prev_alpha < 0.98))
            d_alpha = np.abs(alpha - prev_alpha)
            d_luma = np.abs(luma - prev_luma)
            still = band & (d_luma < 0.01)
            row.update(
                band_px=int(band.sum()),
                alpha_change=float(d_alpha[band].mean()) if band.any() else 0.0,
                image_change=float(d_luma[band].mean()) if band.any() else 0.0,
                still_px=int(still.sum()),
                flicker=float(d_alpha[still].mean()) if still.any() else 0.0,
            )
        stats.append(row)
        prev_alpha, prev_luma = alpha, luma

        done = index + 1
        elapsed = time.perf_counter() - started
        if done % 10 == 0 or done == frames:
            eta = elapsed / done * (frames - done)
            print(f"  {done}/{frames}  {row['total_s']:.2f} s/frame  eta {eta / 60:.1f} min",
                  flush=True)

    for p in (comp_out, matte_out):
        p.stdin.close()
        p.wait()
    # Closing the pipe is what stops it: a write to a closed pipe ends the
    # process. SIGTERM would not -- SDL turns it into a quit event, which pipe
    # mode never polls.
    source.stdout.close()
    try:
        source.wait(timeout=10)
    except subprocess.TimeoutExpired:
        source.kill()
        source.wait()

    fields = ["frame", "model_s", "total_s", "band_px", "alpha_change", "image_change",
              "still_px", "flicker"]
    with (args.out / f"{name}_stability.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(stats)

    measured = [r for r in stats if "flicker" in r and r["still_px"] > 0]
    if measured:
        flicker = np.array([r["flicker"] for r in measured])
        print(f"\nflicker (mean matte change where the image held still, edge band):")
        print(f"  median {np.median(flicker):.4f}, 95th percentile {np.percentile(flicker, 95):.4f},"
              f" worst {flicker.max():.4f} at frame {measured[int(flicker.argmax())]['frame']}")
    model_s = np.array([r["model_s"] for r in stats])
    print(f"model: {model_s.mean():.2f} s/frame mean; wall clock "
          f"{(time.perf_counter() - started) / 60:.1f} min for {len(stats)} frames")
    print(f"wrote {args.out / f'{name}_comp.mp4'} and {args.out / f'{name}_matte.mp4'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
