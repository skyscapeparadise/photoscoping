#include "color/cube_lut.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string_view>

namespace ps::color {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// Reads up to `count` floats from `line`. from_chars rather than streams: a
// 65-point Canon LUT is 274,625 lines, and stream parsing makes loading one a
// visible pause.
int parse_floats(std::string_view line, float* out, int count) {
    int n = 0;
    const char* p = line.data();
    const char* end = p + line.size();
    while (n < count) {
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p >= end) break;
        // from_chars rejects a leading '+', which some writers emit.
        if (*p == '+') ++p;
        const auto result = std::from_chars(p, end, out[n]);
        if (result.ec != std::errc()) break;
        p = result.ptr;
        ++n;
    }
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    return p == end ? n : -1;  // trailing junk makes the line invalid
}

// The value of a "# Key : value" comment, Canon's header style.
std::optional<std::string> comment_field(std::string_view comment, std::string_view key) {
    std::string_view body = trim(comment.substr(1));
    if (!starts_with(lower(body), lower(key))) return std::nullopt;
    body = trim(body.substr(key.size()));
    if (body.empty() || body.front() != ':') return std::nullopt;
    return std::string(trim(body.substr(1)));
}

// The part after " to " in "Cinema Gamut to BT.2020".
std::string after_to(const std::string& s) {
    const std::string l = lower(s);
    const auto at = l.rfind(" to ");
    return at == std::string::npos ? s : s.substr(at + 4);
}

std::optional<Gamut> gamut_from(const std::string& text) {
    const std::string s = lower(text);
    if (contains(s, "2020")) return Gamut::kRec2020;
    if (contains(s, "p3")) return Gamut::kDciP3;
    if (contains(s, "709")) return Gamut::kRec709;
    if (contains(s, "cinema")) return Gamut::kCinemaGamut;
    return std::nullopt;
}

std::optional<Transfer> transfer_from(const std::string& text) {
    const std::string s = lower(text);
    if (contains(s, "pq") || contains(s, "2084")) return Transfer::kPq;
    if (contains(s, "hlg")) return Transfer::kHlg;
    if (contains(s, "dci")) return Transfer::kGamma26;
    if (contains(s, "linear")) return Transfer::kLinear;
    // Wide DR is Canon's SDR video curve; BT709 is the plain one.
    if (contains(s, "wide dr") || contains(s, "widedr") || contains(s, "709"))
        return Transfer::kBt1886;
    return std::nullopt;
}

// Canon's naming convention (ReadMe_3DLUT.pdf):
//   [Gamut]_[Gamma]-to-[Gamut]_[Gamma]_[Grid]_[InRange][OutRange]_[Version].cube
// e.g. CinemaGamut_CanonLog3-to-BT2020_PQ_65_FF_Ver.2.0.cube
void read_canon_file_name(const std::string& file_name, CubeLut& lut) {
    const auto to = file_name.find("-to-");
    if (to == std::string::npos) return;
    std::vector<std::string> parts;
    std::stringstream ss(file_name.substr(to + 4));
    for (std::string part; std::getline(ss, part, '_');) parts.push_back(part);
    // Output gamut, output gamma, grid, ranges, version.
    if (parts.size() < 5) return;

    if (!lut.full_range_in && parts[3].size() == 2) {
        if (parts[3][0] == 'F') lut.full_range_in = true;
        if (parts[3][0] == 'N') lut.full_range_in = false;
    }
    if (!lut.output) {
        const auto gamut = gamut_from(parts[0]);
        // "BT2020_709WideDR": the gamma field names the curve, which is SDR.
        const auto transfer = transfer_from(parts[1]);
        if (gamut && transfer) lut.output = LutOutput{*gamut, *transfer, parts[1]};
    }
}

}  // namespace

