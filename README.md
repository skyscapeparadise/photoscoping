# photoscoping

An app for photoscoping live action video footage.

Native C++20 on SDL3 (SDL_GPU) with a bespoke widget set, and an LGPL FFmpeg
for reading camera originals and intermediates (ProRes, DNxHD/DNxHR,
CineForm, H.264/HEVC in MOV, MXF and MP4) and writing exports.

## Building

    ./build.sh            # Release build, packaged to build/photoscoping.app
    ./build.sh --debug    # Debug build
    ./clean.sh            # remove build/ (./clean.sh --all also drops vendor builds)

The first build compiles pinned versions of SDL3, SDL3_image, freetype,
HarfBuzz + SDL3_ttf, libvpx and FFmpeg from source into `vendor/`, which takes several
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

Fixes made by hand go on top of the matte rather than into it, so changing
one never means matting again. They are keyframed: set at the frames that
need them, interpolated between, held before the first key and after the
last, and kept in `renders/<clip>_fixes.json`. Command-Z and Shift-Command-Z
undo and redo them.

The garbage tool draws garbage mattes -- shapes the subject is always inside;
outside them the matte is forced to background. Click to place points, and
click the first point, double-click or press Return to close the shape. Drag
a point to move it, or an edge to move the whole shape; either keys the
current frame. Option-click an edge to add a point there, or a point to take
it out -- on every key, so the shape still tweens. [ and ] feather the selected
shape: the matte fades out over that many pixels outside it instead of
stopping at its edge. Backspace removes the selected shape's key on this frame (or
the shape, if it has no other), Shift-Backspace the whole shape, and Escape
abandons one being drawn. Their keys are marked along the ruler.

The paint tool paints coverage out of the matte, or back into it. Drag to
paint; Option while starting a stroke paints the other way, X swaps which way
is the default, and [ and ] shrink and grow the brush. A stroke is keyframed
like a shape but is live only from its first key to its last, so it never
leaks onto frames it was not meant for. Command-drag a stroke to move it on
this frame, which keys it there; K keys the selected stroke on this frame as
it stands, carrying it further; Backspace and Shift-Backspace delete as with
shapes. Strokes apply in the order they were painted, and inside the garbage
mattes.

The re-matte tool draws boxes the matting stack looks at again, closer. Drag
on the footage to draw one; drag inside it to move it, or an edge or corner
to resize it. Like a stroke, a box is live only from its first key to its
last: K keys the selected box on this frame, and Backspace and
Shift-Backspace delete. A box drawn lilac is waiting; the pill under the
inspector re-mattes every frame that has one, in the background, and each
turns white as it is done. The card says how much closer the selected box
looks -- its crop is magnified to fill the model, up to four times.

Re-matting crops around the box, runs the model on the crop, trusts that
closer look only where the edge is soft (as the tiled stage does), refines it
with closed-form matting, and writes the box as a 16-bit PNG in
`renders/<clip>_rematte/`, named for its frame and the box to the pixel. The
viewer blends it into the matte, feathered in from the box's edge, before
garbage mattes and paint apply. Moving a box only re-mattes the frames it
moved on, and each run clears away the patches no box wants any more. `--rematte-clip DIR` runs it headless.

A clean plate, optional, makes a locked-off shot's edges and colour better.
Clip > Start Clean Plate Here and End Clean Plate Here (Command-[ and
Command-]) mark a stretch of the clip where the set is empty; up to sixteen of
its frames are averaged into `renders/<clip>_plate_<first>_<last>.png` in the
background, and the frames show as a faint band on the ruler. Each frame then
relights the plate from its own visible wall -- following a strobe's level and
the subject's soft shadows -- and the matte is refined with it: around the
edge, what matches the wall is solved as wall, and the wall's true colour says
how far the edge solve can be trusted. Exports and the subject view work out
the subject's colour from the wall exactly, so a dark, motion-blurred edge
loses the wall's grey halo. The refined mattes go in a folder of their own,
named for the plate, and the stack's own are untouched -- Clear Clean Plate
shows them again. The pill refines a matted clip; matting a clip with a plate
set refines as it goes.

