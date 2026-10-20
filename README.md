# photoscoping

An app for photoscoping live action video footage.

Native C++20 on SDL3 (SDL_GPU) with a bespoke widget set, and an LGPL,
decode-only FFmpeg for reading camera originals and intermediates (ProRes,
DNxHD/DNxHR, CineForm, H.264/HEVC in MOV, MXF and MP4).

## Building

    ./build.sh            # Release build, packaged to build/photoscoping.app
    ./build.sh --debug    # Debug build
    ./clean.sh            # remove build/ (./clean.sh --all also drops vendor builds)

The first build compiles pinned versions of SDL3, SDL3_image, freetype,
HarfBuzz + SDL3_ttf and FFmpeg from source into `vendor/`, which takes several
minutes. Later builds skip them.

## Running

    ./build/photoscoping clip.mov --lut CinemaGamut_CanonLog3-to-BT709_WideDR_65_FF_Ver.2.0.cube

A clip can also be opened by dropping it on the window or with File > Open
(Command-O); dropping or opening a `.cube` puts the clip through that LUT
instead. Footage and mattes decode on threads of their own, so seeking never
stalls the window.

The viewer zooms and pans. Pinch, or Command, Control or Option with the
scroll wheel, zooms about the pointer; two-finger scrolling pans, as does
dragging with the navigate tool, with Space held, or with the middle button.
Command-0 fits the frame, Command-1 shows it at actual pixels, Command-= and
Command-- zoom in and out, and a double-click with the navigate tool goes
between fit and actual pixels. Past two screen pixels an image pixel, the
footage is shown unfiltered, so single pixels read as squares.

Space plays and pauses (on release, so a hold can pan instead), left/right
step a frame, home returns to the start,
and tab shows or hides the inspector. The window resizes freely: the viewer
takes what the inspector and timeline leave, the inspector can be dragged
wider or narrower by its left edge, and it folds away when the window gets
too narrow for both. `--window WxH` opens at a given size and
`--screenshot FILE` renders the window once to a PNG and exits.
The pill above the view buttons mattes the clip, natively: the app runs
itself headless (`--matte-clip`) in the background, and the ruler fills in as
each frame's matte lands in `renders/<clip>_mattes/` as a 16-bit PNG. Stopping
it, or closing the window, keeps every finished frame; resuming picks up from
there. Its output goes to `renders/<clip>_matte.log`.

The viewer shows a matte four ways: the footage, the matte, the matted subject
over a checkerboard, or the footage washed purple wherever the matte drops it.
The pills under the inspector switch between them, as do the keys 1-4, and a
lilac line along the top of the ruler marks the frames that have a matte. The
subject view needs the subject rendered, which `tools/matte_clip.py` (the
Python stack, still there for experiments) does as `<clip>_comp.mp4`.

The matting stack runs through ONNX Runtime -- on the GPU through Core ML, on
the Mac -- with two models `tools/export_matting_model.py` writes into
`models/`. The first run on a machine compiles them for Core ML, which takes a
few seconds and is cached under `~/Library/Caches`. `--matte FILE` runs a
single frame through the stack headless, stage by stage, and with `--bench N`
times N frames.

`--probe` prints the working-space values of a frame instead of opening a
window, and `--bench N` times decode and ingest over N frames.

Each frame is converted into one working space: linear light, Rec.2020, with
1.0 at reference white (203 nits for HDR). The input LUT sees the file's
signal exactly as recorded, decoded to full-range R'G'B' using the file's own
matrix and range tags. Canon's LUTs describe their own output in their headers
and file names, so SDR (Wide DR) and HDR (PQ, HLG) LUTs are recognised
without any setup.

photoscoping is free software released under the
[GNU Lesser General Public License v2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html)
or later.

Copyright © 2026 Rain Multimedia LLC
