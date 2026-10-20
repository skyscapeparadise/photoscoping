#!/usr/bin/env python3
"""Can the matting model leave Python? A short test, not part of the app.

photoscoping will go cross-platform, so a native matting stack would run its
model through ONNX Runtime: Core ML on the Mac, DirectML on Windows, CUDA or
CPU elsewhere. The model is the part that might not survive the trip --
BiRefNet's decoder uses deformable convolutions, which not every runtime
has -- so this exports it and checks it against PyTorch on a few real frames:
how fast, and how close.

  fp16     the Core ML-ready model converted to half precision
  check    the deformable convolution rebuilt from standard operations,
           against torchvision's, through the whole model
  export   BiRefNet HR-matting at 2048², fp32, to ONNX, with that swap made
  frames   a few frames of the clip through photoscoping --pipe, preprocessed
           exactly as matting_bakeoff.TorchMethod does, saved as raw floats
           so the C++ side (tools/onnx_spike/) reads the same bytes
  bench    PyTorch on MPS (today's path) against ONNX Runtime with Core ML
           and on the CPU: time per frame and difference in alpha
  compare  the C++ side's alphas against PyTorch's
  stack    the Python stack's stages on frames of a clip, saved
  stack-compare
           the native stack's stages (photoscoping --matte) against those

Everything goes to renders/onnx_spike/.

usage: .venv/bin/python tools/onnx_spike.py export
       .venv/bin/python tools/onnx_spike.py frames CLIP [--lut LUT] [--at 100 300 500]
       .venv/bin/python tools/onnx_spike.py bench
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from matting_bakeoff import IMAGENET_MEAN, IMAGENET_STD, REC2020_TO_709, srgb_encode  # noqa: E402
from export_matting_model import (Head, load_torch, make_coreml_friendly,  # noqa: E402
                                 make_portable)

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "renders" / "onnx_spike"
SIZE = 2048
MODEL = OUT / f"birefnet_hr_matting_{SIZE}.onnx"
# The same model with the backbone rewritten for Core ML (make_coreml_friendly).
MODEL_COREML = OUT / f"birefnet_hr_matting_{SIZE}_coreml.onnx"
# That model in half precision, as PyTorch runs it today, with its input and
# output left as fp32 so the callers do not change.
MODEL_COREML_FP16 = OUT / f"birefnet_hr_matting_{SIZE}_coreml_fp16.onnx"


def check(args) -> int:
    """The portable deformable convolution against torchvision's, whole model."""
    torch.manual_seed(0)
    model = load_torch("cpu", half=False)
    x = torch.randn(1, 3, args.size, args.size)
    with torch.no_grad():
        want = model(x)[-1].sigmoid()
        print(f"swapped {make_portable(model)} deformable convolutions")
        if args.coreml:
            print(f"rewrote {make_coreml_friendly(model)} attention blocks")
        got = model(x)[-1].sigmoid()
    d = (got - want).abs()
    print(f"at {args.size}²: max |Δα| {d.max().item():.2e}, mean {d.mean().item():.2e}")
    return 0


def export(args) -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    model = load_torch("cpu", half=False)
    if not args.decompose:
        print(f"swapped {make_portable(model)} deformable convolutions")
    if args.coreml:
        print(f"rewrote {make_coreml_friendly(model)} attention blocks")
    head = Head(model)
    path = MODEL_COREML if args.coreml else MODEL
    x = torch.zeros(1, 3, SIZE, SIZE)
    if args.decompose:
        # Deformable convolution as plain gathers and arithmetic, for a
        # runtime without a DeformConv kernel.
        import deform_conv2d_onnx_exporter
        # It looks for JitScalarType where PyTorch kept it before 2.14, and on
        # not finding it takes an older path that no longer works.
        from torch.onnx._internal.torchscript_exporter._type_utils import JitScalarType
        deform_conv2d_onnx_exporter.JitScalarType = JitScalarType
        deform_conv2d_onnx_exporter.register_deform_conv2d_onnx_op()
    # Without this the trace runs with autograd on and keeps every activation
    # for a backward pass that never comes: past 45 GB at 2048².
    head.requires_grad_(False)
    t0 = time.perf_counter()
    with torch.no_grad():
        torch.onnx.export(head, (x,), str(path), input_names=["image"], output_names=["alpha"],
                          opset_version=args.opset, dynamo=False)
    print(f"exported {path} ({path.stat().st_size / 1e6:.0f} MB) "
          f"in {time.perf_counter() - t0:.0f} s")
    import onnx
    ops = sorted({n.op_type for n in onnx.load(str(path), load_external_data=False).graph.node})
    print(f"{len(ops)} op types: {' '.join(ops)}")
    return 0


