"""The subject-separation stack, shared by the clip renderer and experiments.

  1. BiRefNet HR-matting on the whole frame at 2048² decides what is subject.
  2. BiRefNet again on 2048x1152 tiles at the frame's own resolution, blended
     in where the edge is soft, so strands keep their real width.
  3. Closed-form matting (Levin et al.) in a trimap band at full resolution,
     where subject and wall differ enough in colour for it to mean anything --
     the same shape as Photoshop's Refine Hair.
  4. Multilevel foreground estimation (Germer et al.) removes the wall's
     share from edge colours, giving the premultiplied subject.

BiRefNet and pymatting are MIT; numba (under pymatting) is BSD.
"""

from __future__ import annotations

import numpy as np
import torch
import torch.nn.functional as F
from scipy import ndimage

from matting_bakeoff import BiRefNet


def fill_plate(linear: np.ndarray, known: np.ndarray) -> np.ndarray:
    """Fills every pixel from the nearest `known` ones, by push-pull: average
    down a pyramid, then fill each level's holes from the level below. A
    hidden pixel takes its *nearest* known neighbours rather than a wide
    average, so local detail -- a shadow the subject casts right beside
    itself, the colour of the hair next to its stray strands -- is kept."""
    h, w = known.shape
    img = torch.from_numpy(linear).permute(2, 0, 1)[None]
    mask = torch.from_numpy(known.astype(np.float32))[None, None]
    # Half resolution is plenty for a plate and keeps the pyramid cheap.
    sums = [F.avg_pool2d(img * mask, 2)]
    weights = [F.avg_pool2d(mask, 2)]
    while min(sums[-1].shape[-2:]) > 4:
        sums.append(F.avg_pool2d(sums[-1], 2, ceil_mode=True))
        weights.append(F.avg_pool2d(weights[-1], 2, ceil_mode=True))
    colour = sums[-1] / weights[-1].clamp_min(1e-6)
    for s, wt in zip(reversed(sums[:-1]), reversed(weights[:-1])):
        up = F.interpolate(colour, size=s.shape[-2:], mode="bilinear", align_corners=False)
        # Mostly-known cells keep their own value; hidden ones take the
        # coarser level's.
        trust = (wt * 4).clamp(0, 1)
        colour = (s / wt.clamp_min(1e-6)) * trust + up * (1 - trust)
    plate = F.interpolate(colour, size=(h, w), mode="bilinear", align_corners=False)
    return plate[0].permute(1, 2, 0).numpy()


def premultiplied_foreground(linear: np.ndarray, alpha: np.ndarray) -> np.ndarray:
    """aF from I = aF + (1-a)B: the subject's own light with the wall's share
    removed, which is also exactly the composite over black.

    B (the wall) and F (the subject) are both estimated locally: B from the
    pixels the matte calls background, F from the ones it calls solid. In the
    edge band each pixel's coverage is then solved as the mix of the two that
    best explains it, and allowed to raise the matte a little. That matters
    where the matte undershoots a subject much darker than the wall -- dark
    hair -- because subtracting a bright wall's share at the matte's coverage
    leaves nothing, and the hair goes black. Where F and B are too alike in
    colour to tell apart (a light grey hoodie on white), the solve is
    meaningless and the matte stands.

    Solved without dividing by alpha, so thin edges cannot amplify noise.
    """
    luma = linear @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    background = alpha < 0.02
    # Wall is background that looks like wall: stray hair strands outside the
    # matte are background to it, but nearly black.
    floor = 0.35 * np.median(luma[background]) if background.any() else 0.0
    wall = fill_plate(linear, background & (luma > floor))
    subject = fill_plate(linear, alpha > 0.95)

    d = subject - wall
    separation = np.sqrt((d * d).sum(-1))
    solved = np.clip(((linear - wall) * d).sum(-1) / np.maximum(separation ** 2, 1e-6), 0.0, 1.0)
    # Per-pixel, the solve carries the sensor's noise straight into the edge;
    # a small blur takes it out.
    t = torch.from_numpy(solved)[None, None]
    for _ in range(2):
        t = F.avg_pool2d(F.pad(t, (2, 2, 2, 2), mode="replicate"), 5, stride=1)
    solved = t[0, 0].numpy()
    confidence = np.clip(separation / 0.15, 0.0, 1.0)
    edge = (alpha > 0.02) & (alpha < 0.98)
    # The solve may only *raise* the matte, and not by much: it is there to
    # catch dark hair the matte undershoots. Lowering would trim the motion
    # blur BiRefNet keeps, which is the reason it won.
    raise_by = np.clip(solved - alpha, 0.0, 0.3) * confidence
    a = np.where(edge, alpha + raise_by, alpha)[..., None]

    fg = np.clip(linear - (1.0 - a) * wall, 0.0, linear)
    # aF cannot exceed alpha times a plausible subject brightness, so where
    # alpha is ~0 the result is black rather than the wall's texture against
    # a smooth estimate of it.
    brightest = np.maximum(wall.max(-1, keepdims=True) * 1.25, 1.0)
    return np.minimum(fg, a * brightest)


