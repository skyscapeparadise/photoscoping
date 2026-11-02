#pragma once

#include <string>
#include <vector>

#include "matting/preprocess.hpp"

// A clean plate: the set without the subject, from frames of the clip where
// it is empty. Optional -- every stage works without one -- and only as good
// as the shot is locked off.
//
// The plate is never compared with a frame as it is. A strobe changes the
// wall's brightness frame to frame, and the subject shades the wall it stands
// in front of, so each frame relights the plate from its own visible wall
// first (relight_plate): the plate keeps its detail, the frame sets its
// light.
namespace ps::matting {

struct PlateImage {
    std::vector<float> linear;  // planar linear Rec.709
    int width = 0, height = 0;
    bool empty() const { return linear.empty(); }
};

// The mean of the frames added: noise averages out of it.
class PlateBuilder {
public:
    void add(const MattingFrame& frame);
    int count() const { return count_; }
    PlateImage finish() const;

private:
    std::vector<double> sum_;
    int width_ = 0, height_ = 0, count_ = 0;
};

// As a 16-bit RGBA PNG, sRGB-encoded, so it can be looked at as it is.
bool save_plate(const std::string& path, const PlateImage& plate);
bool load_plate(const std::string& path, PlateImage* plate);
// Half size, each pixel the mean of four.
PlateImage halve_plate(const PlateImage& plate);

// The plate as `frame` would see the wall: its colour times the frame's light,
// which is measured where the frame shows the wall -- `alpha` under 0.02, kept
// back from the subject's edge -- as the ratio of the two, smoothed and filled
// in behind the subject. Soft shadows and the strobe's level follow; hard
// contact shadows do not, and stay for the band to decide. Planar linear
// Rec.709, the frame's size. Empty if the plate is not the frame's size.
// `margin` is how far back from the subject, in pixels, the wall is measured:
// wider leaves the subject's shadow out of what is measured.
std::vector<float> relight_plate(const PlateImage& plate, const MattingFrame& frame,
                                 const std::vector<float>& alpha, int margin = 6);

// How far the frame's visible wall strays from the relit plate by noise
// alone: three times the median colour distance over it, held to at least
// 0.01. Closer than this, a pixel is the wall.
float plate_tolerance(const MattingFrame& frame, const std::vector<float>& wall,
                      const std::vector<float>& alpha);

// The override, for when the plate should overrule the model: wall the
// matte kept as subject -- a region matching the plate, reached from the
// visible wall -- goes, and holes -- a region the matte dropped that is
// unlike the plate, unlike a shadow on it, and enclosed by the subject -- are
// filled. Region by region at a quarter of the size, never pixel by pixel;
// what changes is softened at its edge so the band stage refines it.
std::vector<float> correct_with_plate(const MattingFrame& frame, const std::vector<float>& wall,
                                      const std::vector<float>& alpha, float tolerance);

}  // namespace ps::matting