Plate Corrects the Matte (Clip menu) lets the plate overrule the model too.
Wall the matte kept as subject goes, if it shows the plate's own plaster grain
in place, reached from the open wall; a hole in the subject is filled only if
its colour is one no shadow on the wall could make. A grey subject on a grey
wall is beyond it either way -- for those, paint. It stands down on frames too
far out of focus for the wall's grain to show, and on close-ups with no open
wall.

Light maps are the backdrop read as a lighting record: crops of it, exported
frame by frame to drive emissive lights in Blender. The light tool draws
them: drag on the backdrop for a crop square to the frame, then pull its
corners onto the board -- an angled board is straightened on export -- or
drag inside to move it. Like a garbage matte a crop holds its place, and
keys on the frame it is changed on; K keys it, Backspace and Shift-Backspace
delete. The card shows the selected crop's Hue, Saturation and Lightness, with
Colorize, as Photoshop has them (drag, or double-click to reset), the size
it will come out at, and a preview of it as it will export -- straightened,
the performer filled in, the colour adjusted -- made on a thread of its own
and following the crop, its colour and the frame as they change. Linear light
is previewed at the frame's own exposure; the export's spans the clip. File > Export Light Maps writes every crop at once into a
chosen folder -- "clip light 1.mp4" and so on, each with a JSON sidecar -- as
HEVC, ProRes 422, 422 LT or 4444, H.264, VP9, or PNG or EXR sequences. Its
settings: Linear Light (the default) or Graded Look; Fill the Performer's
Shadow; Size: the crop's own, a preset or a custom long side.

Where the performer passes in front of a crop, the backdrop behind them is
filled in: a smooth surface fitted to the backdrop around the crop (with a
clean plate, the plate under the frame's own light), matched to the real
backdrop at the edge and blended in by the matte's softness, so a light driven
by it eases rather than steps as they cross. Linear light in a video or PNG
is scaled by one exposure for every crop and frame -- found by a first pass --
so the brightest light sits just under white; the sidecar's
`emission_strength` restores it in Blender. EXR keeps the light as it is,
unclipped. Their shadow is kept by default, since it is light that did not
reach the backdrop; filling it works well with a clean plate, and less
reliably without one, and assumes the light across the backdrop only changes
smoothly -- a moving spot or a projection can be mistaken for shadow.
`--light-maps DIR --light-format ID --mattes DIR` runs it headless.

File > Export Subject writes the matted subject with its alpha: ProRes 4444
(Command-E, the default), VP9 in WebM, or a folder of 16-bit RGBA PNGs. File >
Export Matte writes the matte alone, white on black: ProRes 422, ProRes 422
LT, HEVC (10-bit) or H.264. Exporting needs every frame matted; it runs in the
background, with its progress on the card and the pill to stop it. Each frame
is the matte with its re-matte patches blended in -- made on the way, if they
were not -- and the garbage mattes and paint applied, at full resolution. The
subject's colour is estimated free of the wall's spill and written with
straight alpha, encoded for Rec.709 the way the footage was read, so where
the subject is solid it comes out as it went in. Everything is tagged Rec.709,
video range. HEVC and H.264 use the Mac's own encoders; ProRes is FFmpeg's,
VP9 libvpx's. A stopped export leaves only a `.partial` file.
`--export-clip PATH --export-format ID --mattes DIR` runs it headless
(`--range A:B` for part of the clip). `--plate-clip DIR` makes the plate
headless.

The viewer shows a matte four ways: the footage, the matte, the matted subject
over a checkerboard, or the footage washed purple wherever the matte drops it.
The pills under the inspector switch between them, as do the keys 1-4, and a
lilac line along the top of the ruler marks the frames that have a matte. The
subject view of a clip the app matted works out the subject's colour live, as
an export does -- the matte with its patches and fixes, the wall's spill taken
out of the edges -- at half size on a thread of its own, once the frame is
still; while playing, the footage stands in for it. Where the subject is
solid it shows the footage at full size. A clip matted by
`tools/matte_clip.py` (the Python stack, still there for experiments) shows
the `<clip>_comp.mp4` it renders instead.

The stack's full-resolution tiles are only run where the subject's edge is:
a tile of open wall or solid subject would be thrown away, so it is skipped,
which leaves the matte the same to the bit. At 8K that holds a frame to about
26 seconds on an M1 Max (10.5 GB at most); the matting run's log gives each
frame's time by stage.

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
