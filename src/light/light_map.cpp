#include "light/light_map.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "matting/image_ops.hpp"

namespace ps::light {

namespace {

// Solves the 8x8 system for the homography taking the unit corners of the
// output to the quad, by Gaussian elimination with partial pivoting.
bool solve8(double a[8][9], double out[8]) {
    for (int c = 0; c < 8; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 8; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-12) return false;
        for (int k = 0; k < 9; ++k) std::swap(a[c][k], a[pivot][k]);
        for (int r = 0; r < 8; ++r) {
            if (r == c) continue;
            const double f = a[r][c] / a[c][c];
            for (int k = c; k < 9; ++k) a[r][k] -= f * a[c][k];
        }
    }
    for (int i = 0; i < 8; ++i) out[i] = a[i][8] / a[i][i];
    return true;
}

float length(const fixes::Point& a, const fixes::Point& b) {
    return std::hypot(b.x - a.x, b.y - a.y);
}

void rgb_to_hsl(float r, float g, float b, float* h, float* s, float* l) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    *l = (mx + mn) * 0.5f;
    const float d = mx - mn;
    if (d <= 1e-6f) {
        *h = 0.0f;
        *s = 0.0f;
        return;
    }
    *s = *l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == r) *h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) *h = (b - r) / d + 2.0f;
    else *h = (r - g) / d + 4.0f;
    *h *= 60.0f;
}

float hue_channel(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

void hsl_to_rgb(float h, float s, float l, float* r, float* g, float* b) {
    if (s <= 1e-6f) {
        *r = *g = *b = l;
        return;
    }
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    const float hh = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f) / 360.0f;
    *r = hue_channel(p, q, hh + 1.0f / 3.0f);
    *g = hue_channel(p, q, hh);
    *b = hue_channel(p, q, hh - 1.0f / 3.0f);
}

}  // namespace

void Homography::map(double u, double v, double* x, double* y) const {
    const double w = m[6] * u + m[7] * v + m[8];
    *x = (m[0] * u + m[1] * v + m[2]) / w;
    *y = (m[3] * u + m[4] * v + m[5]) / w;
}

Homography quad_homography(const std::vector<fixes::Point>& c, int width, int height) {
    Homography h;
    if (c.size() != 4) return h;
    const double src[4][2] = {{0, 0}, {static_cast<double>(width), 0},
                              {static_cast<double>(width), static_cast<double>(height)},
                              {0, static_cast<double>(height)}};
    double a[8][9] = {};
    for (int i = 0; i < 4; ++i) {
        const double u = src[i][0], v = src[i][1], x = c[i].x, y = c[i].y;
        const double r1[9] = {u, v, 1, 0, 0, 0, -u * x, -v * x, x};
        const double r2[9] = {0, 0, 0, u, v, 1, -u * y, -v * y, y};
        std::copy(r1, r1 + 9, a[i * 2]);
        std::copy(r2, r2 + 9, a[i * 2 + 1]);
    }
    double sol[8];
    if (!solve8(a, sol)) return h;
    std::copy(sol, sol + 8, h.m);
    h.m[8] = 1.0;
    return h;
}

void output_size(const std::vector<fixes::Point>& c, int size, int* width, int* height) {
    if (c.size() != 4) {
        *width = *height = 0;
        return;
    }
    float w = std::max(length(c[0], c[1]), length(c[3], c[2]));
    float h = std::max(length(c[0], c[3]), length(c[1], c[2]));
    if (size > 0 && w > 0.0f && h > 0.0f) {
        const float k = static_cast<float>(size) / std::max(w, h);
        w *= k;
        h *= k;
    }
    *width = std::max(1, static_cast<int>(std::lround(w)));
    *height = std::max(1, static_cast<int>(std::lround(h)));
}