std::optional<CubeLut> parse_cube(const std::string& text, const std::string& file_name,
                                  std::string* error) {
    auto fail = [&](std::string message) -> std::optional<CubeLut> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };

    CubeLut lut;
    std::optional<std::string> canon_gamut, canon_gamma, canon_range;
    std::size_t expected = 0;
    int line_number = 0;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t eol = std::min(text.find('\n', pos), text.size());
        const std::string_view line = trim(std::string_view(text).substr(pos, eol - pos));
        pos = eol + 1;
        ++line_number;
        if (line.empty()) continue;

        if (line.front() == '#') {
            lut.comments.emplace_back(line);
            if (auto v = comment_field(line, "Gamut")) canon_gamut = v;
            if (auto v = comment_field(line, "Gamma")) canon_gamma = v;
            if (auto v = comment_field(line, "Range")) canon_range = v;
            continue;
        }

        const bool is_data = std::isdigit(static_cast<unsigned char>(line.front())) ||
                             line.front() == '-' || line.front() == '.' || line.front() == '+';
        if (is_data) {
            if (lut.size == 0) return fail("data before LUT_1D_SIZE / LUT_3D_SIZE");
            float rgb[3];
            if (parse_floats(line, rgb, 3) != 3)
                return fail("line " + std::to_string(line_number) + ": expected three numbers");
            if (lut.data.size() >= expected * 3)
                return fail("more entries than LUT size " + std::to_string(lut.size) + " allows");
            lut.data.insert(lut.data.end(), rgb, rgb + 3);
            continue;
        }

        const auto space = line.find_first_of(" \t");
        const std::string keyword(line.substr(0, space));
        const std::string_view rest =
            space == std::string_view::npos ? std::string_view() : trim(line.substr(space));

        if (keyword == "TITLE") {
            std::string_view t = rest;
            if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
            lut.title = std::string(t);
        } else if (keyword == "LUT_3D_SIZE" || keyword == "LUT_1D_SIZE") {
            if (lut.size != 0) return fail("more than one LUT size declared");
            float n = 0;
            if (parse_floats(rest, &n, 1) != 1 || n < 2 || n != std::floor(n))
                return fail("invalid " + keyword);
            lut.size = static_cast<int>(n);
            lut.is_3d = keyword == "LUT_3D_SIZE";
            if (lut.is_3d && lut.size > 256) return fail("LUT_3D_SIZE above 256");
            if (!lut.is_3d && lut.size > 65536) return fail("LUT_1D_SIZE above 65536");
            expected = lut.is_3d ? static_cast<std::size_t>(lut.size) * lut.size * lut.size
                                 : static_cast<std::size_t>(lut.size);
            lut.data.reserve(expected * 3);
        } else if (keyword == "DOMAIN_MIN" || keyword == "DOMAIN_MAX") {
            float v[3];
            if (parse_floats(rest, v, 3) != 3) return fail("invalid " + keyword);
            auto& target = keyword == "DOMAIN_MIN" ? lut.domain_min : lut.domain_max;
            target = {v[0], v[1], v[2]};
        } else if (keyword == "LUT_3D_INPUT_RANGE" || keyword == "LUT_1D_INPUT_RANGE") {
            // Older Resolve form: one min and max for all three channels.
            float v[2];
            if (parse_floats(rest, v, 2) != 2) return fail("invalid " + keyword);
            lut.domain_min = {v[0], v[0], v[0]};
            lut.domain_max = {v[1], v[1], v[1]};
        } else {
            return fail("line " + std::to_string(line_number) + ": unknown keyword " + keyword);
        }
    }

    if (lut.size == 0) return fail("no LUT_1D_SIZE or LUT_3D_SIZE");
    if (lut.data.size() != expected * 3)
        return fail("expected " + std::to_string(expected) + " entries, found " +
                    std::to_string(lut.data.size() / 3));
    for (int c = 0; c < 3; ++c)
        if (!(lut.domain_max[c] > lut.domain_min[c])) return fail("empty DOMAIN range");

    // Header comments are the more explicit statement, so they win; the file
    // name fills in whatever they leave open.
    if (canon_range) {
        const std::string r = lower(*canon_range);
        if (starts_with(r, "full in")) lut.full_range_in = true;
        if (starts_with(r, "narrow in") || starts_with(r, "legal in")) lut.full_range_in = false;
    }
    if (canon_gamut && canon_gamma) {
        const std::string curve = after_to(*canon_gamma);
        const auto gamut = gamut_from(after_to(*canon_gamut));
        const auto transfer = transfer_from(curve);
        if (gamut && transfer) lut.output = LutOutput{*gamut, *transfer, curve};
    }
    read_canon_file_name(file_name, lut);
    return lut;
}

