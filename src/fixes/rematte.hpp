#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fixes/fixes.hpp"

// Where a re-matte box is, in the frame's pixels, and what the stack looks at
// to matte it again: shared by the run that makes the patches and the viewer
// that shows them, so both agree on which patch is which.
//
// A patch is named for its frame and the box it covers, to the pixel. Move a
// box and the frames it moved on want new patches, and the old ones are
// simply never asked for again -- nothing has to notice that they went stale.
namespace ps::fixes {

struct PixelRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // x1, y1 exclusive

    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    friend bool operator==(const PixelRect& a, const PixelRect& b) {
        return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
    }
};

// Smaller than this either way, a box is not re-matted.
constexpr int kMinRematte = 16;

// The box at `frame`, rounded to whole pixels and held inside the frame.
PixelRect rematte_rect(const Rematte& box, std::int64_t frame, int frame_w, int frame_h);

// What the stack crops around a box, and which of its two models it runs the
// crop through. The crop is the box with room around it -- the model needs
// to see what the edge is an edge of -- widened to the model's shape, and to
// no less than a quarter of the model's size, past which magnifying further
// only invents detail. `detail` is how many model pixels each frame pixel
// gets: the point of looking again.
struct RematteCrop {
    PixelRect crop;
    bool wide = false;  // the 2048x1152 model, not the 2048x2048 one
    int model_w = 0, model_h = 0;
    float detail = 1.0f;
};
RematteCrop rematte_crop(const PixelRect& box, int frame_w, int frame_h);

// How far in from its edge a patch fades in over the matte, in pixels.
int rematte_feather(const PixelRect& box);
// The patch's weight at (x, y) inside the box, 0 at its edge to 1 a
// feather in.
float rematte_weight(const PixelRect& box, int x, int y);

// "000123_x0_y0_x1_y1.png".
std::string patch_name(std::int64_t frame, const PixelRect& box);

struct PatchJob {
    std::int64_t frame = 0;
    PixelRect box;
};
// Every patch the boxes want across the clip, by frame and then position,
// once each.
std::vector<PatchJob> rematte_jobs(const Fixes& fixes, std::int64_t frame_count, int frame_w,
                                   int frame_h);

}  // namespace ps::fixes