def fp16(args) -> int:
    import onnx
    from onnxconverter_common import float16
    t0 = time.perf_counter()
    # Folded first. The window and padding arithmetic is on fixed sizes, so
    # it reduces to constants; left in, the converter makes it half too, and
    # Core ML refuses the one-element half tensors that come out of it.
    import onnxruntime as ort
    folded = OUT / "folded_fp32.onnx"
    options = ort.SessionOptions()
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
    options.optimized_model_filepath = str(folded)
    ort.InferenceSession(str(MODEL_COREML), options, providers=["CPUExecutionProvider"])
    print(f"folded: {len(onnx.load(str(MODEL_COREML), load_external_data=False).graph.node)} -> "
          f"{len(onnx.load(str(folded), load_external_data=False).graph.node)} nodes")
    model = float16.convert_float_to_float16(onnx.load(str(folded)), keep_io_types=True)
    folded.unlink()
    # The converter declares the graph's tensors half but leaves the graph's
    # own casts to float as they were, so those casts no longer produce the
    # type declared for their output and the model will not load. Each is
    # made to produce what was declared. The casts the converter inserts
    # itself, around operations it keeps in fp32, declare float and stay.
    declared = {v.name: v.type.tensor_type.elem_type for v in model.graph.value_info}
    fixed = 0
    for node in model.graph.node:
        if node.op_type != "Cast" or declared.get(node.output[0]) != onnx.TensorProto.FLOAT16:
            continue
        for attr in node.attribute:
            if attr.name == "to" and attr.i == onnx.TensorProto.FLOAT:
                attr.i = onnx.TensorProto.FLOAT16
                fixed += 1
    print(f"retyped {fixed} casts to half")
    onnx.save(model, str(MODEL_COREML_FP16))
    print(f"wrote {MODEL_COREML_FP16} ({MODEL_COREML_FP16.stat().st_size / 1e6:.0f} MB) "
          f"in {time.perf_counter() - t0:.0f} s")
    return 0


def frames(args) -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    for index in args.at:
        cmd = [str(ROOT / "build" / "photoscoping"), str(args.clip), "--pipe", "--frame",
               str(index)]
        if args.lut:
            cmd += ["--lut", str(args.lut)]
        source = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        header = source.stdout.readline().split()
        w, h = int(header[1]), int(header[2])
        raw = source.stdout.read(w * h * 8)
        source.stdout.close()
        source.wait()
        rgba = np.frombuffer(raw, dtype=np.float16).reshape(h, w, 4)
        srgb = srgb_encode(rgba[..., :3].astype(np.float32) @ REC2020_TO_709.T).astype(np.float32)
        # As TorchMethod.__call__ does it, short of the move to the device.
        x = torch.from_numpy(srgb).permute(2, 0, 1).unsqueeze(0)
        x = F.interpolate(x, size=(SIZE, SIZE), mode="bicubic", align_corners=False).clamp(0, 1)
        x = (x - IMAGENET_MEAN) / IMAGENET_STD
        path = OUT / f"input_{index:06d}.f32"
        x.numpy().astype(np.float32).tofile(path)
        print(f"frame {index}: {w}x{h} -> {path.name}")
    return 0


def inputs() -> list[tuple[str, np.ndarray]]:
    found = sorted(OUT.glob("input_*.f32"))
    if not found:
        sys.exit("no frames; run `frames` first")
    return [(p.stem.split("_")[1], np.fromfile(p, dtype=np.float32).reshape(1, 3, SIZE, SIZE))
            for p in found]


