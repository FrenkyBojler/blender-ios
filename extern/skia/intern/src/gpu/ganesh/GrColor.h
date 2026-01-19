/*
 * Minimal GrColor for Blender's GrTriangulator integration.
 * Stripped of GPU-specific dependencies.
 */

#ifndef GrColor_DEFINED
#define GrColor_DEFINED

#include "include/core/SkColor.h"
#include <cstdint>

/**
 * GrColor is 4 bytes for R, G, B, A.
 */
typedef uint32_t GrColor;

// shift amount to assign a component to a GrColor int
#ifdef SK_CPU_BENDIAN
    #define GrColor_SHIFT_R     24
    #define GrColor_SHIFT_G     16
    #define GrColor_SHIFT_B     8
    #define GrColor_SHIFT_A     0
#else
    #define GrColor_SHIFT_R     0
    #define GrColor_SHIFT_G     8
    #define GrColor_SHIFT_B     16
    #define GrColor_SHIFT_A     24
#endif

static inline GrColor GrColorPackRGBA(unsigned r, unsigned g, unsigned b, unsigned a) {
    return (r << GrColor_SHIFT_R) |
           (g << GrColor_SHIFT_G) |
           (b << GrColor_SHIFT_B) |
           (a << GrColor_SHIFT_A);
}

#define GrColorUnpackR(color)   (((color) >> GrColor_SHIFT_R) & 0xFF)
#define GrColorUnpackG(color)   (((color) >> GrColor_SHIFT_G) & 0xFF)
#define GrColorUnpackB(color)   (((color) >> GrColor_SHIFT_B) & 0xFF)
#define GrColorUnpackA(color)   (((color) >> GrColor_SHIFT_A) & 0xFF)

#define GrColor_ILLEGAL     (~(0xFF << GrColor_SHIFT_A))

static inline float GrNormalizeByteToFloat(uint8_t value) {
    static const float ONE_OVER_255 = 1.f / 255.f;
    return value * ONE_OVER_255;
}

#endif  // GrColor_DEFINED
