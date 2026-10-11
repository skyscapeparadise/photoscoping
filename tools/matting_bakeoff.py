#!/usr/bin/env python3
"""Subject-separation bake-off: candidate matting methods on real frames.

Not part of the app. This picks which model photoscoping builds around, by
running every candidate on the same frames and putting the results side by
side with their timings on this machine.

Input is frames exported by the app itself (photoscoping --export, a PFM of
the linear Rec.2020 working image), so every method sees exactly what the
real pipeline produces. Models get those frames as 8-bit sRGB, which is what
they were trained on; the classical cyc key works on the linear values.

Every model here is MIT-licensed and pinned to a commit, so a rerun compares
the same weights. Nothing needing payment or a vendor agreement is used.

usage: .venv/bin/python tools/matting_bakeoff.py FRAME.pfm [...] --out DIR
"""

from __future__ import annotations

import argparse
import html
import json
import pathlib
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F
from PIL import Image
from scipy import ndimage

PINS = {
    "ZhengPeng7/BiRefNet_HR-matting": "5d6b6f8adcb5b417c871b1d84ceaae9871355b7f",
    "ZhengPeng7/BiRefNet_HR": "a7a562f6fd16021180f2f4348f4de003a2d3d1e1",
    "ZhengPeng7/BiRefNet_dynamic-matting": "074df545be87034e74a96bf71566ecbbc4c15f0a",
    "PramaLLC/BEN2": "e48a20765fb421d19dcdb0bf3cc61e802ca5ec8f",
}

# Rec.2020 -> Rec.709 primaries, linear (inverse of BT.2087).
REC2020_TO_709 = np.array([[1.6605, -0.5876, -0.0728],
                           [-0.1246, 1.1329, -0.0083],
                           [-0.0182, -0.1006, 1.1187]], dtype=np.float32)
IMAGENET_MEAN = torch.tensor([0.485, 0.456, 0.406]).view(1, 3, 1, 1)
IMAGENET_STD = torch.tensor([0.229, 0.224, 0.225]).view(1, 3, 1, 1)
# Composites go over black: leftover cyc shows up on it as a pale fringe,
# which a checkerboard hides.
BACKGROUND = np.zeros(3, dtype=np.float32)


def read_pfm(path: pathlib.Path) -> np.ndarray:
    with path.open("rb") as f:
        if f.readline().strip() != b"PF":
            raise ValueError(f"{path}: not a colour PFM")
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.fromfile(f, dtype="<f4" if scale < 0 else ">f4", count=w * h * 3)
    return np.ascontiguousarray(data.reshape(h, w, 3)[::-1])


def srgb_encode(linear: np.ndarray) -> np.ndarray:
    l = np.clip(linear, 0.0, 1.0)
    return np.where(l <= 0.0031308, l * 12.92, 1.055 * np.power(l, 1 / 2.4) - 0.055)


def upsample(alpha: torch.Tensor, h: int, w: int) -> np.ndarray:
    a = F.interpolate(alpha.float().cpu().view(1, 1, *alpha.shape[-2:]), size=(h, w),
                      mode="bilinear", align_corners=False)
    return a[0, 0].clamp(0, 1).numpy()


def sync(device: str) -> None:
    if device == "mps":
        torch.mps.synchronize()


class TorchMethod:
    """A model run at a fixed input size, its alpha upsampled to the frame."""

    def __init__(self, name: str, size: tuple[int, int] | None, half: bool):
        self.name = name
        self.size = size  # (w, h); None means square, set by subclass
        self.half = half
        self.model = None

    def load(self, device: str) -> None:
        raise NotImplementedError

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        raise NotImplementedError

    def unload(self) -> None:
        self.model = None
        if torch.backends.mps.is_available():
            torch.mps.empty_cache()

    def __call__(self, srgb: np.ndarray, linear: np.ndarray, device: str) -> np.ndarray:
        h, w = srgb.shape[:2]
        tw, th = self.size
        x = torch.from_numpy(srgb).permute(2, 0, 1).unsqueeze(0).float()
        x = F.interpolate(x, size=(th, tw), mode="bicubic", align_corners=False).clamp(0, 1)
        x = ((x - IMAGENET_MEAN) / IMAGENET_STD).to(device)
        x = x.half() if self.half else x
        with torch.no_grad():
            alpha = self.forward(x)
        if torch.isnan(alpha).any():
            raise FloatingPointError(f"{self.name}: NaN in output")
        return upsample(alpha, h, w)