std::vector<float> warp(const std::vector<float>& planes, int channels, int fw, int fh,
                        const Homography& hm, int width, int height) {
    const std::size_t fn = static_cast<std::size_t>(fw) * fh, n = static_cast<std::size_t>(width) * height;
    std::vector<float> out(n * channels, 0.0f);
    // How far apart neighbouring output pixels land in the frame, at the
    // centre: samples enough to cover that, up to four each way.
    double x0, y0, x1, y1, x2, y2;
    hm.map(width * 0.5, height * 0.5, &x0, &y0);
    hm.map(width * 0.5 + 1, height * 0.5, &x1, &y1);
    hm.map(width * 0.5, height * 0.5 + 1, &x2, &y2);
    const double spread = std::max(std::hypot(x1 - x0, y1 - y0), std::hypot(x2 - x0, y2 - y0));
    const int k = std::clamp(static_cast<int>(std::ceil(spread)), 1, 4);
    const float weight = 1.0f / static_cast<float>(k * k);
    for (int v = 0; v < height; ++v)
        for (int u = 0; u < width; ++u) {
            const std::size_t o = static_cast<std::size_t>(v) * width + u;
            for (int sy = 0; sy < k; ++sy)
                for (int sx = 0; sx < k; ++sx) {
                    double x, y;
                    hm.map(u + (sx + 0.5) / k, v + (sy + 0.5) / k, &x, &y);
                    // Pixel centres at half-integers.
                    const double px = std::clamp(x - 0.5, 0.0, static_cast<double>(fw - 1));
                    const double py = std::clamp(y - 0.5, 0.0, static_cast<double>(fh - 1));
                    const int ix = std::min(static_cast<int>(px), fw - 2 < 0 ? 0 : fw - 2);
                    const int iy = std::min(static_cast<int>(py), fh - 2 < 0 ? 0 : fh - 2);
                    const float tx = static_cast<float>(px - ix), ty = static_cast<float>(py - iy);
                    const int jx = std::min(ix + 1, fw - 1), jy = std::min(iy + 1, fh - 1);
                    for (int c = 0; c < channels; ++c) {
                        const float* p = &planes[c * fn];
                        const float a = p[static_cast<std::size_t>(iy) * fw + ix];
                        const float b = p[static_cast<std::size_t>(iy) * fw + jx];
                        const float d = p[static_cast<std::size_t>(jy) * fw + ix];
                        const float e = p[static_cast<std::size_t>(jy) * fw + jx];
                        out[c * n + o] += weight * ((a * (1 - tx) + b * tx) * (1 - ty) + (d * (1 - tx) + e * tx) * ty);
                    }
                }
        }
    return out;
}

std::vector<float> cover_weight(const std::vector<float>& alpha, const std::vector<float>* shadow,
                                int width, int height) {
    const std::size_t n = static_cast<std::size_t>(width) * height;
    // The performer's faintest edge -- stray hair, the tail of motion blur --
    // still carries their colour, so what is replaced reaches a few pixels
    // past anything the matte gives them at all, fading out over that.
    std::vector<float> touched(n);
    for (std::size_t i = 0; i < n; ++i) touched[i] = alpha[i] > 0.01f ? 1.0f : 0.0f;
    const std::vector<float> reach = matting::gaussian_blur(touched, width, height, 2.5f);
    std::vector<float> w(n);
    for (std::size_t i = 0; i < n; ++i) {
        w[i] = std::max(std::min(1.0f, alpha[i] * 3.0f), std::min(1.0f, reach[i] * 3.0f));
        if (shadow) w[i] = std::max(w[i], (*shadow)[i]);
    }
    w = matting::gaussian_blur(w, width, height, 1.5f);
    for (float& v : w) v = std::clamp(v * 1.5f, 0.0f, 1.0f);
    return w;
}

float LightSurface::at(int channel, double x, double y) const {
    if (terms == 0) return 0.0f;
    const double u = (x - cx) * scale, v = (y - cy) * scale;
    const double basis[6] = {1, u, v, u * u, u * v, v * v};
    double s = 0;
    for (int k = 0; k < terms; ++k) s += coefficient[channel][k] * basis[k];
    return static_cast<float>(s);
}

