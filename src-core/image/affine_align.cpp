#include "affine_align.h"

#include "core/exception.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace satdump
{
    namespace image
    {
        AffineTransform AffineTransform::inverse() const
        {
            const double det = a * d - b * c;
            if (std::fabs(det) < 1e-12)
                throw satdump_exception("Cannot invert a singular affine transform!");

            const double id = 1.0 / det;
            AffineTransform r;
            r.a = d * id;
            r.b = -b * id;
            r.c = -c * id;
            r.d = a * id;
            r.tx = -(r.a * tx + r.b * ty);
            r.ty = -(r.c * tx + r.d * ty);
            return r;
        }

        namespace
        {
            struct FloatPlane
            {
                int w = 0;
                int h = 0;
                std::vector<float> data;

                void init(int width, int height)
                {
                    w = width;
                    h = height;
                    data.assign((size_t)w * (size_t)h, 0.0f);
                }

                inline float &at(int x, int y) { return data[(size_t)y * (size_t)w + (size_t)x]; }
                inline const float &at(int x, int y) const { return data[(size_t)y * (size_t)w + (size_t)x]; }
            };

            /// BORDER_REFLECT_101 index, as used by OpenCV's filters.
            inline int reflect101(int i, int n)
            {
                if (i < 0)
                    i = -i;
                if (i >= n)
                    i = 2 * n - 2 - i;
                return i;
            }

            inline float bilinear(const FloatPlane &src, double x, double y)
            {
                if (!(x >= 0.0 && y >= 0.0 && x <= (double)(src.w - 1) && y <= (double)(src.h - 1)))
                    return 0.0f;

                const int ix = (int)x;
                const int iy = (int)y;
                const double fx = x - ix;
                const double fy = y - iy;
                const int ix1 = std::min(ix + 1, src.w - 1);
                const int iy1 = std::min(iy + 1, src.h - 1);

                const double top = src.at(ix, iy) * (1.0 - fx) + src.at(ix1, iy) * fx;
                const double bot = src.at(ix, iy1) * (1.0 - fx) + src.at(ix1, iy1) * fx;
                return (float)(top * (1.0 - fy) + bot * fy);
            }

            /// Area-average downsampling by an integer factor. Output pixels cover
            /// [i*factor, (i+1)*factor) of the source; the remainder at the right /
            /// bottom edge (if the size is not divisible) is ignored.
            FloatPlane downsampleImage(const Image &img, int factor)
            {
                if (factor < 1)
                    throw satdump_exception("Invalid pyramid factor " + std::to_string(factor));

                const int sw = (int)img.width();
                const int sh = (int)img.height();
                const int dw = std::max(1, sw / factor);
                const int dh = std::max(1, sh / factor);

                FloatPlane out;
                out.init(dw, dh);

                const double maxv = std::max(1, img.maxval());
                const bool is16 = img.depth() > 8;

                if (factor == 1)
                {
                    if (is16)
                    {
                        const uint16_t *src = (const uint16_t *)img.raw_data();
#pragma omp parallel for
                        for (int y = 0; y < dh; y++)
                            for (int x = 0; x < dw; x++)
                                out.at(x, y) = (float)(src[(size_t)y * sw + x] / maxv);
                    }
                    else
                    {
                        const uint8_t *src = (const uint8_t *)img.raw_data();
#pragma omp parallel for
                        for (int y = 0; y < dh; y++)
                            for (int x = 0; x < dw; x++)
                                out.at(x, y) = (float)(src[(size_t)y * sw + x] / maxv);
                    }
                    return out;
                }

#pragma omp parallel for schedule(dynamic, 2)
                for (int y = 0; y < dh; y++)
                {
                    const int y0 = y * factor;
                    const int y1 = std::min(sh, y0 + factor);

                    if (is16)
                    {
                        const uint16_t *base = (const uint16_t *)img.raw_data();
                        for (int x = 0; x < dw; x++)
                        {
                            const int x0 = x * factor;
                            const int x1 = std::min(sw, x0 + factor);
                            uint64_t sum = 0;
                            for (int sy = y0; sy < y1; sy++)
                            {
                                const uint16_t *row = base + (size_t)sy * sw;
                                for (int sx = x0; sx < x1; sx++)
                                    sum += row[sx];
                            }
                            const uint64_t count = (uint64_t)(y1 - y0) * (uint64_t)(x1 - x0);
                            out.at(x, y) = (float)((double)sum / ((double)count * maxv));
                        }
                    }
                    else
                    {
                        const uint8_t *base = (const uint8_t *)img.raw_data();
                        for (int x = 0; x < dw; x++)
                        {
                            const int x0 = x * factor;
                            const int x1 = std::min(sw, x0 + factor);
                            uint64_t sum = 0;
                            for (int sy = y0; sy < y1; sy++)
                            {
                                const uint8_t *row = base + (size_t)sy * sw;
                                for (int sx = x0; sx < x1; sx++)
                                    sum += row[sx];
                            }
                            const uint64_t count = (uint64_t)(y1 - y0) * (uint64_t)(x1 - x0);
                            out.at(x, y) = (float)((double)sum / ((double)count * maxv));
                        }
                    }
                }

                return out;
            }

            /// Per-row "this scanline contains decoded data" lookup table. Sparse
            /// captures (and the empty top/bottom of a scan) otherwise dominate the
            /// correlation with black rows and make the affine fit diverge.
            std::vector<uint8_t> computeValidRows(const Image &img)
            {
                const size_t h = img.height();
                const size_t w = img.width();
                std::vector<uint8_t> rows(h, 0);
                if (h == 0 || w == 0 || img.raw_data() == nullptr)
                    return rows;

                const size_t step = std::max<size_t>(1, w / 512);
                const size_t samples = (w + step - 1) / step;
                const size_t min_nonzero = std::max<size_t>(1, samples / 64); // ~1.5% of the row

                if (img.depth() > 8)
                {
                    const uint16_t *data = (const uint16_t *)img.raw_data();
                    for (size_t y = 0; y < h; y++)
                    {
                        const uint16_t *row = data + y * w;
                        size_t nonzero = 0;
                        for (size_t x = 0; x < w; x += step)
                            if (row[x] != 0)
                                nonzero++;
                        rows[y] = nonzero >= min_nonzero ? 1 : 0;
                    }
                }
                else
                {
                    const uint8_t *data = (const uint8_t *)img.raw_data();
                    for (size_t y = 0; y < h; y++)
                    {
                        const uint8_t *row = data + y * w;
                        size_t nonzero = 0;
                        for (size_t x = 0; x < w; x += step)
                            if (row[x] != 0)
                                nonzero++;
                        rows[y] = nonzero >= min_nonzero ? 1 : 0;
                    }
                }

                return rows;
            }

            /// Merge a full-resolution valid-row table down to a pyramid level.
            std::vector<uint8_t> downsampleValidRows(const std::vector<uint8_t> &rows, int factor, int dst_h)
            {
                std::vector<uint8_t> out((size_t)dst_h, 0);
                for (int y = 0; y < dst_h; y++)
                {
                    const int y0 = y * factor;
                    const int y1 = std::min((int)rows.size(), y0 + factor);
                    for (int sy = y0; sy < y1; sy++)
                        if (rows[(size_t)sy])
                        {
                            out[(size_t)y] = 1;
                            break;
                        }
                }
                return out;
            }

            /// Separable 5x5 Gaussian, sigma = 1.1 (OpenCV's GaussianBlur(Size(5, 5), 0)).
            void gaussianBlur5(FloatPlane &img)
            {
                static const float k[5] = {0.070858797f, 0.244396974f, 0.369488458f, 0.244396974f, 0.070858797f};

                FloatPlane tmp;
                tmp.init(img.w, img.h);

#pragma omp parallel for
                for (int y = 0; y < img.h; y++)
                    for (int x = 0; x < img.w; x++)
                    {
                        float v = 0.0f;
                        for (int i = -2; i <= 2; i++)
                            v += k[i + 2] * img.at(reflect101(x + i, img.w), y);
                        tmp.at(x, y) = v;
                    }

#pragma omp parallel for
                for (int y = 0; y < img.h; y++)
                    for (int x = 0; x < img.w; x++)
                    {
                        float v = 0.0f;
                        for (int i = -2; i <= 2; i++)
                            v += k[i + 2] * tmp.at(x, reflect101(y + i, img.h));
                        img.at(x, y) = v;
                    }
            }

            /// Central-difference gradients, filter2D with [-0.5, 0, 0.5] / BORDER_REFLECT_101.
            void computeGradients(const FloatPlane &img, FloatPlane &gx, FloatPlane &gy)
            {
                gx.init(img.w, img.h);
                gy.init(img.w, img.h);

#pragma omp parallel for
                for (int y = 0; y < img.h; y++)
                    for (int x = 0; x < img.w; x++)
                    {
                        gx.at(x, y) = 0.5f * (img.at(reflect101(x + 1, img.w), y) - img.at(reflect101(x - 1, img.w), y));
                        gy.at(x, y) = 0.5f * (img.at(x, reflect101(y + 1, img.h)) - img.at(x, reflect101(y - 1, img.h)));
                    }
            }

            /// Warp src into dst (both w*h) using map, bilinear, 0 outside.
            void warpPlane(const FloatPlane &src, const AffineTransform &map, FloatPlane &dst,
                           std::vector<uint8_t> *mask, bool mask_out_of_bounds)
            {
                dst.init(src.w, src.h);
                if (mask)
                    mask->assign((size_t)src.w * (size_t)src.h, 0);

#pragma omp parallel for schedule(static)
                for (int y = 0; y < src.h; y++)
                {
                    const double row_x = map.b * (double)y + map.tx;
                    const double row_y = map.d * (double)y + map.ty;
                    uint8_t *mrow = mask ? (mask->data() + (size_t)y * src.w) : nullptr;

                    for (int x = 0; x < src.w; x++)
                    {
                        const double sx = map.a * (double)x + row_x;
                        const double sy = map.c * (double)x + row_y;

                        const bool inside = sx >= 0.0 && sy >= 0.0 && sx <= (double)(src.w - 1) && sy <= (double)(src.h - 1);
                        if (!inside && mask_out_of_bounds)
                            continue;

                        dst.at(x, y) = bilinear(src, sx, sy);
                        if (mrow)
                            mrow[x] = (inside || !mask_out_of_bounds) ? 1 : 0;
                    }
                }
            }

            /// Convert an affine in source coordinates to one in the coordinates of an
            /// integer-factor area-downsampled image. See the comment at the call site.
            AffineTransform toLevel(const AffineTransform &src, int factor)
            {
                const double c = 0.5 * (double)(factor - 1);
                AffineTransform out;
                out.a = src.a;
                out.b = src.b;
                out.c = src.c;
                out.d = src.d;
                out.tx = (src.tx + src.a * c + src.b * c - c) / (double)factor;
                out.ty = (src.ty + src.c * c + src.d * c - c) / (double)factor;
                return out;
            }

            AffineTransform fromLevel(const AffineTransform &level, int factor)
            {
                const double c = 0.5 * (double)(factor - 1);
                AffineTransform out;
                out.a = level.a;
                out.b = level.b;
                out.c = level.c;
                out.d = level.d;
                out.tx = (double)factor * level.tx + c - (level.a * c + level.b * c);
                out.ty = (double)factor * level.ty + c - (level.c * c + level.d * c);
                return out;
            }

            /// Invert an n x n matrix (n <= 6) with Gauss-Jordan / partial pivoting.
            bool invertN(const double m[6][6], double out[6][6], int n)
            {
                double a[6][12] = {};
                for (int r = 0; r < n; r++)
                    for (int c = 0; c < n; c++)
                    {
                        a[r][c] = m[r][c];
                        a[r][n + c] = (r == c) ? 1.0 : 0.0;
                    }

                for (int col = 0; col < n; col++)
                {
                    int pivot = col;
                    double best = std::fabs(a[col][col]);
                    for (int r = col + 1; r < n; r++)
                        if (std::fabs(a[r][col]) > best)
                        {
                            best = std::fabs(a[r][col]);
                            pivot = r;
                        }

                    if (!(best > 1e-18))
                        return false;

                    if (pivot != col)
                        for (int c = 0; c < 2 * n; c++)
                            std::swap(a[col][c], a[pivot][c]);

                    const double inv = 1.0 / a[col][col];
                    for (int c = 0; c < 2 * n; c++)
                        a[col][c] *= inv;

                    for (int r = 0; r < n; r++)
                    {
                        if (r == col)
                            continue;
                        const double f = a[r][col];
                        if (f == 0.0)
                            continue;
                        for (int c = 0; c < 2 * n; c++)
                            a[r][c] -= f * a[col][c];
                    }
                }

                for (int r = 0; r < n; r++)
                    for (int c = 0; c < n; c++)
                        out[r][c] = a[r][n + c];
                return true;
            }

            struct ECCTotals
            {
                double h[6][6] = {};
                double image_proj[6] = {};
                double template_proj[6] = {};
                double dot = 0.0;
                double template_sq = 0.0;
                double image_sq = 0.0;
            };

            /**
             * @brief One pyramid level of ECC iterations.
             *
             * Follows OpenCV's cv::findTransformECC affine update (including the
             * illumination term lambda) so results are comparable with the old tool.
             * Returns the last correlation coefficient, or NaN if the level failed.
             */
            double eccIterate(const FloatPlane &tmpl, const FloatPlane &img,
                              const FloatPlane &gx, const FloatPlane &gy,
                              const std::vector<uint8_t> &ref_rows,
                              AffineTransform &map, int iterations, double epsilon, bool mask_out_of_bounds,
                              bool estimate_linear)
            {
                const int w = tmpl.w;
                const int h = tmpl.h;
                const int np = estimate_linear ? 6 : 2;

                FloatPlane warped, gxw, gyw;
                std::vector<uint8_t> mask;

                const double origin_tx = map.tx;
                const double origin_ty = map.ty;
                double rho = -1.0;
                double last_rho = -epsilon;

                for (int iter = 0; iter < iterations; iter++)
                {
                    warpPlane(img, map, warped, &mask, mask_out_of_bounds);
                    warpPlane(gx, map, gxw, nullptr, mask_out_of_bounds);
                    warpPlane(gy, map, gyw, nullptr, mask_out_of_bounds);

                    // Only compare scanlines that carry data in the reference. Do not
                    // mask by the warped target's own validity: that makes the mask part
                    // of the objective, and ECC then "optimises" by warping into gaps.
                    if (!ref_rows.empty())
                    {
#pragma omp parallel for schedule(static)
                        for (int y = 0; y < h; y++)
                            if (!ref_rows[(size_t)y])
                                memset(&mask[(size_t)y * w], 0, (size_t)w);
                    }

                    // Means over the valid (mapped-inside) pixels.
                    double sum_t = 0.0, sum_i = 0.0;
                    long long count = 0;
#pragma omp parallel for reduction(+ : sum_t, sum_i, count) schedule(static)
                    for (int y = 0; y < h; y++)
                    {
                        const float *trow = &tmpl.at(0, y);
                        const float *irow = &warped.at(0, y);
                        const uint8_t *mrow = &mask[(size_t)y * w];
                        for (int x = 0; x < w; x++)
                            if (mrow[x])
                            {
                                sum_t += trow[x];
                                sum_i += irow[x];
                                count++;
                            }
                    }

                    if (count < 256)
                        return std::numeric_limits<double>::quiet_NaN();

                    const double mean_t = sum_t / (double)count;
                    const double mean_i = sum_i / (double)count;

                    const int nthreads =
#ifdef _OPENMP
                        std::max(1, omp_get_max_threads());
#else
                        1;
#endif
                    std::vector<ECCTotals> totals((size_t)nthreads);

#pragma omp parallel num_threads(nthreads)
                    {
#ifdef _OPENMP
                        ECCTotals &acc = totals[(size_t)omp_get_thread_num()];
#else
                        ECCTotals &acc = totals[0];
#endif
#pragma omp for schedule(static)
                        for (int y = 0; y < h; y++)
                        {
                            const float *trow = &tmpl.at(0, y);
                            const float *irow = &warped.at(0, y);
                            const float *gxrow = &gxw.at(0, y);
                            const float *gyrow = &gyw.at(0, y);
                            const uint8_t *mrow = &mask[(size_t)y * w];

                            for (int x = 0; x < w; x++)
                            {
                                if (!mrow[x])
                                    continue;

                                const double tz = (double)trow[x] - mean_t;
                                const double iz = (double)irow[x] - mean_i;

                                double j[6] = {};
                                if (estimate_linear)
                                {
                                    j[0] = (double)gxrow[x] * (double)x;
                                    j[1] = (double)gyrow[x] * (double)x;
                                    j[2] = (double)gxrow[x] * (double)y;
                                    j[3] = (double)gyrow[x] * (double)y;
                                    j[4] = (double)gxrow[x];
                                    j[5] = (double)gyrow[x];
                                }
                                else
                                {
                                    j[0] = (double)gxrow[x];
                                    j[1] = (double)gyrow[x];
                                }

                                for (int r = 0; r < np; r++)
                                {
                                    acc.image_proj[r] += j[r] * iz;
                                    acc.template_proj[r] += j[r] * tz;
                                    for (int c = r; c < np; c++)
                                        acc.h[r][c] += j[r] * j[c];
                                }

                                acc.dot += tz * iz;
                                acc.template_sq += tz * tz;
                                acc.image_sq += iz * iz;
                            }
                        }
                    }

                    ECCTotals tot;
                    for (const ECCTotals &acc : totals)
                    {
                        for (int r = 0; r < np; r++)
                        {
                            tot.image_proj[r] += acc.image_proj[r];
                            tot.template_proj[r] += acc.template_proj[r];
                            for (int c = r; c < np; c++)
                                tot.h[r][c] += acc.h[r][c];
                        }
                        tot.dot += acc.dot;
                        tot.template_sq += acc.template_sq;
                        tot.image_sq += acc.image_sq;
                    }

                    for (int r = 0; r < np; r++)
                        for (int c = 0; c < r; c++)
                            tot.h[r][c] = tot.h[c][r];

                    const double template_norm = std::sqrt(tot.template_sq);
                    const double image_norm = std::sqrt(tot.image_sq);
                    if (!(template_norm > 0.0) || !(image_norm > 0.0))
                        return std::numeric_limits<double>::quiet_NaN();

                    last_rho = rho;
                    const double correlation = tot.dot; // raw zero-mean dot product (OpenCV's "correlation")
                    rho = correlation / (template_norm * image_norm);
                    if (!std::isfinite(rho))
                        return std::numeric_limits<double>::quiet_NaN();

                    double h_inv[6][6];
                    if (!invertN(tot.h, h_inv, np))
                        return std::numeric_limits<double>::quiet_NaN();

                    double h_inv_image[6] = {};
                    for (int r = 0; r < np; r++)
                        for (int c = 0; c < np; c++)
                            h_inv_image[r] += h_inv[r][c] * tot.image_proj[c];

                    const double lambda_n = tot.image_sq - [&] {
                        double d = 0.0;
                        for (int r = 0; r < np; r++)
                            d += tot.image_proj[r] * h_inv_image[r];
                        return d;
                    }();

                    const double lambda_d = correlation - [&] {
                        double d = 0.0;
                        for (int r = 0; r < np; r++)
                            d += tot.template_proj[r] * h_inv_image[r];
                        return d;
                    }();

                    if (!(lambda_d > 0.0))
                        return std::numeric_limits<double>::quiet_NaN();

                    const double lambda = lambda_n / lambda_d;

                    double err_proj[6] = {};
                    for (int r = 0; r < np; r++)
                        err_proj[r] = lambda * tot.template_proj[r] - tot.image_proj[r];

                    double dp[6] = {};
                    for (int r = 0; r < np; r++)
                        for (int c = 0; c < np; c++)
                            dp[r] += h_inv[r][c] * err_proj[c];

                    // OpenCV's affine parameter order: a, c, b, d, tx, ty.
                    if (estimate_linear)
                    {
                        map.a += dp[0];
                        map.c += dp[1];
                        map.b += dp[2];
                        map.d += dp[3];
                        map.tx += dp[4];
                        map.ty += dp[5];
                    }
                    else
                    {
                        map.tx += dp[0];
                        map.ty += dp[1];
                    }

                    // Keep the fit within the physically plausible MSU-GS envelope.
                    const auto clampd = [](double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); };
                    map.a = clampd(map.a, 0.99, 1.01);
                    map.d = clampd(map.d, 0.99, 1.01);
                    map.b = clampd(map.b, -0.012, 0.012);
                    map.c = clampd(map.c, -0.012, 0.012);
                    map.tx = clampd(map.tx, origin_tx - 512.0, origin_tx + 512.0);
                    map.ty = clampd(map.ty, origin_ty - 512.0, origin_ty + 512.0);

                    if (!std::isfinite(map.a) || !std::isfinite(map.b) || !std::isfinite(map.tx) ||
                        !std::isfinite(map.c) || !std::isfinite(map.d) || !std::isfinite(map.ty))
                        return std::numeric_limits<double>::quiet_NaN();

                    if (std::fabs(rho - last_rho) < epsilon)
                        break;
                }

                return rho;
            }
        } // namespace

        double findAffineTransformECC(const Image &reference, const Image &target, AffineTransform &transform,
                                      const AffineECCOptions &options)
        {
            if (reference.channels() != 1 || target.channels() != 1)
                throw satdump_exception("ECC alignment requires single-channel images");
            if (reference.width() != target.width() || reference.height() != target.height())
                throw satdump_exception("ECC alignment requires images of the same size");
            if (reference.width() < 16 || reference.height() < 16)
                throw satdump_exception("ECC alignment requires images of at least 16x16");
            if (options.pyramid_factors.empty() || options.iterations.empty())
                throw satdump_exception("ECC alignment requires a pyramid");

            const double nan = std::numeric_limits<double>::quiet_NaN();
            AffineTransform map = transform;
            double rho = nan;

            const std::vector<uint8_t> full_ref_rows =
                options.use_row_mask ? computeValidRows(reference) : std::vector<uint8_t>();

            for (size_t level = 0; level < options.pyramid_factors.size(); level++)
            {
                const int factor = options.pyramid_factors[level];
                const int iterations = options.iterations[std::min(level, options.iterations.size() - 1)];

                FloatPlane ref_l = downsampleImage(reference, factor);
                FloatPlane tgt_l = downsampleImage(target, factor);

                if (options.gaussian_blur)
                {
                    gaussianBlur5(ref_l);
                    gaussianBlur5(tgt_l);
                }

                if (ref_l.w < 16 || ref_l.h < 16)
                    return nan;

                FloatPlane gx, gy;
                computeGradients(tgt_l, gx, gy);

                const std::vector<uint8_t> ref_rows =
                    full_ref_rows.empty() ? std::vector<uint8_t>()
                                          : downsampleValidRows(full_ref_rows, factor, ref_l.h);

                AffineTransform level_map = toLevel(map, factor);
                rho = eccIterate(ref_l, tgt_l, gx, gy, ref_rows, level_map, iterations, options.epsilon,
                                 options.mask_out_of_bounds, options.estimate_linear_part);
                if (!std::isfinite(rho))
                    return nan;

                map = fromLevel(level_map, factor);

                // Reject fits that warp the layers by more than a few percent.
                if (std::fabs(map.a - 1.0) > 0.02 || std::fabs(map.d - 1.0) > 0.02 ||
                    std::fabs(map.b) > 0.015 || std::fabs(map.c) > 0.015)
                {
                    return nan;
                }
            }

            transform = map;
            return rho;
        }

        Image warpAffine(const Image &input, const AffineTransform &transform, size_t out_width, size_t out_height)
        {
            if (input.channels() != 1)
                throw satdump_exception("Affine warp requires a single-channel image");

            Image out(input.depth(), out_width, out_height, 1);

            const int sw = (int)input.width();
            const int sh = (int)input.height();
            const int ow = (int)out_width;
            const int oh = (int)out_height;
            const int maxv = input.maxval();
            const bool is16 = input.depth() > 8;

            if (is16)
            {
                const uint16_t *src = (const uint16_t *)input.raw_data();
                uint16_t *dst = (uint16_t *)out.raw_data();

#pragma omp parallel for schedule(static)
                for (int y = 0; y < oh; y++)
                {
                    const double row_x = transform.b * (double)y + transform.tx;
                    const double row_y = transform.d * (double)y + transform.ty;

                    for (int x = 0; x < ow; x++)
                    {
                        const double sx = transform.a * (double)x + row_x;
                        const double sy = transform.c * (double)x + row_y;

                        if (!(sx >= 0.0 && sy >= 0.0 && sx <= (double)(sw - 1) && sy <= (double)(sh - 1)))
                        {
                            dst[(size_t)y * ow + x] = 0;
                            continue;
                        }

                        const int ix = (int)sx;
                        const int iy = (int)sy;
                        const double fx = sx - ix;
                        const double fy = sy - iy;
                        const int ix1 = std::min(ix + 1, sw - 1);
                        const int iy1 = std::min(iy + 1, sh - 1);

                        const double top = src[(size_t)iy * sw + ix] * (1.0 - fx) + src[(size_t)iy * sw + ix1] * fx;
                        const double bot = src[(size_t)iy1 * sw + ix] * (1.0 - fx) + src[(size_t)iy1 * sw + ix1] * fx;
                        double v = top * (1.0 - fy) + bot * fy;

                        if (v < 0.0)
                            v = 0.0;
                        if (v > (double)maxv)
                            v = (double)maxv;
                        dst[(size_t)y * ow + x] = (uint16_t)(v + 0.5);
                    }
                }
            }
            else
            {
                const uint8_t *src = (const uint8_t *)input.raw_data();
                uint8_t *dst = (uint8_t *)out.raw_data();

#pragma omp parallel for schedule(static)
                for (int y = 0; y < oh; y++)
                {
                    const double row_x = transform.b * (double)y + transform.tx;
                    const double row_y = transform.d * (double)y + transform.ty;

                    for (int x = 0; x < ow; x++)
                    {
                        const double sx = transform.a * (double)x + row_x;
                        const double sy = transform.c * (double)x + row_y;

                        if (!(sx >= 0.0 && sy >= 0.0 && sx <= (double)(sw - 1) && sy <= (double)(sh - 1)))
                        {
                            dst[(size_t)y * ow + x] = 0;
                            continue;
                        }

                        const int ix = (int)sx;
                        const int iy = (int)sy;
                        const double fx = sx - ix;
                        const double fy = sy - iy;
                        const int ix1 = std::min(ix + 1, sw - 1);
                        const int iy1 = std::min(iy + 1, sh - 1);

                        const double top = src[(size_t)iy * sw + ix] * (1.0 - fx) + src[(size_t)iy * sw + ix1] * fx;
                        const double bot = src[(size_t)iy1 * sw + ix] * (1.0 - fx) + src[(size_t)iy1 * sw + ix1] * fx;
                        double v = top * (1.0 - fy) + bot * fy;

                        if (v < 0.0)
                            v = 0.0;
                        if (v > (double)maxv)
                            v = (double)maxv;
                        dst[(size_t)y * ow + x] = (uint8_t)(v + 0.5);
                    }
                }
            }

            return out;
        }
    } // namespace image
} // namespace satdump
