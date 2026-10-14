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

Space plays and pauses, left/right step a frame, home returns to the start,
and tab shows or hides the inspector. The window resizes freely: the viewer
takes what the inspector and timeline leave, the inspector can be dragged
wider or narrower by its left edge, and it folds away when the window gets
too narrow for both. `--window WxH` opens at a given size and
`--screenshot FILE` renders the window once to a PNG and exits.
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
