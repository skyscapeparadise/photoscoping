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

photoscoping is free software released under the
[GNU Lesser General Public License v2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html)
or later.

Copyright © 2026 Rain Multimedia LLC
