/*
 * Minimal WangsFormula stub for Blender's GrTriangulator integration.
 * Provides curve subdivision count estimation using Wang's formula.
 */

#ifndef skgpu_tessellate_WangsFormula_DEFINED
#define skgpu_tessellate_WangsFormula_DEFINED

#include "include/core/SkPoint.h"
#include <cmath>
#include <algorithm>

namespace skgpu::wangs_formula {

/**
 * Wang's formula for quadratic Bezier curves.
 * Returns log2 of the number of line segments needed to approximate the curve
 * within the given precision.
 */
inline int quadratic_log2(float precision, const SkPoint pts[3]) {
    if (precision <= 0) {
        return 0;
    }

    /* For a quadratic Bezier P(t) = (1-t)^2*P0 + 2t(1-t)*P1 + t^2*P2,
     * the second derivative is constant: P''(t) = 2*(P0 - 2*P1 + P2)
     * Wang's formula: n = ceil(sqrt(L_inf * precision / 8))
     * where L_inf is the max norm of the second derivative. */
    float ax = pts[0].fX - 2.0f * pts[1].fX + pts[2].fX;
    float ay = pts[0].fY - 2.0f * pts[1].fY + pts[2].fY;

    /* L_inf norm of second derivative */
    float l_inf = std::max(std::abs(ax), std::abs(ay)) * 2.0f;

    if (l_inf <= 0) {
        return 0;
    }

    /* n = sqrt(l_inf * precision / 8) */
    float n = std::sqrt(l_inf * precision * 0.125f);

    /* Return log2(n), clamped to reasonable range */
    if (n <= 1.0f) {
        return 0;
    }
    int log2_n = static_cast<int>(std::ceil(std::log2(n)));
    return std::min(log2_n, 10);  /* Cap at 1024 segments */
}

/**
 * Wang's formula for cubic Bezier curves.
 * Returns log2 of the number of line segments needed to approximate the curve
 * within the given precision.
 */
inline int cubic_log2(float precision, const SkPoint pts[4]) {
    if (precision <= 0) {
        return 0;
    }

    /* For a cubic Bezier, the second derivative varies.
     * We use a conservative estimate based on control point spread.
     * P''(t) = 6*(1-t)*(P1-2*P0+P1) + 6*t*(P2-2*P1+P0) simplified to
     * bounding by max of |P0-2P1+P2| and |P1-2P2+P3| */
    float ax0 = pts[0].fX - 2.0f * pts[1].fX + pts[2].fX;
    float ay0 = pts[0].fY - 2.0f * pts[1].fY + pts[2].fY;
    float ax1 = pts[1].fX - 2.0f * pts[2].fX + pts[3].fX;
    float ay1 = pts[1].fY - 2.0f * pts[2].fY + pts[3].fY;

    /* Take max of both estimates */
    float l0 = std::max(std::abs(ax0), std::abs(ay0));
    float l1 = std::max(std::abs(ax1), std::abs(ay1));
    float l_inf = std::max(l0, l1) * 6.0f;

    if (l_inf <= 0) {
        return 0;
    }

    /* Wang's formula for cubics: n = ceil((l_inf * precision / 18)^(1/3) * n_base)
     * Simplified approximation: n = sqrt(l_inf * precision / 8) */
    float n = std::sqrt(l_inf * precision * 0.125f);

    if (n <= 1.0f) {
        return 0;
    }
    int log2_n = static_cast<int>(std::ceil(std::log2(n)));
    return std::min(log2_n, 10);  /* Cap at 1024 segments */
}

}  // namespace skgpu::wangs_formula

#endif  // skgpu_tessellate_WangsFormula_DEFINED
