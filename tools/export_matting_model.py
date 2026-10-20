#!/usr/bin/env python3
"""Exports the matting model for photoscoping's native stack.

BiRefNet HR-matting, as ONNX in half precision, at the two sizes the stack
runs it: 2048x2048 for the whole frame, scaled down, and 2048x1152 for the
full-resolution tiles. src/matting/ loads them through ONNX Runtime, on Core
ML's GPU on the Mac.

Made portable and Core ML-ready on the way out, all of it checked identical
to the original model in PyTorch (tools/onnx_spike.py check --coreml):

  - torchvision's deformable convolution, which no runtime here has, rebuilt
    from grid_sample and 1x1 convolutions;
  - the Swin backbone's 6-D window split, indexed q/k/v and looked-up
    position bias reshaped into what Core ML takes, without which it cuts
    the model into 73 separately compiled pieces;
  - fixed-size shape arithmetic folded to constants before the conversion to
    half precision, and the converter's mistyped casts put right;
  - stamped with a hash of its contents as COREML_CACHE_KEY. Without one,
    ONNX Runtime keys Core ML's compiled model on the path the model was
    opened by, so the same model reached by another path -- relative or
    absolute, the checkout or the .app -- compiles again, minutes and
    gigabytes each time; and a changed model at the same path reuses the old
    compile, which it never notices.

The export traces a full fp32 forward pass on the CPU, about 26 GB at
2048x2048; run it under tools/guarded.py.

usage: .venv/bin/python tools/export_matting_model.py [--out models] [--size WxH ...]
       .venv/bin/python tools/export_matting_model.py --stamp MODEL.onnx ...
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import time

import torch
import torch.nn.functional as F

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from matting_bakeoff import PINS  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
REPO = "ZhengPeng7/BiRefNet_HR-matting"
# The whole frame, and a tile (matting_stack.TILE_W, TILE_H).
SIZES = [(2048, 2048), (2048, 1152)]


def model_name(width: int, height: int) -> str:
    return f"birefnet_hr_matting_{width}x{height}_fp16.onnx"


def portable_deform_conv2d(x, offset, weight, bias, padding, mask, stride, kernel_size):
    """torchvision's deform_conv2d out of operations every runtime has.

    Each kernel tap samples the input bilinearly at its own offset position --
    grid_sample, with zeros outside, which is how torchvision treats samples
    off the edge -- is scaled by the tap's mask, and goes through that tap's
    slice of the weights as a 1x1 convolution. Summed over taps, that is the
    deformable convolution, one tap at a time so a 7x7 kernel never builds a
    49-fold copy of its input.

    Covers what BiRefNet uses: one offset group, one weight group, no
    dilation.
    """
    # Sizes from the layer, as plain numbers: read off a tensor while being
    # traced for export, they come back as tensors.
    h, w = x.shape[-2:]
    kh, kw = kernel_size
    sh, sw = stride
    ph, pw = (padding, padding) if isinstance(padding, int) else padding
    ho, wo = offset.shape[-2:]
    base_y = (torch.arange(ho, dtype=x.dtype, device=x.device) * sh - ph).view(1, ho, 1)
    base_x = (torch.arange(wo, dtype=x.dtype, device=x.device) * sw - pw).view(1, 1, wo)
    out = None
    for k in range(kh * kw):
        i, j = divmod(k, kw)
        # torchvision's layout: a (dy, dx) pair per tap.
        py = base_y + i + offset[:, 2 * k]
        px = base_x + j + offset[:, 2 * k + 1]
        grid = torch.stack((2.0 * px / (w - 1) - 1.0, 2.0 * py / (h - 1) - 1.0), -1)
        sample = F.grid_sample(x, grid, mode="bilinear", padding_mode="zeros",
                               align_corners=True) * mask[:, k:k + 1]
        term = F.conv2d(sample, weight[:, :, i:i + 1, j:j + 1])
        out = term if out is None else out + term
    return out if bias is None else out + bias.view(1, -1, 1, 1)


def make_portable(model) -> int:
    """Swaps every deformable convolution in the model for the portable one."""
    swapped = 0
    for module in model.modules():
        if type(module).__name__ != "DeformableConv2d":
            continue

        def forward(x, m=module):
            offset = m.offset_conv(x)
            modulator = 2.0 * torch.sigmoid(m.modulator_conv(x))
            return portable_deform_conv2d(x, offset, m.regular_conv.weight, m.regular_conv.bias,
                                          m.padding, modulator, m.stride,
                                          m.regular_conv.kernel_size)

        module.forward = forward
        swapped += 1
    return swapped


def make_coreml_friendly(model) -> int:
    """Rewrites the Swin backbone's attention so Core ML can take all of it.

    Core ML stops at five dimensions and does not do this graph's Gathers, and
    the backbone uses both in every block: the window split is a 6-D reshape
    and transpose, the query, key and value are picked out of a stacked tensor
    by index, and the position bias is looked up from a table. Each one cut
    the model into another separately compiled Core ML piece -- 73 of them,
    with the CPU in between. Rewritten, for one frame at a time:

      - the window split and its reverse drop the batch axis, which is one, and
        fit in five dimensions;
      - query, key and value are split off the projection rather than indexed;
      - the position bias, which depends on nothing but the weights, is looked
        up once here and stored as a constant.

    The same arithmetic in another shape; `check --coreml` compares it.
    """
    import sys as _sys
    remote = _sys.modules[type(model).__module__]

    def window_partition(x, ws):
        _, h, w, c = x.shape
        x = x.reshape(h // ws, ws, w // ws, ws, c)
        return x.permute(0, 2, 1, 3, 4).reshape(-1, ws, ws, c)

    def window_reverse(windows, ws, h, w):
        c = windows.shape[-1]
        x = windows.reshape(h // ws, w // ws, ws, ws, c)
        return x.permute(0, 2, 1, 3, 4).reshape(1, h, w, c)

    remote.window_partition = window_partition
    remote.window_reverse = window_reverse

    rewritten = 0
    for module in model.modules():
        if type(module).__name__ != "WindowAttention":
            continue
        n = module.window_size[0] * module.window_size[1]
        with torch.no_grad():
            bias = module.relative_position_bias_table[module.relative_position_index.view(-1)]
            bias = bias.view(n, n, -1).permute(2, 0, 1).unsqueeze(0).contiguous()
        module.register_buffer("bias_constant", bias, persistent=False)

        def forward(x, mask=None, m=module):
            b, n_, c = x.shape
            heads = m.num_heads
            q, k, v = m.qkv(x).split(c, dim=-1)
            q = q.reshape(b, n_, heads, c // heads).transpose(1, 2) * m.scale
            k = k.reshape(b, n_, heads, c // heads).transpose(1, 2)
            v = v.reshape(b, n_, heads, c // heads).transpose(1, 2)
            attn = q @ k.transpose(-2, -1) + m.bias_constant
            if mask is not None:
                nw = mask.shape[0]
                attn = attn.view(b // nw, nw, heads, n_, n_) + mask.unsqueeze(1).unsqueeze(0)
                attn = attn.view(-1, heads, n_, n_)
            attn = m.softmax(attn)
            out = (attn @ v).transpose(1, 2).reshape(b, n_, c)
            return m.proj_drop(m.proj(out))

        module.forward = forward
        rewritten += 1
    return rewritten


def load_torch(device: str, half: bool):
    from transformers import AutoModelForImageSegmentation
    model = AutoModelForImageSegmentation.from_pretrained(REPO, revision=PINS[REPO],
                                                          trust_remote_code=True)
    # The checkpoint loads as half; fp32 has to be asked for.
    model.eval().to(device)
    return model.half() if half else model.float()


class Head(torch.nn.Module):
    """The model as the stack uses it: last output, through a sigmoid."""

    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, x):
        return self.model(x)[-1].sigmoid()


def linears_as_gemm(model) -> int:
    """Every Linear applied to its input flattened to two dimensions.

    Swin's linear layers act on (windows, tokens, channels), which exports as
    MatMul against the weight transposed. ONNX Runtime turns that into Core
    ML's linear by transposing the weight back -- and writes the transposed
    copy into Core ML's program as text, not into its weight file: four
    gigabytes of hex for 200 MB of weights, which is most of the minutes Core
    ML spends compiling, and most of the cache it leaves. Flattened, the same
    layer exports as Gemm with the weight already the way round linear takes
    it. The same arithmetic, in another shape."""
    swapped = 0
    for module in model.modules():
        if not isinstance(module, torch.nn.Linear):
            continue

        def forward(x, m=module):
            if x.dim() == 2:
                return F.linear(x, m.weight, m.bias)
            lead = x.shape[:-1]
            return F.linear(x.reshape(-1, x.shape[-1]), m.weight, m.bias).reshape(
                *lead, m.out_features)

        module.forward = forward
        swapped += 1
    return swapped


def stamp_cache_key(path: pathlib.Path) -> str:
    """Sets COREML_CACHE_KEY to a hash of the model as it is without one.
    Hex only: ONNX Runtime wants the key alphanumeric."""
    import hashlib
    import onnx
    model = onnx.load(str(path))
    kept = [p for p in model.metadata_props if p.key != "COREML_CACHE_KEY"]
    del model.metadata_props[:]
    model.metadata_props.extend(kept)
    key = hashlib.sha256(model.SerializeToString()).hexdigest()[:40]
    entry = model.metadata_props.add()
    entry.key, entry.value = "COREML_CACHE_KEY", key
    onnx.save(model, str(path))
    return key


def export_fp16(width: int, height: int, out: pathlib.Path, gemm: bool = True) -> pathlib.Path:
    import onnx
    import onnxruntime as ort
    from onnxconverter_common import float16

    out.mkdir(parents=True, exist_ok=True)
    final = out / model_name(width, height)
    traced = out / f".traced_{width}x{height}.onnx"
    folded = out / f".folded_{width}x{height}.onnx"

    model = load_torch("cpu", half=False)
    make_portable(model)
    make_coreml_friendly(model)
    if gemm:
        linears_as_gemm(model)
    head = Head(model)
    # Without this the trace runs with autograd on and keeps every activation
    # for a backward pass that never comes: past 45 GB at 2048².
    head.requires_grad_(False)
    t0 = time.perf_counter()
    with torch.no_grad():
        torch.onnx.export(head, (torch.zeros(1, 3, height, width),), str(traced),
                          input_names=["image"], output_names=["alpha"], opset_version=19,
                          dynamo=False)
    del head, model

    # Folded first. The window and padding arithmetic is on fixed sizes, so
    # it reduces to constants; left in, the converter makes it half too, and
    # Core ML refuses the one-element half tensors that come out of it.
    options = ort.SessionOptions()
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
    options.optimized_model_filepath = str(folded)
    ort.InferenceSession(str(traced), options, providers=["CPUExecutionProvider"])
    converted = float16.convert_float_to_float16(onnx.load(str(folded)), keep_io_types=True)
    # The converter declares the graph's tensors half but leaves the graph's
    # own casts to float as they were, so those casts no longer produce the
    # type declared for their output and the model will not load. Each is
    # made to produce what was declared. The casts the converter inserts
    # itself, around operations it keeps in fp32, declare float and stay.
    declared = {v.name: v.type.tensor_type.elem_type for v in converted.graph.value_info}
    for node in converted.graph.node:
        if node.op_type != "Cast" or declared.get(node.output[0]) != onnx.TensorProto.FLOAT16:
            continue
        for attr in node.attribute:
            if attr.name == "to" and attr.i == onnx.TensorProto.FLOAT:
                attr.i = onnx.TensorProto.FLOAT16
    onnx.save(converted, str(final))
    traced.unlink()
    folded.unlink()
    stamp_cache_key(final)
    print(f"wrote {final} ({final.stat().st_size / 1e6:.0f} MB) in "
          f"{time.perf_counter() - t0:.0f} s", flush=True)
    return final


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "models")
    parser.add_argument("--size", action="append", metavar="WxH",
                        help="export only these sizes (default: both the stack uses)")
    parser.add_argument("--matmul", action="store_true",
                        help="leave the linear layers as MatMul, for comparing")
    parser.add_argument("--stamp", nargs="+", type=pathlib.Path, metavar="MODEL",
                        help="only stamp existing models with their cache key")
    args = parser.parse_args()
    if args.stamp:
        for path in args.stamp:
            print(f"{path}: COREML_CACHE_KEY {stamp_cache_key(path)}")
        return 0
    sizes = [tuple(map(int, s.split("x"))) for s in args.size] if args.size else SIZES
    for width, height in sizes:
        export_fp16(width, height, args.out, gemm=not args.matmul)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
