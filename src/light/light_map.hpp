#pragma once

#include <cstdint>
#include <vector>

#include "fixes/fixes.hpp"

// Light maps: the backdrop as photoscoping's lighting record. Each is a crop
// of the frame -- four corners on the backdrop -- straightened to a rectangle
// and exported frame by frame, to drive an emissive light in Blender.
//
// The backdrop is a measurement, so a light map is the light as it fell, not
// an image of the set: where the performer stands in front of it, the backdrop
// behind them is filled in from what surrounds them (or from the clean plate),
// blended in by the matte's own softness and matched to the real backdrop at
// its edge, so a light driven by it never steps as they cross.
namespace ps::light {

// A projective map from the output rectangle, (0, 0) to (width, height), to
// the four corners in the frame: top left, top right, bottom right, bottom
// left.
struct Homography {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    void map(double u, double v, double* x, double* y) const;
};
Homography quad_homography(const std::vector<fixes::Point>& corners, int width, int height);

// The size a crop comes out at: its own (its longer top or bottom edge
// across, its longer side down), or scaled so its long side is `size`.
void output_size(const std::vector<fixes::Point>& corners, int size, int* width, int* height);

// Planar `channels` from the frame (frame_w x frame_h), sampled through the
// homography into width x height -- bilinear, and averaged over several
// samples a pixel when the crop is being made much smaller.
std::vector<float> warp(const std::vector<float>& planes, int channels, int frame_w, int frame_h,
                        const Homography& h, int width, int height);
// How many samples each way warp() takes per output pixel (1 to 4).
int warp_samples(const Homography& h, int width, int height);

// The light on a flat backdrop is smooth, so behind the performer it is
// estimated as a smooth surface -- a quadratic in x and y, per channel --
// fitted to the visible backdrop around a crop, robustly, so a prop or a
// stray dark patch does not pull it. With less backdrop showing it falls back
// to a plane, then to a constant, rather than extrapolate wildly.
struct LightSurface {
    double coefficient[3][6] = {};
    int terms = 0;  // 0 when nothing could be fitted
    double cx = 0, cy = 0, scale = 1;  // the coordinates it was fitted in
    float at(int channel, double x, double y) const;
};
// Fitted to frame pixels `not_wall` leaves (frame-sized, nonzero where the
// backdrop is hidden), within the crop's bounds widened by half again.
// With `whole_frame`, over the whole frame instead, and never more than a
// plane: for the light's broad change across a backdrop, which a shadow, a
// minority of it, does not pull.
LightSurface fit_light_surface(const std::vector<float>& light, const std::vector<std::uint8_t>& not_wall,
                               int frame_w, int frame_h, const std::vector<fixes::Point>& corners,
                               bool whole_frame = false);
// Where a surface is fitted: a grid over the crop and half its size again
// on every side (the whole frame for `whole_frame`), every `stride` pixels.
struct FitGrid {
    int rx0 = 0, ry0 = 0, rx1 = -1, ry1 = -1, stride = 1;
    double cx = 0, cy = 0, scale = 1;
    bool whole_frame = false;
    bool valid() const { return rx1 >= rx0 && ry1 >= ry0; }
    int columns() const { return valid() ? (rx1 - rx0) / stride + 1 : 0; }
    int rows() const { return valid() ? (ry1 - ry0) / stride + 1 : 0; }
    std::size_t count() const { return static_cast<std::size_t>(columns()) * rows(); }
};
FitGrid fit_grid(const std::vector<fixes::Point>& corners, int frame_w, int frame_h, bool whole_frame);

// The fit from the grid's samples: `values` three planes of count(), `wall`
// nonzero where the sample is backdrop.
LightSurface fit_samples(const FitGrid& grid, const std::vector<float>& values,
                         const std::vector<std::uint8_t>& wall);

// The surface through a crop's homography: planar RGB, width x height.
std::vector<float> surface_image(const LightSurface& surface, const Homography& h, int width, int height);

// The backdrop where the performer is: `light` (planar linear RGB) with what
// `cover` says is not backdrop -- the performer's alpha, and their shadow if
// that is being filled -- replaced by `wall` (planar linear RGB, the backdrop
// as it would be: the relit plate, or an estimate from the frame), corrected
// so it meets the real backdrop where they touch. All width x height.
std::vector<float> fill_behind(const std::vector<float>& light, const std::vector<float>& cover,
                               const std::vector<float>& wall, int width, int height);

// How much of each pixel to replace: the alpha, eased -- fully replaced by a
// third of coverage, and softened a pixel or two -- with `shadow` if given.
std::vector<float> cover_weight(const std::vector<float>& alpha, const std::vector<float>* shadow,
                                int width, int height);

// The performer's shadow on the backdrop, 0..1: backdrop the matte leaves
// that is clearly darker than `unshadowed` predicts -- 12% and more -- its
// colour the same, and near the performer: a backdrop darkening toward the
// floor on its own is not their shadow. Planar linear RGB in.
//
// `reliable` says the prediction comes from a clean plate, which knows how
// the backdrop falls off on its own: then the shadow's soft edge is taken in
// too, from 3% darker. Without one it starts at 12%.
std::vector<float> shadow_map(const std::vector<float>& light, const std::vector<float>& unshadowed,
                              const std::vector<float>& alpha, int width, int height,
                              bool reliable = false);

// One frame, made ready for its light maps: what render_crop() reads.
struct FrameLight {
    const std::vector<float>* linear = nullptr;  // planar linear Rec.709
    const std::vector<float>* alpha = nullptr;   // the matte as shown
    int width = 0, height = 0;
    // Nonzero where the backdrop is hidden or too near the performer to
    // measure (backdrop_hidden()).
    std::vector<std::uint8_t> not_wall;
    // With a clean plate: the plate, and the frame's light on it (frame over
    // plate). Empty without one.
    const std::vector<float>* plate = nullptr;
    std::vector<float> ratio;
    bool fill_shadows = false;
};
// The frame made ready: the backdrop kept back from the performer (further,
// when shadows are being filled, so it stands for the light before they fell)
// and, with a plate, the light on it.
FrameLight prepare_frame_light(const std::vector<float>& linear, const std::vector<float>& alpha,
                               int width, int height, const std::vector<float>* plate,
                               bool fill_shadows);

// What a crop takes from the frame, all crop-sized: the frame's light and the
// matte warped to it, the plate warped likewise (empty without one), and the
// surface fitted around it. Made on the CPU by render_crop, or on the GPU.
struct CropSource {
    std::vector<float> rgb, alpha, plate;
    LightSurface surface;
};
// The crop finished from its source: the backdrop behind the performer
// estimated, their shadow found if it is being filled, and the fill eased in.
std::vector<float> finish_crop(const CropSource& source, const Homography& h, int width, int height,
                               bool fill_shadows, bool with_plate);

// How far from the performer the backdrop is kept back, in pixels.
int backdrop_margin(int frame_width, bool fill_shadows);

// prepare_frame_light's not_wall, asked only where a fit samples: hidden
// within `margin` steps (Manhattan) of any pixel the matte gives the
// performer. Built from row distances, so the frame is never dilated whole.
class BackdropMask {
public:
    void build(const std::vector<float>& alpha, int width, int height, int margin);
    bool hidden(int x, int y) const;
    std::vector<std::uint8_t> wall_at(const FitGrid& grid) const;

private:
    std::vector<std::uint16_t> reach_;
    int width_ = 0, height_ = 0, margin_ = 0;
};

// One crop of a frame: straightened to width x height, the performer -- and
// their shadow, if filling -- replaced by the backdrop behind them. Planar
// linear light, not yet scaled, encoded or adjusted.
std::vector<float> render_crop(const FrameLight& frame, const std::vector<fixes::Point>& corners,
                               int width, int height);

// A rendered crop as it is written: linear light times `scale`, or the look
// clipped at white; sRGB-encoded; and adjusted as the crop says. Planar.
std::vector<float> encode_crop(const std::vector<float>& filled, bool linear, float scale,
                               const fixes::LightCrop& crop, int width, int height);

// Photoshop's Hue/Saturation on encoded RGB (0..1, planar), in place.
void adjust_hue_saturation(std::vector<float>& planes, int width, int height,
                           const fixes::LightCrop& crop);

// sRGB's curve, both ways, for values a light map holds.
float srgb_encode(float linear);
float srgb_decode(float encoded);

}  // namespace ps::light