class BiRefNet(TorchMethod):
    def __init__(self, name: str, repo: str, size: tuple[int, int]):
        super().__init__(name, size, half=True)
        self.repo = repo

    def load(self, device: str) -> None:
        from transformers import AutoModelForImageSegmentation
        model = AutoModelForImageSegmentation.from_pretrained(
            self.repo, revision=PINS[self.repo], trust_remote_code=True)
        model.eval().to(device)
        self.model = model.half() if self.half else model

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.model(x)[-1].sigmoid()


class Ben2(TorchMethod):
    def __init__(self):
        # BEN2 splits its input into fixed patches, so it only runs at 1024².
        super().__init__("BEN2 Base @1024²", (1024, 1024), half=False)

    def load(self, device: str) -> None:
        from huggingface_hub import snapshot_download
        repo = "PramaLLC/BEN2"
        path = pathlib.Path(snapshot_download(repo, revision=PINS[repo],
                                              allow_patterns=["BEN2.py", "BEN2_Base.pth"]))
        sys.path.insert(0, str(path))
        try:
            import BEN2  # noqa: N813 -- the module's own name
        finally:
            sys.path.remove(str(path))
        model = BEN2.BEN_Base()
        model.loadcheckpoints(str(path / "BEN2_Base.pth"))
        self.model = model.eval().to(device)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.model(x)


class CycKey:
    """Classical white-cyc key, the planned non-AI half of the pipeline.

    Estimates the cyc's white per region from this frame alone (bright,
    near-neutral areas, smoothed into a field so uneven lighting is followed)
    and keys how far each pixel falls below it in its most-darkened channel.
    Time-blind by construction: a strobe changes the field, not the key.
    """

    name = "cyc key (classical) @native"

    def load(self, device: str) -> None:
        pass

    def unload(self) -> None:
        pass

    def __call__(self, srgb: np.ndarray, linear: np.ndarray, device: str) -> np.ndarray:
        h, w = linear.shape[:2]
        s = 16
        small = linear[: h // s * s, : w // s * s].reshape(h // s, s, w // s, s, 3).mean((1, 3))
        lum = small @ np.array([0.2627, 0.6780, 0.0593], dtype=np.float32)
        hi, lo = small.max(-1), small.min(-1)
        sat = (hi - lo) / np.maximum(hi, 1e-6)
        cyc = (lum > 0.7 * np.percentile(lum, 95)) & (sat < 0.15)
        # Stay clear of the subject's edge, where blocks are part subject.
        cyc = ndimage.binary_erosion(cyc, iterations=2)
        weight = ndimage.gaussian_filter(cyc.astype(np.float32), 6)
        field = np.stack([ndimage.gaussian_filter(small[..., c] * cyc, 6) for c in range(3)], -1)
        field = np.where(weight[..., None] > 1e-3, field / np.maximum(weight[..., None], 1e-6),
                         small[cyc].mean(0))
        white = np.stack([upsample(torch.from_numpy(np.ascontiguousarray(field[..., c])), h, w)
                          for c in range(3)], -1)
        drop = np.clip(1.0 - (linear / np.maximum(white, 1e-4)).min(-1), 0.0, None)
        t = np.clip((drop - 0.05) / (0.30 - 0.05), 0.0, 1.0)
        return (t * t * (3 - 2 * t)).astype(np.float32)


def save_gray16(alpha: np.ndarray, path: pathlib.Path) -> None:
    Image.fromarray((np.clip(alpha, 0, 1) * 65535 + 0.5).astype(np.uint16)).save(path)


def save_rgb(img: np.ndarray, path: pathlib.Path, width: int | None = None) -> None:
    im = Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8))
    if width and im.width > width:
        im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
    im.save(path)