std::optional<CubeLut> load_cube(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto slash = path.find_last_of('/');
    return parse_cube(text, slash == std::string::npos ? path : path.substr(slash + 1), error);
}

std::array<float, 3> CubeLut::sample(const std::array<float, 3>& rgb) const {
    const float last = static_cast<float>(size - 1);
    float p[3];
    for (int c = 0; c < 3; ++c) {
        const float t = (rgb[c] - domain_min[c]) / (domain_max[c] - domain_min[c]);
        p[c] = std::clamp(t, 0.0f, 1.0f) * last;
    }

    if (!is_3d) {
        std::array<float, 3> out{};
        for (int c = 0; c < 3; ++c) {
            const int i = std::min(static_cast<int>(p[c]), size - 2);
            const float f = p[c] - static_cast<float>(i);
            out[c] = data[i * 3 + c] * (1.0f - f) + data[(i + 1) * 3 + c] * f;
        }
        return out;
    }

    int i[3];
    float f[3];
    for (int c = 0; c < 3; ++c) {
        i[c] = std::min(static_cast<int>(p[c]), size - 2);
        f[c] = p[c] - static_cast<float>(i[c]);
    }
    auto at = [&](int dr, int dg, int db) {
        const std::size_t index =
            (static_cast<std::size_t>(i[2] + db) * size + (i[1] + dg)) * size + (i[0] + dr);
        return std::array<float, 3>{data[index * 3], data[index * 3 + 1], data[index * 3 + 2]};
    };

    // The cube splits into six tetrahedra by the ordering of the fractional
    // parts; each output is a weighted sum of four corners. Smoother than
    // trilinear across the neutral axis, which is where skin and the white
    // cyc live.
    const float fr = f[0], fg = f[1], fb = f[2];
    std::array<float, 3> c0 = at(0, 0, 0), c3 = at(1, 1, 1), c1, c2;
    float w0, w1, w2, w3;
    if (fr > fg) {
        if (fg > fb) {         // r > g > b
            c1 = at(1, 0, 0); c2 = at(1, 1, 0);
            w0 = 1 - fr; w1 = fr - fg; w2 = fg - fb; w3 = fb;
        } else if (fr > fb) {  // r > b >= g
            c1 = at(1, 0, 0); c2 = at(1, 0, 1);
            w0 = 1 - fr; w1 = fr - fb; w2 = fb - fg; w3 = fg;
        } else {               // b >= r > g
            c1 = at(0, 0, 1); c2 = at(1, 0, 1);
            w0 = 1 - fb; w1 = fb - fr; w2 = fr - fg; w3 = fg;
        }
    } else {
        if (fb > fg) {         // b > g >= r
            c1 = at(0, 0, 1); c2 = at(0, 1, 1);
            w0 = 1 - fb; w1 = fb - fg; w2 = fg - fr; w3 = fr;
        } else if (fb > fr) {  // g >= b > r
            c1 = at(0, 1, 0); c2 = at(0, 1, 1);
            w0 = 1 - fg; w1 = fg - fb; w2 = fb - fr; w3 = fr;
        } else {               // g >= r >= b
            c1 = at(0, 1, 0); c2 = at(1, 1, 0);
            w0 = 1 - fg; w1 = fg - fr; w2 = fr - fb; w3 = fb;
        }
    }
    return {w0 * c0[0] + w1 * c1[0] + w2 * c2[0] + w3 * c3[0],
            w0 * c0[1] + w1 * c1[1] + w2 * c2[1] + w3 * c3[1],
            w0 * c0[2] + w1 * c1[2] + w2 * c2[2] + w3 * c3[2]};
}

}  // namespace ps::color