def timed(run, xs) -> tuple[list[np.ndarray], list[float]]:
    run(xs[0][1])  # warm: first runs compile and allocate
    outs, times = [], []
    for _, x in xs:
        t0 = time.perf_counter()
        outs.append(run(x))
        times.append(time.perf_counter() - t0)
    return outs, times


def bench(args) -> int:
    xs = inputs()
    results = {}

    model = load_torch("mps", half=True)

    def torch_run(x):
        with torch.no_grad():
            a = model(torch.from_numpy(x).to("mps").half())[-1].sigmoid()
        torch.mps.synchronize()
        return a.float().cpu().numpy()

    results["pytorch mps fp16"] = timed(torch_run, xs)
    reference = results["pytorch mps fp16"][0]
    for i, (name, _) in enumerate(xs):
        reference[i].astype(np.float32).tofile(OUT / f"torch_{name}.f32")
    del model
    torch.mps.empty_cache()

    import onnxruntime as ort
    providers = {
        "onnxruntime coreml": [("CoreMLExecutionProvider", {"ModelFormat": "MLProgram",
                                                           "MLComputeUnits": "ALL"}),
                               "CPUExecutionProvider"],
        "onnxruntime cpu": ["CPUExecutionProvider"],
    }
    for label, chosen in providers.items():
        if args.only and args.only not in label:
            continue
        t0 = time.perf_counter()
        try:
            session = ort.InferenceSession(str(MODEL), providers=chosen)
        except Exception as e:  # a provider refusing the graph is a result too
            print(f"{label}: could not load: {e}")
            continue
        print(f"{label}: session in {time.perf_counter() - t0:.1f} s, "
              f"providers {session.get_providers()}")
        results[label] = timed(lambda x: session.run(None, {"image": x})[0], xs)

    print(f"\n{'':22} {'s/frame':>8}  {'mean |Δα|':>10}  {'max |Δα|':>9}  {'>1/255':>8}")
    for label, (outs, times) in results.items():
        diffs = [np.abs(o.astype(np.float32) - r.astype(np.float32)) for o, r in zip(outs, reference)]
        mean = np.mean([d.mean() for d in diffs])
        worst = max(d.max() for d in diffs)
        off = np.mean([(d > 1 / 255).mean() for d in diffs])
        print(f"{label:22} {np.mean(times):8.2f}  {mean:10.5f}  {worst:9.4f}  {off:8.2%}")
    return 0


def pipe_frame(clip: pathlib.Path, lut: pathlib.Path | None, index: int):
    """Frame `index` as the matting stack sees it: (sRGB, linear), Rec.709."""
    cmd = [str(ROOT / "build" / "photoscoping"), str(clip), "--pipe", "--frame", str(index)]
    if lut:
        cmd += ["--lut", str(lut)]
    source = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    header = source.stdout.readline().split()
    w, h = int(header[1]), int(header[2])
    raw = source.stdout.read(w * h * 8)
    source.stdout.close()
    source.wait()
    rgba = np.frombuffer(raw, dtype=np.float16).reshape(h, w, 4)
    linear = rgba[..., :3].astype(np.float32) @ REC2020_TO_709.T
    return srgb_encode(linear).astype(np.float32), linear


STAGES = ["coarse", "tiled", "band", "subject"]


def stack(args) -> int:
    """The Python stack's stages on frames, saved for the native stack's to be
    compared with (photoscoping --matte, then stack-compare)."""
    from matting_bakeoff import BiRefNet
    from matting_stack import band_matting, ml_foreground, tiled_alpha
    OUT.mkdir(parents=True, exist_ok=True)
    model = BiRefNet("BiRefNet HR-matting @2048²", "ZhengPeng7/BiRefNet_HR-matting", (2048, 2048))
    model.load("mps")
    for index in args.at:
        srgb, linear = pipe_frame(args.clip, args.lut, index)
        t0 = time.perf_counter()
        coarse = model(srgb, linear, "mps")
        t1 = time.perf_counter()
        tiled = tiled_alpha(model, srgb, coarse, "mps")
        t2 = time.perf_counter()
        band = band_matting(srgb, linear, tiled)
        t3 = time.perf_counter()
        subject = ml_foreground(np.clip(linear, 0.0, None), band)
        t4 = time.perf_counter()
        for stage, data in (("coarse", coarse), ("tiled", tiled), ("band", band),
                            ("subject", subject)):
            data.astype(np.float32).tofile(OUT / f"py_{stage}_{index:06d}.f32")
        print(f"frame {index}: coarse {t1 - t0:.2f} s, tiled {t2 - t1:.2f} s, "
              f"band {t3 - t2:.2f} s, subject {t4 - t3:.2f} s")
    return 0


