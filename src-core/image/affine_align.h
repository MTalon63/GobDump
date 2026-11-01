#pragma once

#include "image/image.h"

#include <cstddef>
#include <vector>

namespace satdump
{
    namespace image
    {
        /// 2x3 affine transform mapping destination to source coordinates:
        /// src = (a*x + b*y + tx, c*x + d*y + ty).
        struct AffineTransform
        {
            double a = 1.0, b = 0.0, tx = 0.0;
            double c = 0.0, d = 1.0, ty = 0.0;

            inline void forward(double *x, double *y) const
            {
                const double ox = *x;
                *x = a * ox + b * *y + tx;
                *y = c * ox + d * *y + ty;
            }

            AffineTransform inverse() const;
        };

        struct AffineECCOptions
        {
            std::vector<int> pyramid_factors = {16, 8, 4, 2};
            std::vector<int> iterations = {100, 60, 40, 30};
            double epsilon = 1e-6;
            bool gaussian_blur = true;
            bool mask_out_of_bounds = true;   ///< Ignore pixels mapped outside the target.
            bool use_row_mask = true;         ///< Compare only scanlines with data in both images.
            bool estimate_linear_part = true; ///< Fit scale/rotation/shear as well as translation.
        };

        /// ECC affine fit: target(transform * p) ~= reference(p).
        /// Returns the final correlation coefficient, or NaN on failure.
        double findAffineTransformECC(const Image &reference, const Image &target, AffineTransform &transform,
                                      const AffineECCOptions &options = AffineECCOptions());

        /// Warp with out(x,y) = input(transform * (x,y)), bilinear, zero outside.
        Image warpAffine(const Image &input, const AffineTransform &transform, size_t out_width, size_t out_height);
    } // namespace image
} // namespace satdump