LightSurface fit_light_surface(const std::vector<float>& light, const std::vector<std::uint8_t>& not_wall,
                               int fw, int fh, const std::vector<fixes::Point>& corners, bool whole_frame) {
    LightSurface out;
    if (corners.size() != 4) return out;
    float x0 = corners[0].x, x1 = x0, y0 = corners[0].y, y1 = y0;
    for (const fixes::Point& p : corners) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    const float mx = (x1 - x0) * 0.5f, my = (y1 - y0) * 0.5f;
    int rx0 = std::clamp(static_cast<int>(x0 - mx), 0, fw - 1), rx1 = std::clamp(static_cast<int>(x1 + mx), 0, fw - 1);
    int ry0 = std::clamp(static_cast<int>(y0 - my), 0, fh - 1), ry1 = std::clamp(static_cast<int>(y1 + my), 0, fh - 1);
    if (whole_frame) {
        rx0 = ry0 = 0;
        rx1 = fw - 1;
        ry1 = fh - 1;
    }
    out.cx = (rx0 + rx1) * 0.5;
    out.cy = (ry0 + ry1) * 0.5;
    out.scale = 2.0 / std::max(1, std::max(rx1 - rx0, ry1 - ry0));

    // The backdrop's pixels in the region, every few: enough to fit, few
    // enough to be quick.
    const std::size_t fn = static_cast<std::size_t>(fw) * fh;
    const int stride = std::max(2, std::max(rx1 - rx0, ry1 - ry0) / 160);
    std::vector<double> us, vs, vals[3];
    std::size_t region = 0;
    for (int y = ry0; y <= ry1; y += stride)
        for (int x = rx0; x <= rx1; x += stride) {
            ++region;
            const std::size_t i = static_cast<std::size_t>(y) * fw + x;
            if (not_wall[i]) continue;
            us.push_back((x + 0.5 - out.cx) * out.scale);
            vs.push_back((y + 0.5 - out.cy) * out.scale);
            for (int c = 0; c < 3; ++c) vals[c].push_back(light[c * fn + i]);
        }
    const std::size_t n = us.size();
    if (n < 12) return out;
    // Less backdrop showing, a simpler surface.
    const double shown = static_cast<double>(n) / static_cast<double>(region);
    out.terms = shown > 0.4 ? 6 : shown > 0.1 ? 3 : 1;
    if (whole_frame) out.terms = std::min(out.terms, 3);
    const int t = out.terms;

    for (int c = 0; c < 3; ++c) {
        std::vector<double> weight(n, 1.0);
        double coef[6] = {};
        // Least squares, then twice more with outliers down-weighted
        // (Tukey's biweight, by the residuals' median spread).
        for (int pass = 0; pass < 3; ++pass) {
            double a[6][7] = {};
            for (std::size_t i = 0; i < n; ++i) {
                const double basis[6] = {1, us[i], vs[i], us[i] * us[i], us[i] * vs[i], vs[i] * vs[i]};
                for (int r = 0; r < t; ++r) {
                    for (int k = 0; k < t; ++k) a[r][k] += weight[i] * basis[r] * basis[k];
                    a[r][t] += weight[i] * basis[r] * vals[c][i];
                }
            }
            for (int r = 0; r < t; ++r) a[r][r] += 1e-9;
            // Gaussian elimination.
            for (int col = 0; col < t; ++col) {
                int pivot = col;
                for (int r = col + 1; r < t; ++r)
                    if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
                for (int k = 0; k <= t; ++k) std::swap(a[col][k], a[pivot][k]);
                if (std::fabs(a[col][col]) < 1e-15) continue;
                for (int r = 0; r < t; ++r) {
                    if (r == col) continue;
                    const double f = a[r][col] / a[col][col];
                    for (int k = col; k <= t; ++k) a[r][k] -= f * a[col][k];
                }
            }
            for (int k = 0; k < t; ++k) coef[k] = std::fabs(a[k][k]) > 1e-15 ? a[k][t] / a[k][k] : 0.0;
            if (pass == 2) break;
            std::vector<double> residual(n);
            for (std::size_t i = 0; i < n; ++i) {
                const double basis[6] = {1, us[i], vs[i], us[i] * us[i], us[i] * vs[i], vs[i] * vs[i]};
                double s = 0;
                for (int k = 0; k < t; ++k) s += coef[k] * basis[k];
                residual[i] = std::fabs(vals[c][i] - s);
            }
            std::vector<double> sorted = residual;
            std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
            const double spread = std::max(sorted[n / 2] * 1.4826 * 4.685, 1e-4);
            for (std::size_t i = 0; i < n; ++i) {
                const double r = residual[i] / spread;
                weight[i] = r < 1.0 ? (1 - r * r) * (1 - r * r) : 0.0;
            }
        }
        for (int k = 0; k < t; ++k) out.coefficient[c][k] = coef[k];
    }
    return out;
}

std::vector<float> surface_image(const LightSurface& surface, const Homography& hm, int width, int height) {
    const std::size_t n = static_cast<std::size_t>(width) * height;
    std::vector<float> out(n * 3);
    for (int v = 0; v < height; ++v)
        for (int u = 0; u < width; ++u) {
            double x, y;
            hm.map(u + 0.5, v + 0.5, &x, &y);
            const std::size_t i = static_cast<std::size_t>(v) * width + u;
            for (int c = 0; c < 3; ++c) out[c * n + i] = std::max(0.0f, surface.at(c, x, y));
        }
    return out;
}