def stack_compare(args) -> int:
    print(f"{'':8} {'frame':>7}  {'mean |Δα|':>10}  {'max |Δα|':>9}  {'>1/255':>8}")
    for stage in STAGES:
        for py in sorted(OUT.glob(f"py_{stage}_*.f32")):
            frame = py.stem.split("_")[-1]
            native = OUT / f"native_{stage}_{frame}.f32"
            if not native.exists():
                continue
            d = np.abs(np.fromfile(native, dtype=np.float32) - np.fromfile(py, dtype=np.float32))
            print(f"{stage:8} {frame:>7}  {d.mean():10.5f}  {d.max():9.4f}  "
                  f"{(d > 1 / 255).mean():8.2%}")
    return 0


def compare(args) -> int:
    """The C++ side's alphas against PyTorch's, frame by frame."""
    rows = []
    for ref in sorted(OUT.glob("torch_*.f32")):
        frame = ref.stem.split("_")[1]
        want = np.fromfile(ref, dtype=np.float32)
        for got_path in sorted(OUT.glob(f"cpp_*_{frame}.f32")):
            got = np.fromfile(got_path, dtype=np.float32)
            d = np.abs(got - want)
            rows.append((got_path.stem.rsplit("_", 1)[0], frame, d.mean(), d.max(),
                         (d > 1 / 255).mean()))
    # The model's input as photoscoping prepares it natively (--dump-input),
    # against the frames prepared here.
    for native in sorted(OUT.glob("native_input_*.f32")):
        frame = native.stem.split("_")[-1]
        python = OUT / f"input_{frame}.f32"
        if not python.exists():
            continue
        d = np.abs(np.fromfile(native, dtype=np.float32) - np.fromfile(python, dtype=np.float32))
        print(f"input, native vs Python, frame {frame}: max |Δ| {d.max():.2e}, "
              f"mean {d.mean():.2e} (normalised units)")
    if not rows:
        sys.exit("nothing to compare; run `bench` and the C++ side first")
    print(f"{'':12} {'frame':>7}  {'mean |Δα|':>10}  {'max |Δα|':>9}  {'>1/255':>8}")
    for side, frame, mean, worst, off in rows:
        print(f"{side:12} {frame:>7}  {mean:10.5f}  {worst:9.4f}  {off:8.2%}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("export")
    p.add_argument("--opset", type=int, default=19)
    p.add_argument("--decompose", action="store_true",
                   help="export deformable convolution as plain ops")
    p.add_argument("--coreml", action="store_true",
                   help="rewrite the backbone so Core ML can take all of it")
    sub.add_parser("fp16")
    p = sub.add_parser("check")
    p.add_argument("--size", type=int, default=512)
    p.add_argument("--coreml", action="store_true", help="with the backbone rewritten too")
    p = sub.add_parser("frames")
    p.add_argument("clip", type=pathlib.Path)
    p.add_argument("--lut", type=pathlib.Path)
    p.add_argument("--at", type=int, nargs="+", default=[100, 300, 500])
    sub.add_parser("compare")
    p = sub.add_parser("stack")
    p.add_argument("clip", type=pathlib.Path)
    p.add_argument("--lut", type=pathlib.Path)
    p.add_argument("--at", type=int, nargs="+", default=[100, 300, 500])
    sub.add_parser("stack-compare")
    p = sub.add_parser("bench")
    p.add_argument("--only", help="only the ONNX Runtime providers whose name contains this")
    args = parser.parse_args()
    return {"export": export, "check": check, "fp16": fp16, "frames": frames, "bench": bench,
            "compare": compare, "stack": stack,
            "stack-compare": stack_compare}[args.command](args)


if __name__ == "__main__":
    raise SystemExit(main())
