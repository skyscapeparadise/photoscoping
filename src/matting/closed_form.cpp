#include "matting/closed_form.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ps::matting {

namespace {

// A symmetric sparse matrix, row by row, columns ascending in each row. Being
// symmetric, it is its own transpose, so the same arrays also read as the
// column-major form the factorisation wants.
struct Sparse {
    std::vector<double> values;
    std::vector<std::int64_t> columns;
    std::vector<std::int64_t> starts;  // n + 1
    std::int64_t n = 0;

    void multiply(const std::vector<double>& x, std::vector<double>& y) const {
        for (std::int64_t i = 0; i < n; ++i) {
            double s = 0.0;
            for (std::int64_t k = starts[i]; k < starts[i + 1]; ++k) s += values[k] * x[columns[k]];
            y[i] = s;
        }
    }
};

// pymatting's _cf_laplacian, radius 1. Each row holds the 5x5 neighbourhood
// a pixel shares a 3x3 window with, accumulated over every window not wholly
// inside the known region.
std::vector<double> cf_laplacian(const std::vector<double>& image, int w, int h,
                                 const std::vector<std::uint8_t>& known, double epsilon) {
    constexpr int r = 1, size = 3, span = 5, area = 9;
    std::vector<double> values(static_cast<std::size_t>(w) * h * span * span, 0.0);
    double c[size][size][3];
    for (int y = r; y < h - r; ++y) {
        for (int x = r; x < w - r; ++x) {
            bool all_known = true;
            for (int dy = -r; dy <= r && all_known; ++dy)
                for (int dx = -r; dx <= r; ++dx)
                    if (!known[static_cast<std::size_t>(y + dy) * w + (x + dx)]) {
                        all_known = false;
                        break;
                    }
            if (all_known) continue;

            for (int dc = 0; dc < 3; ++dc) {
                double s = 0.0;
                for (int dy = 0; dy < size; ++dy)
                    for (int dx = 0; dx < size; ++dx)
                        s += image[(static_cast<std::size_t>(y + dy - r) * w + (x + dx - r)) * 3 + dc];
                for (int dy = 0; dy < size; ++dy)
                    for (int dx = 0; dx < size; ++dx)
                        c[dy][dx][dc] =
                            image[(static_cast<std::size_t>(y + dy - r) * w + (x + dx - r)) * 3 + dc] -
                            s / area;
            }
            double a00 = epsilon, a01 = 0.0, a02 = 0.0, a11 = epsilon, a12 = 0.0, a22 = epsilon;
            for (int dy = 0; dy < size; ++dy)
                for (int dx = 0; dx < size; ++dx) {
                    const double* v = c[dy][dx];
                    a00 += v[0] * v[0];
                    a01 += v[0] * v[1];
                    a02 += v[0] * v[2];
                    a11 += v[1] * v[1];
                    a12 += v[1] * v[2];
                    a22 += v[2] * v[2];
                }
            a00 /= area; a01 /= area; a02 /= area; a11 /= area; a12 /= area; a22 /= area;
            const double det = a00 * a12 * a12 + a01 * a01 * a22 + a02 * a02 * a11 -
                               a00 * a11 * a22 - 2 * a01 * a02 * a12;
            const double inv_det = 1.0 / det;
            const double m00 = (a12 * a12 - a11 * a22) * inv_det;
            const double m01 = (a01 * a22 - a02 * a12) * inv_det;
            const double m02 = (a02 * a11 - a01 * a12) * inv_det;
            const double m11 = (a02 * a02 - a00 * a22) * inv_det;
            const double m12 = (a00 * a12 - a01 * a02) * inv_det;
            const double m22 = (a01 * a01 - a00 * a11) * inv_det;

            for (int dyi = 0; dyi < size; ++dyi)
                for (int dxi = 0; dxi < size; ++dxi) {
                    const double s = c[dyi][dxi][0], t = c[dyi][dxi][1], u = c[dyi][dxi][2];
                    const double c0 = m00 * s + m01 * t + m02 * u;
                    const double c1 = m01 * s + m11 * t + m12 * u;
                    const double c2 = m02 * s + m12 * t + m22 * u;
                    for (int dyj = 0; dyj < size; ++dyj)
                        for (int dxj = 0; dxj < size; ++dxj) {
                            const int xi = x + dxi - r, yi = y + dyi - r;
                            const int xj = x + dxj - r, yj = y + dyj - r;
                            const std::int64_t i = xi + static_cast<std::int64_t>(yi) * w;
                            const std::int64_t j = xj + static_cast<std::int64_t>(yj) * w;
                            const double temp = c0 * c[dyj][dxj][0] + c1 * c[dyj][dxj][1] +
                                                c2 * c[dyj][dxj][2];
                            const double value = (i == j ? 1.0 : 0.0) - (1.0 + temp) / area;
                            const int ddx = xj - xi + 2 * r, ddy = yj - yi + 2 * r;
                            values[(static_cast<std::size_t>(i) * span + ddy) * span + ddx] += value;
                        }
                }
        }
    }
    return values;
}

// pymatting's _ichol, column by column over the lower triangle. Returns false
// when a pivot goes non-positive, for the caller to try a larger shift.
bool ichol(const Sparse& a, double discard_threshold, double shift, std::vector<double>& lv,
           std::vector<std::int64_t>& lr, std::vector<std::int64_t>& lp) {
    const std::int64_t n = a.n;
    lv.clear();
    lr.clear();
    lp.assign(n + 1, 0);
    std::vector<std::int64_t> s(n, 0), t(n, 0), l(n, -1), c(n);
    std::vector<double> av(n, 0.0), d(n, shift);
    std::vector<std::uint8_t> b(n, 0);
    std::int64_t c_n = 0;

    for (std::int64_t j = 0; j < n; ++j)
        for (std::int64_t idx = a.starts[j]; idx < a.starts[j + 1]; ++idx)
            if (a.columns[idx] == j) {
                d[j] += a.values[idx];
                t[j] = idx + 1;
            }

    for (std::int64_t j = 0; j < n; ++j) {
        for (std::int64_t idx = t[j]; idx < a.starts[j + 1]; ++idx) {
            const std::int64_t i = a.columns[idx];
            const double l_ij = a.values[idx];
            if (l_ij != 0.0 && i > j) {
                av[i] += l_ij;
                if (!b[i]) {
                    b[i] = 1;
                    c[c_n++] = i;
                }
            }
        }
        std::int64_t k = l[j];
        while (k != -1) {
            std::int64_t k0 = s[k];
            const std::int64_t k1 = lp[k + 1];
            const std::int64_t k2 = l[k];
            const double l_jk = lv[k0];
            ++k0;
            if (k0 < k1) {
                s[k] = k0;
                const std::int64_t i = lr[k0];
                l[k] = l[i];
                l[i] = k;
                for (std::int64_t idx = k0; idx < k1; ++idx) {
                    const std::int64_t i2 = lr[idx];
                    av[i2] -= lv[idx] * l_jk;
                    if (!b[i2]) {
                        b[i2] = 1;
                        c[c_n++] = i2;
                    }
                }
            }
            k = k2;
        }
        if (d[j] <= 0.0) return false;
        d[j] = std::sqrt(d[j]);
        lv.push_back(d[j]);
        lr.push_back(j);
        s[j] = static_cast<std::int64_t>(lv.size());
        std::sort(c.begin(), c.begin() + c_n);
        for (std::int64_t q = 0; q < c_n; ++q) {
            const std::int64_t i = c[q];
            const double l_ij = av[i] / d[j];
            d[i] -= l_ij * l_ij;  // diag_keep_discarded
            if (std::abs(l_ij) > discard_threshold && std::abs(av[i]) > 0.0) {
                lv.push_back(l_ij);
                lr.push_back(i);
            }
            av[i] = 0.0;
            b[i] = 0;
        }
        c_n = 0;
        lp[j + 1] = static_cast<std::int64_t>(lv.size());
        if (lp[j] + 1 < lp[j + 1]) {
            const std::int64_t i = lr[lp[j] + 1];
            l[j] = l[i];
            l[i] = j;
        }
    }
    return true;
}

// L L^T x = b, by the two triangular solves pymatting's preconditioner does.
void cholesky_solve(const std::vector<double>& lv, const std::vector<std::int64_t>& lr,
                    const std::vector<std::int64_t>& lp, std::vector<double>& x) {
    const std::int64_t n = static_cast<std::int64_t>(lp.size()) - 1;
    for (std::int64_t j = 0; j < n; ++j) {
        const double temp = x[j] / lv[lp[j]];
        x[j] = temp;
        for (std::int64_t k = lp[j] + 1; k < lp[j + 1]; ++k) x[lr[k]] -= lv[k] * temp;
    }
    for (std::int64_t i = n - 1; i >= 0; --i) {
        double s = x[i];
        for (std::int64_t k = lp[i] + 1; k < lp[i + 1]; ++k) s -= lv[k] * x[lr[k]];
        x[i] = s / lv[lp[i]];
    }
}

double dot(const std::vector<double>& a, const std::vector<double>& b) {
    double s = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
    return s;
}

}  // namespace

