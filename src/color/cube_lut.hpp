#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "color/color_space.hpp"

namespace ps::color {

// What a LUT turns its input into, so the app knows how to bring its output
// into the working space. Read from the file where it says (Canon writes
// "# Gamut :" / "# Gamma :" / "# Range :" comments, and encodes the same in
// its file names); unknown for LUTs that do not, in which case the user sets
// it once by hand.
struct LutOutput {
    Gamut gamut = Gamut::kRec709;
    Transfer transfer = Transfer::kBt1886;
    // Free-form description of the curve as the file names it ("Wide DR",
    // "HLGKnee-Vivid"), for display. Variants of one transfer -- HLG Knee and
    // Vivid are looks baked into an HLG signal -- decode identically.
    std::string curve;
};

// A parsed .cube file (the Resolve/Adobe format): either a 1D or a 3D table.
struct CubeLut {
    std::string title;
    int size = 0;           // entries per axis
    bool is_3d = false;
    // RGB triples. For 3D, index = r + g*size + b*size*size (red fastest),
    // which is also the x-fastest order a 3D texture upload wants.
    std::vector<float> data;
    std::array<float, 3> domain_min = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> domain_max = {1.0f, 1.0f, 1.0f};

    // Whether the input is full range, as the file states. Unknown is not
    // assumed to be either; the caller decides and says so.
    std::optional<bool> full_range_in;
    std::optional<LutOutput> output;
    // Every '#' comment line, verbatim, for showing the user what the file
    // says about itself.
    std::vector<std::string> comments;

    // Tetrahedral interpolation of an input triple. The shader does the same
    // arithmetic (src/gpu/shaders/ingest.frag); this is its reference.
    std::array<float, 3> sample(const std::array<float, 3>& rgb) const;
};

// Parses `text` as a .cube file. `file_name` is used only to recognise
// Canon's naming convention when the header comments do not settle the
// output. Returns nullopt and fills `error` on a malformed file.
std::optional<CubeLut> parse_cube(const std::string& text, const std::string& file_name,
                                  std::string* error);

std::optional<CubeLut> load_cube(const std::string& path, std::string* error);

}  // namespace ps::color