# Tiles are 16:9 at the model's 2048-wide input: 1:1 source pixels, and four
# of them cover a UHD frame where 2048² squares need six.
TILE_W, TILE_H = 2048, 1152


def smoothstep(x: np.ndarray, lo: float, hi: float) -> np.ndarray:
    t = np.clip((x - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def guided_hair(srgb: np.ndarray, alpha: np.ndarray, radius: int = 4,
                eps: float = 1e-3) -> np.ndarray:
    """Re-derives alpha in the soft band (hair, blur) with a guided filter
    (He et al.): in every small window, alpha is refitted as a linear
    function of the image. A strand half as dark as its surroundings gets
    about half the coverage, rather than the model's near yes/no call that
    can flip between frames. Local, linear and closed-form -- no trimap, no
    global solve -- so it moves smoothly when the footage does.

    `eps` sets how strongly it follows the image: smaller tracks finer detail.
    Solid edges (alpha already near 0 or 1) are left exactly as they are."""
    import cv2

    guide = (srgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)).astype(np.float32)
    a = alpha.astype(np.float32)
    k = (2 * radius + 1, 2 * radius + 1)
    box = lambda x: cv2.boxFilter(x, -1, k, borderType=cv2.BORDER_REFLECT)  # noqa: E731
    mean_i, mean_a = box(guide), box(a)
    cov = box(guide * a) - mean_i * mean_a
    var = box(guide * guide) - mean_i * mean_i
    slope = cov / (var + eps)
    offset = mean_a - slope * mean_i
    refined = np.clip(box(slope) * guide + box(offset), 0.0, 1.0)

    soft = ((alpha > 0.02) & (alpha < 0.98)).astype(np.float32)
    weight = np.clip(ndimage.gaussian_filter(soft, 3) * 3, 0.0, 1.0)
    return (weight * refined + (1 - weight) * alpha).astype(np.float32)


def tile_starts(length: int, tile: int, min_overlap: int = 128) -> list[int]:
    """The fewest evenly spaced tiles covering `length` that still overlap by
    `min_overlap`, enough to feather across. UHD takes 2 x 2."""
    if length <= tile:
        return [0]
    n = 1 + int(np.ceil((length - tile) / (tile - min_overlap)))
    return [round(i * (length - tile) / (n - 1)) for i in range(n)]


def tiled_alpha(model: BiRefNet, srgb: np.ndarray, coarse: np.ndarray, device: str) -> np.ndarray:
    """BiRefNet on overlapping full-resolution tiles, feathered together, then
    used only in the soft band of the coarse matte. A tile sees a part of the
    subject without the whole, so it is trusted for edge detail, not for
    deciding what the subject is."""
    h, w = coarse.shape
    # The same weights at the tile's own size, so tiles are never resampled.
    tiler = BiRefNet(model.name, model.repo, (TILE_W, TILE_H))
    tiler.model = model.model
    xs, ys = tile_starts(w, TILE_W), tile_starts(h, TILE_H)
    acc = np.zeros((h, w), np.float32)
    weight = np.zeros((h, w), np.float32)
    ramp_y = np.minimum(np.arange(TILE_H) + 1, TILE_H - np.arange(TILE_H)).astype(np.float32)
    ramp_x = np.minimum(np.arange(TILE_W) + 1, TILE_W - np.arange(TILE_W)).astype(np.float32)
    feather = np.minimum.outer(ramp_y, ramp_x) ** 2
    for y in ys:
        for x in xs:
            tile = srgb[y:y + TILE_H, x:x + TILE_W]
            th, tw = tile.shape[:2]
            if th < TILE_H or tw < TILE_W:  # frames smaller than a tile: pad
                tile = np.pad(tile, ((0, TILE_H - th), (0, TILE_W - tw), (0, 0)), mode="edge")
            alpha = tiler(tile, tile, device)[:th, :tw]
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


def refine(model: BiRefNet, srgb: np.ndarray, linear: np.ndarray,
           device: str) -> tuple[np.ndarray, np.ndarray]:
    """The whole stack: (alpha, premultiplied subject in linear light)."""
    coarse = model(srgb, linear, device)
    tiled = tiled_alpha(model, srgb, coarse, device)
    alpha = band_matting(srgb, linear, tiled)
    return alpha, ml_foreground(linear, alpha)