std::vector<double> estimate_alpha_cf(const std::vector<double>& rgb,
                                      const std::vector<double>& trimap, int w, int h,
                                      std::string* error) {
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    std::vector<std::uint8_t> known(pixels), fg(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        fg[i] = trimap[i] >= 0.9;
        known[i] = fg[i] || trimap[i] <= 0.1;
    }

    const std::vector<double> lap = cf_laplacian(rgb, w, h, known, 1e-7);

    // Unknown pixels in raster order, as numpy's boolean indexing has them.
    std::vector<std::int64_t> slot(pixels, -1);
    std::int64_t n = 0;
    for (std::size_t i = 0; i < pixels; ++i)
        if (!known[i]) slot[i] = n++;
    std::vector<double> x(trimap.begin(), trimap.end());
    if (n == 0) {
        for (double& v : x) v = std::clamp(v, 0.0, 1.0);
        return x;
    }

    // L_U, and b = -R m: the known neighbours' pull, which is only the
    // subject's since the background's value is zero.
    Sparse lu;
    lu.n = n;
    lu.starts.reserve(n + 1);
    lu.starts.push_back(0);
    std::vector<double> rhs(n, 0.0);
    for (int yi = 0; yi < h; ++yi)
        for (int xi = 0; xi < w; ++xi) {
            const std::size_t i = static_cast<std::size_t>(yi) * w + xi;
            if (known[i]) continue;
            for (int ddy = 0; ddy < 5; ++ddy)
                for (int ddx = 0; ddx < 5; ++ddx) {
                    const int xj = xi + ddx - 2, yj = yi + ddy - 2;
                    if (xj < 0 || yj < 0 || xj >= w || yj >= h) continue;
                    const double v = lap[(i * 5 + ddy) * 5 + ddx];
                    const std::size_t j = static_cast<std::size_t>(yj) * w + xj;
                    if (known[j]) {
                        if (fg[j]) rhs[slot[i]] -= v;
                    } else {
                        lu.values.push_back(v);
                        lu.columns.push_back(slot[j]);
                    }
                }
            lu.starts.push_back(static_cast<std::int64_t>(lu.values.size()));
        }

    // The factorisation, with pymatting's ladder of diagonal shifts for a
    // matrix not positive-definite enough at the first.
    std::vector<double> lv;
    std::vector<std::int64_t> lr, lp;
    bool factored = false;
    for (double shift : {0.0, 1e-4, 1e-3, 1e-2, 0.1, 0.5, 1.0, 10.0, 100.0, 1e3, 1e4, 1e5}) {
        if (ichol(lu, 1e-4, shift, lv, lr, lp)) {
            factored = true;
            break;
        }
    }
    if (!factored) {
        if (error) *error = "incomplete Cholesky failed at every shift";
        return {};
    }

    // pymatting's cg: from zero, to a residual 1e-7 of the right-hand side's.
    std::vector<double> sol(n, 0.0), r = rhs, z(n), p(n), ap(n);
    const double norm_b = std::sqrt(dot(rhs, rhs));
    double norm_r = std::sqrt(dot(r, r));
    // No unknown pixel touches the subject: nothing pulls any of them up, and
    // the answer is zero -- which CG would reach by dividing zero by zero.
    bool converged = norm_b == 0.0 || norm_r < 1e-7 * norm_b;
    if (!converged) {
        z = r;
        cholesky_solve(lv, lr, lp, z);
        p = z;
        double rz = dot(r, z);
        for (int iteration = 0; iteration < 10000; ++iteration) {
            lu.multiply(p, ap);
            const double alpha = rz / dot(p, ap);
            for (std::int64_t i = 0; i < n; ++i) {
                sol[i] += alpha * p[i];
                r[i] -= alpha * ap[i];
            }
            norm_r = std::sqrt(dot(r, r));
            if (norm_r < 1e-7 * norm_b) {
                converged = true;
                break;
            }
            z = r;
            cholesky_solve(lv, lr, lp, z);
            const double rz_new = dot(r, z);
            const double beta = rz_new / rz;
            rz = rz_new;
            for (std::int64_t i = 0; i < n; ++i) p[i] = z[i] + beta * p[i];
        }
    }
    if (!converged) {
        if (error) *error = "conjugate gradients did not converge in 10000 iterations";
        return {};
    }
    for (std::size_t i = 0; i < pixels; ++i)
        if (slot[i] >= 0) x[i] = sol[slot[i]];
    for (double& v : x) v = std::clamp(v, 0.0, 1.0);
    return x;
}

}  // namespace ps::matting