def composite(srgb: np.ndarray, alpha: np.ndarray) -> np.ndarray:
    # Display-referred over the background. No foreground colour estimation: the
    # point is to see how much cyc each matte lets through.
    background = srgb_encode(BACKGROUND)
    return srgb * alpha[..., None] + background * (1 - alpha[..., None])


def edge_crops(alpha: np.ndarray, size: int) -> list[tuple[int, int]]:
    """Top-left corners of two crops on the subject's hardest edges: the top
    of the silhouette (usually hair) and its left side at mid height."""
    h, w = alpha.shape
    ys, xs = np.nonzero(alpha > 0.5)
    if len(ys) == 0:
        return [((w - size) // 2, (h - size) // 2)]
    top = ys.min()
    top_x = int(xs[ys < top + 40].mean())
    mid = (top + ys.max()) // 2
    row = np.nonzero(alpha[mid] > 0.5)[0]
    left_x = int(row.min()) if len(row) else top_x

    def clamp(x: int, y: int) -> tuple[int, int]:
        return (int(np.clip(x - size // 2, 0, w - size)), int(np.clip(y - size // 2, 0, h - size)))

    return [clamp(top_x, top + size // 3), clamp(left_x, mid)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("frames", nargs="+", type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--crop", type=int, default=640, help="edge crop size in source pixels")
    args = parser.parse_args()

    device = "mps" if torch.backends.mps.is_available() else "cpu"
    methods = [
        BiRefNet("BiRefNet HR-matting @2048² (official square)",
                 "ZhengPeng7/BiRefNet_HR-matting", (2048, 2048)),
        BiRefNet("BiRefNet HR-matting @2048×1152 (aspect kept)",
                 "ZhengPeng7/BiRefNet_HR-matting", (2048, 1152)),
        BiRefNet("BiRefNet HR (segmentation) @2048×1152",
                 "ZhengPeng7/BiRefNet_HR", (2048, 1152)),
        # Input sides must be multiples of 32; 2304×1296 is not.
        BiRefNet("BiRefNet dynamic-matting @2304×1280",
                 "ZhengPeng7/BiRefNet_dynamic-matting", (2304, 1280)),
        Ben2(),
        CycKey(),
    ]

    frames = []
    for path in args.frames:
        linear2020 = read_pfm(path)
        linear = linear2020 @ REC2020_TO_709.T
        frames.append((path.stem, linear, srgb_encode(linear).astype(np.float32)))
    args.out.mkdir(parents=True, exist_ok=True)

    results: dict[str, dict] = {}
    alphas: dict[tuple[str, str], np.ndarray] = {}
    for mi, method in enumerate(methods):
        print(f"[{mi + 1}/{len(methods)}] {method.name}", flush=True)
        try:
            t0 = time.perf_counter()
            method.load(device)
            load_s = time.perf_counter() - t0
            # One untimed run first: MPS compiles kernels on first use.
            method(frames[0][2], frames[0][1], device)
            sync(device)
            times = []
            for stem, linear, srgb in frames:
                t0 = time.perf_counter()
                alpha = method(srgb, linear, device)
                sync(device)
                times.append(time.perf_counter() - t0)
                alphas[(stem, method.name)] = alpha
                print(f"    {stem}: {times[-1] * 1000:.0f} ms", flush=True)
        except Exception as error:  # one broken candidate must not sink the rest
            print(f"    FAILED: {error}", flush=True)
            method.unload()
            continue
        peak = torch.mps.driver_allocated_memory() / 2**30 if device == "mps" else 0.0
        results[method.name] = {"load_s": load_s, "ms": [t * 1000 for t in times],
                                "mean_ms": 1000 * sum(times) / len(times), "gpu_gib": peak}
        method.unload()
    methods = [m for m in methods if m.name in results]

    # Crops follow the first model's silhouette, so every method is compared
    # on the same pixels.
    reference = methods[0].name
    rows = []
    for fi, (stem, linear, srgb) in enumerate(frames):
        crops = edge_crops(alphas[(stem, reference)], args.crop)
        frame_dir = args.out / stem
        frame_dir.mkdir(exist_ok=True)
        save_rgb(srgb, frame_dir / "source.png", 1280)
        for ci, (x, y) in enumerate(crops):
            save_rgb(srgb[y:y + args.crop, x:x + args.crop], frame_dir / f"source_crop{ci}.png")
        cells = []
        for mi, method in enumerate(methods):
            alpha = alphas[(stem, method.name)]
            tag = f"m{mi}"
            save_gray16(alpha, frame_dir / f"{tag}_alpha.png")
            comp = composite(srgb, alpha)
            save_rgb(comp, frame_dir / f"{tag}_comp.png", 1280)
            save_rgb(np.repeat(alpha[..., None], 3, -1), frame_dir / f"{tag}_matte.png", 1280)
            for ci, (x, y) in enumerate(crops):
                save_rgb(comp[y:y + args.crop, x:x + args.crop], frame_dir / f"{tag}_crop{ci}.png")
            cells.append((method.name, tag, results[method.name]["ms"][fi]))
        rows.append((stem, len(crops), cells))

    (args.out / "timings.json").write_text(json.dumps(results, indent=2))
    write_page(args.out, rows, results, device)
    print(f"\nwrote {args.out / 'index.html'}")
    return 0


def write_page(out: pathlib.Path, rows, results, device: str) -> None:
    e = html.escape
    parts = [f"""<!doctype html><meta charset="utf-8"><title>photoscoping matting bake-off</title>
<style>
body {{ background:#16171b; color:#d8d8dc; font:14px -apple-system, sans-serif; margin:24px; }}
h1 {{ font-weight:600; }} h2 {{ margin-top:40px; font-weight:500; }}
table {{ border-collapse:collapse; }} td, th {{ padding:6px 12px; text-align:left; vertical-align:top; }}
th {{ color:#9a9aa2; font-weight:500; }} tr.m td {{ border-top:1px solid #2a2b31; }}
img {{ display:block; max-width:100%; }} .comp {{ width:520px; }} .crop {{ width:300px; image-rendering:auto; }}
.label {{ width:220px; }} .ms {{ color:#9a9aa2; }}
</style>
<h1>Matting bake-off</h1>
<p>Composites are over black so leftover cyc shows as a pale fringe. Crops are 1:1
source pixels on the subject's top and left edges. Timings: {e(device)}, per frame, excluding
model load.</p>
<table><tr><th>method</th><th>mean ms / frame</th><th>load s</th><th>GPU memory GiB</th></tr>"""]
    for name, r in results.items():
        parts.append(f"<tr><td>{e(name)}</td><td>{r['mean_ms']:.0f}</td>"
                     f"<td>{r['load_s']:.1f}</td><td>{r['gpu_gib']:.1f}</td></tr>")
    parts.append("</table>")
    for stem, n_crops, cells in rows:
        parts.append(f"<h2>{e(stem)}</h2><table><tr class='m'><td class='label'>source</td>"
                     f"<td><img class='comp' src='{stem}/source.png'></td>")
        parts += [f"<td><img class='crop' src='{stem}/source_crop{c}.png'></td>" for c in range(n_crops)]
        parts.append("</tr>")
        for name, tag, ms in cells:
            parts.append(f"<tr class='m'><td class='label'>{e(name)}<div class='ms'>{ms:.0f} ms</div>"
                         f"<a href='{stem}/{tag}_matte.png'>matte</a> · "
                         f"<a href='{stem}/{tag}_alpha.png'>16-bit alpha</a></td>"
                         f"<td><img class='comp' src='{stem}/{tag}_comp.png'></td>")
            parts += [f"<td><img class='crop' src='{stem}/{tag}_crop{c}.png'></td>"
                      for c in range(n_crops)]
            parts.append("</tr>")
        parts.append("</table>")
    (out / "index.html").write_text("\n".join(parts))


if __name__ == "__main__":
    raise SystemExit(main())