std::vector<float> fill_behind(const std::vector<float>& light, const std::vector<float>& cover,
                               const std::vector<float>& wall, int width, int height) {
    const std::size_t n = static_cast<std::size_t>(width) * height;
    std::vector<std::uint8_t> known(n);
    std::size_t known_count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        known[i] = cover[i] < 0.002f;
        known_count += known[i];
    }
    // What the wall estimate gets wrong, measured where the backdrop shows
    // and carried in behind the performer -- so the fill meets the real
    // backdrop at its edge. With none showing, the estimate stands alone.
    std::vector<float> correction(n * 3, 0.0f);
    if (known_count > 0) {
        std::vector<float> residual(n * 3);
        for (std::size_t i = 0; i < n * 3; ++i) residual[i] = light[i] - wall[i];
        correction = matting::fill_plate(residual, width, height, known);
        // Only near the real backdrop: deeper in, the estimate alone, so a
        // patch of backdrop coming and going at the edge cannot swing the
        // middle of the fill.
        std::vector<float> near(n);
        for (std::size_t i = 0; i < n; ++i) near[i] = known[i] ? 1.0f : 0.0f;
        const float reach = std::max(4.0f, 0.04f * static_cast<float>(std::max(width, height)));
        near = matting::gaussian_blur(near, width, height, reach);
        for (std::size_t i = 0; i < n; ++i) {
            const float k = known[i] ? 1.0f : std::clamp(near[i] * 2.5f, 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c) correction[c * n + i] *= k;
        }
    }
    std::vector<float> out(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        const float w = cover[i];
        for (int c = 0; c < 3; ++c) {
            const std::size_t k = c * n + i;
            const float filled = std::max(0.0f, wall[k] + correction[k]);
            out[k] = light[k] * (1.0f - w) + filled * w;
        }
    }
    return out;
}

std::vector<float> shadow_map(const std::vector<float>& light, const std::vector<float>& unshadowed,
                              const std::vector<float>& alpha, int width, int height, bool reliable) {
    const std::size_t n = static_cast<std::size_t>(width) * height;
    std::vector<float> s(n, 0.0f);
    std::vector<std::uint8_t> performer(n);
    for (std::size_t i = 0; i < n; ++i) performer[i] = alpha[i] >= 0.02f;
    const int reach = std::max(16, std::max(width, height) / 4);
    const std::vector<std::uint8_t> near = matting::dilate_cross(performer, width, height, reach);
    for (std::size_t i = 0; i < n; ++i) {
        if (alpha[i] >= 0.5f || !near[i]) continue;
        float fl = 0, ul = 0, fc[3], uc[3];
        for (int c = 0; c < 3; ++c) {
            fc[c] = std::max(light[c * n + i], 0.0f);
            uc[c] = std::max(unshadowed[c * n + i], 0.0f);
            fl += fc[c];
            ul += uc[c];
        }
        if (ul <= 1e-4f) continue;
        const float ratio = fl / ul;
        float chroma = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const float d = fc[c] / std::max(fl, 1e-4f) - uc[c] / ul;
            chroma += d * d;
        }
        if (std::sqrt(chroma) > 0.03f) continue;
        // None above 88% of the light, fully shadow by 70% -- with a
        // plate's prediction, none above 97%, fully by 80%.
        const float none = reliable ? 0.97f : 0.88f, full = reliable ? 0.80f : 0.70f;
        const float t = std::clamp((none - ratio) / (none - full), 0.0f, 1.0f);
        s[i] = t * t * (3.0f - 2.0f * t);
    }
    // Specks of noise are not shadows.
    s = matting::gaussian_blur(s, width, height, 2.0f);
    for (float& v : s) v = std::clamp((v - 0.15f) / 0.6f, 0.0f, 1.0f);
    return s;
}

void adjust_hue_saturation(std::vector<float>& planes, int width, int height,
                           const fixes::LightCrop& crop) {
    if (!crop.adjusted()) return;
    const std::size_t n = static_cast<std::size_t>(width) * height;
    const float sat = crop.saturation / 100.0f, light = crop.lightness / 100.0f;
    for (std::size_t i = 0; i < n; ++i) {
        float r = std::clamp(planes[i], 0.0f, 1.0f);
        float g = std::clamp(planes[n + i], 0.0f, 1.0f);
        float b = std::clamp(planes[2 * n + i], 0.0f, 1.0f);
        if (crop.colorize) {
            // One hue and saturation throughout, the lightness the pixel's
            // own brightness.
            const float l = 0.299f * r + 0.587f * g + 0.114f * b;
            hsl_to_rgb(crop.hue, std::clamp(crop.saturation, 0.0f, 100.0f) / 100.0f, l, &r, &g, &b);
        } else {
            float h, s, l;
            rgb_to_hsl(r, g, b, &h, &s, &l);
            h += crop.hue;
            s = sat < 0.0f ? s * (1.0f + sat) : s + (1.0f - s) * sat;
            hsl_to_rgb(h, std::clamp(s, 0.0f, 1.0f), l, &r, &g, &b);
        }
        // Lightness: toward black below zero, toward white above.
        const float to = light < 0.0f ? 0.0f : 1.0f;
        const float k = std::fabs(light);
        planes[i] = r + (to - r) * k;
        planes[n + i] = g + (to - g) * k;
        planes[2 * n + i] = b + (to - b) * k;
    }
}

float srgb_encode(float l) {
    l = std::max(l, 0.0f);
    return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

float srgb_decode(float v) {
    v = std::max(v, 0.0f);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

}  // namespace ps::light
