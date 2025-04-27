/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_assert.h"
#include "BLI_sys_types.h"

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name Standard Formats
 *
 * Defined programmatically using macros to avoid copy paste mistake and allow easy modification.
 * \{ */

/* Blender enum. */
#define FORMAT_BL_1(type, bits) type##_##bits
#define FORMAT_BL_2(type, bits) type##_##bits##_##bits
#define FORMAT_BL_3(type, bits) type##_##bits##_##bits##_##bits
#define FORMAT_BL_4(type, bits) type##_##bits##_##bits##_##bits##_##bits

/* Vulkan enum. */
#define FORMAT_VK_1(type, bits) R##bits##_##type
#define FORMAT_VK_2(type, bits) R##bits##G##bits##_##type
#define FORMAT_VK_3(type, bits) R##bits##G##bits##B##bits##_##type
#define FORMAT_VK_4(type, bits) R##bits##G##bits##B##bits##A##bits##_##type

/* Metal enum. */
#define FORMAT_MTL_VERT_SNORM8(comp) Char##comp##Normalized
#define FORMAT_MTL_VERT_SNORM16(comp) Short##comp##Normalized
#define FORMAT_MTL_VERT_UNORM8(comp) UChar##comp##Normalized
#define FORMAT_MTL_VERT_UNORM16(comp) UShort##comp##Normalized
#define FORMAT_MTL_VERT_SINT8(comp) Char##comp
#define FORMAT_MTL_VERT_SINT16(comp) Short##comp
#define FORMAT_MTL_VERT_SINT32(comp) Int##comp
#define FORMAT_MTL_VERT_UINT8(comp) UChar##comp
#define FORMAT_MTL_VERT_UINT16(comp) UShort##comp
#define FORMAT_MTL_VERT_UINT32(comp) UInt##comp
#define FORMAT_MTL_VERT_SFLOAT16(comp) Half##comp
#define FORMAT_MTL_VERT_SFLOAT32(comp) Float##comp
#define FORMAT_MTL_VERT(type, bits, comp) FORMAT_MTL_VERT_##type##bits(comp)

#define FORMAT_MTL_TEX_SNORM(prefix) prefix##Snorm
#define FORMAT_MTL_TEX_UNORM(prefix) prefix##Unorm
#define FORMAT_MTL_TEX_SINT(prefix) prefix##Sint
#define FORMAT_MTL_TEX_UINT(prefix) prefix##Uint
#define FORMAT_MTL_TEX_SFLOAT(prefix) prefix##Float

#define FORMAT_MTL_TEX_1(type, bits) FORMAT_MTL_TEX_##type(R##bits)
#define FORMAT_MTL_TEX_2(type, bits) FORMAT_MTL_TEX_##type(RG##bits)
#define FORMAT_MTL_TEX_3(type, bits) FORMAT_MTL_TEX_##type(RGB##bits)
#define FORMAT_MTL_TEX_4(type, bits) FORMAT_MTL_TEX_##type(RGBA##bits)

/* GL enum. */
#define FORMAT_GL_SNORM(prefix) prefix##_SNORM
#define FORMAT_GL_UNORM(prefix) prefix
#define FORMAT_GL_SINT(prefix) prefix##I
#define FORMAT_GL_UINT(prefix) prefix##U
#define FORMAT_GL_SFLOAT(prefix) prefix##F

#define FORMAT_GL_1(type, bits) FORMAT_GL_##type(R##bits)
#define FORMAT_GL_2(type, bits) FORMAT_GL_##type(RG##bits)
#define FORMAT_GL_3(type, bits) FORMAT_GL_##type(RGB##bits)
#define FORMAT_GL_4(type, bits) FORMAT_GL_##type(RGBA##bits)

/* Shader enum. */
#define FORMAT_SH_UNORM(prefix) prefix##_unorm
#define FORMAT_SH_SNORM(prefix) prefix##_snorm
#define FORMAT_SH_UINT(prefix) prefix##_uint
#define FORMAT_SH_SINT(prefix) prefix##_sint
#define FORMAT_SH_SFLOAT(prefix) prefix##_sfloat

#define FORMAT_SH_1(type, bits) FORMAT_SH_##type(r##bits)
#define FORMAT_SH_2(type, bits) FORMAT_SH_##type(rg##bits)
#define FORMAT_SH_3(type, bits) FORMAT_SH_##type(rgb##bits)
#define FORMAT_SH_4(type, bits) FORMAT_SH_##type(rgba##bits)

/* Standard format declaration. */
#define FORMAT_CAT(a, b) a##b
#define FORMAT_GLUE(a, b) a b
#define FORMAT_VECTOR(impl, typename, type, bits, comp) \
  impl(typename, \
       (bits * comp) / 8, \
       comp, \
       FORMAT_GLUE(FORMAT_BL_##comp, (type, bits)), \
       FORMAT_GLUE(FORMAT_VK_##comp, (type, bits)), \
       FORMAT_GLUE(FORMAT_MTL_TEX_##comp, (type, bits)), \
       FORMAT_MTL_VERT(type, bits, comp), \
       FORMAT_GLUE(FORMAT_GL_##comp, (type, bits)), \
       FORMAT_GLUE(FORMAT_SH_##comp, (type, bits)))

/* clang-format off */
#define SNORM_8_(impl)             FORMAT_VECTOR(impl, /*TODO*/,  SNORM,  8,  1)
#define SNORM_8_8_(impl)           FORMAT_VECTOR(impl, /*TODO*/,  SNORM,  8,  2)
#define SNORM_8_8_8_(impl)         FORMAT_VECTOR(impl, /*TODO*/,  SNORM,  8,  3)
#define SNORM_8_8_8_8_(impl)       FORMAT_VECTOR(impl, /*TODO*/,  SNORM,  8,  4)

#define SNORM_16_(impl)            FORMAT_VECTOR(impl, /*TODO*/,  SNORM,  16, 1)
#define SNORM_16_16_(impl)         FORMAT_VECTOR(impl, /*TODO*/,  SNORM,  16, 2)
#define SNORM_16_16_16_(impl)      FORMAT_VECTOR(impl, /*TODO*/,  SNORM,  16, 3)
#define SNORM_16_16_16_16_(impl)   FORMAT_VECTOR(impl, /*TODO*/,  SNORM,  16, 4)

#define UNORM_8_(impl)             FORMAT_VECTOR(impl, /*TODO*/,  UNORM,  8,  1)
#define UNORM_8_8_(impl)           FORMAT_VECTOR(impl, /*TODO*/,  UNORM,  8,  2)
#define UNORM_8_8_8_(impl)         FORMAT_VECTOR(impl, /*TODO*/,  UNORM,  8,  3)
#define UNORM_8_8_8_8_(impl)       FORMAT_VECTOR(impl, /*TODO*/,  UNORM,  8,  4)

#define UNORM_16_(impl)            FORMAT_VECTOR(impl, /*TODO*/,  UNORM,  16, 1)
#define UNORM_16_16_(impl)         FORMAT_VECTOR(impl, /*TODO*/,  UNORM,  16, 2)
#define UNORM_16_16_16_(impl)      FORMAT_VECTOR(impl, /*TODO*/,  UNORM,  16, 3)
#define UNORM_16_16_16_16_(impl)   FORMAT_VECTOR(impl, /*TODO*/,  UNORM,  16, 4)

#define SINT_8_(impl)              FORMAT_VECTOR(impl, int8_t,    SINT,   8,  1)
#define SINT_8_8_(impl)            FORMAT_VECTOR(impl, char2,     SINT,   8,  2)
#define SINT_8_8_8_(impl)          FORMAT_VECTOR(impl, char3,     SINT,   8,  3)
#define SINT_8_8_8_8_(impl)        FORMAT_VECTOR(impl, char4,     SINT,   8,  4)

#define SINT_16_(impl)             FORMAT_VECTOR(impl, int16_t,   SINT,   16, 1)
#define SINT_16_16_(impl)          FORMAT_VECTOR(impl, short2,    SINT,   16, 2)
#define SINT_16_16_16_(impl)       FORMAT_VECTOR(impl, short3,    SINT,   16, 3)
#define SINT_16_16_16_16_(impl)    FORMAT_VECTOR(impl, short4,    SINT,   16, 4)

#define SINT_32_(impl)             FORMAT_VECTOR(impl, int32_t,   SINT,   32, 1)
#define SINT_32_32_(impl)          FORMAT_VECTOR(impl, int2,      SINT,   32, 2)
#define SINT_32_32_32_(impl)       FORMAT_VECTOR(impl, int3,      SINT,   32, 3)
#define SINT_32_32_32_32_(impl)    FORMAT_VECTOR(impl, int4,      SINT,   32, 4)

#define UINT_8_(impl)              FORMAT_VECTOR(impl, uint8_t,   UINT,   8,  1)
#define UINT_8_8_(impl)            FORMAT_VECTOR(impl, uhar2,     UINT,   8,  2)
#define UINT_8_8_8_(impl)          FORMAT_VECTOR(impl, uhar3,     UINT,   8,  3)
#define UINT_8_8_8_8_(impl)        FORMAT_VECTOR(impl, uhar4,     UINT,   8,  4)

#define UINT_16_(impl)             FORMAT_VECTOR(impl, uint16_t,  UINT,   16, 1)
#define UINT_16_16_(impl)          FORMAT_VECTOR(impl, ushort2,   UINT,   16, 2)
#define UINT_16_16_16_(impl)       FORMAT_VECTOR(impl, ushort3,   UINT,   16, 3)
#define UINT_16_16_16_16_(impl)    FORMAT_VECTOR(impl, ushort4,   UINT,   16, 4)

#define UINT_32_(impl)             FORMAT_VECTOR(impl, uint32_t,  UINT,   32, 1)
#define UINT_32_32_(impl)          FORMAT_VECTOR(impl, uint2,     UINT,   32, 2)
#define UINT_32_32_32_(impl)       FORMAT_VECTOR(impl, uint3,     UINT,   32, 3)
#define UINT_32_32_32_32_(impl)    FORMAT_VECTOR(impl, uint4,     UINT,   32, 4)

#define SFLOAT_16_(impl)           FORMAT_VECTOR(impl, /* n/a */, SFLOAT, 16, 1)
#define SFLOAT_16_16_(impl)        FORMAT_VECTOR(impl, /* n/a */, SFLOAT, 16, 2)
#define SFLOAT_16_16_16_(impl)     FORMAT_VECTOR(impl, /* n/a */, SFLOAT, 16, 3)
#define SFLOAT_16_16_16_16_(impl)  FORMAT_VECTOR(impl, /* n/a */, SFLOAT, 16, 4)

#define SFLOAT_32_(impl)           FORMAT_VECTOR(impl, float,     SFLOAT, 32, 1)
#define SFLOAT_32_32_(impl)        FORMAT_VECTOR(impl, float2,    SFLOAT, 32, 2)
#define SFLOAT_32_32_32_(impl)     FORMAT_VECTOR(impl, float3,    SFLOAT, 32, 3)
#define SFLOAT_32_32_32_32_(impl)  FORMAT_VECTOR(impl, float4,    SFLOAT, 32, 4)
/* clang-format on */

/* All standard format with 1 component. */
#define VEC_X_(impl) \
  SNORM_8_(impl) \
  SNORM_16_(impl) \
  UNORM_8_(impl) \
  UNORM_16_(impl) \
  SINT_8_(impl) \
  SINT_16_(impl) \
  SINT_32_(impl) \
  UINT_8_(impl) \
  UINT_16_(impl) \
  UINT_32_(impl) \
  SFLOAT_16_(impl) \
  SFLOAT_32_(impl)

/* All standard format with 2 components. */
#define VEC_X_X_(impl) \
  SNORM_8_8_(impl) \
  SNORM_16_16_(impl) \
  UNORM_8_8_(impl) \
  UNORM_16_16_(impl) \
  SINT_8_8_(impl) \
  SINT_16_16_(impl) \
  SINT_32_32_(impl) \
  UINT_8_8_(impl) \
  UINT_16_16_(impl) \
  UINT_32_32_(impl) \
  SFLOAT_16_16_(impl) \
  SFLOAT_32_32_(impl)

/* All standard format with 3 components. */
#define VEC_X_X_X_(impl) \
  SNORM_8_8_8_(impl) \
  SNORM_16_16_16_(impl) \
  UNORM_8_8_8_(impl) \
  UNORM_16_16_16_(impl) \
  SINT_8_8_8_(impl) \
  SINT_16_16_16_(impl) \
  SINT_32_32_32_(impl) \
  UINT_8_8_8_(impl) \
  UINT_16_16_16_(impl) \
  UINT_32_32_32_(impl) \
  SFLOAT_16_16_16_(impl) \
  SFLOAT_32_32_32_(impl)

/* All standard format with 4 components. */
#define VEC_X_X_X_X_(impl) \
  SNORM_8_8_8_8_(impl) \
  SNORM_16_16_16_16_(impl) \
  UNORM_8_8_8_8_(impl) \
  UNORM_16_16_16_16_(impl) \
  SINT_8_8_8_8_(impl) \
  SINT_16_16_16_16_(impl) \
  SINT_32_32_32_32_(impl) \
  UINT_8_8_8_8_(impl) \
  UINT_16_16_16_16_(impl) \
  UINT_32_32_32_32_(impl) \
  SFLOAT_16_16_16_16_(impl) \
  SFLOAT_32_32_32_32_(impl)

/** \} */

/* -------------------------------------------------------------------- */
/** \name Special Formats
 *
 * These formats are too specific and are better written manually.
 * \{ */

/* clang-format off */
/*                                           type       size comps blender_enum            vk_enum                   mtl_pixel_enum         mtl_vertex_enum        gl_pixel_enum                        shader_enum  */
#define SNORM_10_10_10_2_(impl)         impl(/*TODO*/,  4,   4,    SNORM_10_10_10_2,       A2R10G10B10_SNORM_PACK32, /* n/a */,             Int1010102Normalized,  /* n/a */,                           /* n/a */       )
#define UNORM_10_10_10_2_(impl)         impl(/*TODO*/,  4,   4,    UNORM_10_10_10_2,       A2R10G10B10_UNORM_PACK32, RGB10A2Unorm,          UInt1010102Normalized, RGB10_A2,                            rgb10_a2_unorm  )
#define UINT_10_10_10_2_(impl)          impl(/*TODO*/,  4,   4,    UINT_10_10_10_2,        A2R10G10B10_UINT_PACK32,  RGB10A2Uint,           /* n/a */,             RGB10_A2UI,                          rgb10_a2_uint   )
/*                                           type    size comps blender_enum            vk_enum                   mtl_pixel_enum         mtl_vertex_enum        gl_pixel_enum                        shader_enum  */
#define UFLOAT_11_11_10_(impl)          impl(/*TODO*/,  4,   3,    UFLOAT_11_11_10,        B10G11R11_UFLOAT_PACK32,  RG11B10Float,          FloatRG11B10,          R11F_G11F_B10F,                      r11_g11_b10_ufloat)
#define UFLOAT_9_9_9_EXP_5_(impl)       impl(/*TODO*/,  4,   3,    UFLOAT_9_9_9_EXP_5,     E5B9G9R9_UFLOAT_PACK32,   RGB9E5Float,           FloatRGB9E5,           RGB9_E5,                             /* n/a */)
/*                                           type    size comps blender_enum            vk_enum                   mtl_pixel_enum         mtl_vertex_enum        gl_pixel_enum                        shader_enum  */
#define SRGBA_8_8_8_8_(impl)            impl(/*TODO*/,  4,   4,    SRGBA_8_8_8_8,          R8G8B8A8_SRGB,            RGBA8Unorm_sRGB,       /* n/a */,             SRGB8_ALPHA8,                        /* n/a */   )
#define SRGBA_8_8_8_(impl)              impl(/*TODO*/,  3,   3,    SRGBA_8_8_8,            R8G8B8_SRGB,              /* n/a */,             /* n/a */,             SRGB8,                               /* n/a */   )
/*                                           type    size comps blender_enum            vk_enum                   mtl_pixel_enum         mtl_vertex_enum        gl_pixel_enum                        shader_enum  */
#define UNORM_16_DEPTH_(impl)           impl(/*TODO*/,  4,   1,    UNORM_16_DEPTH,         D16_UNORM,                Depth16Unorm,          /* n/a */,             DEPTH_COMPONENT16,                   /* n/a */   )
#define UNORM_24_DEPTH_(impl)           impl(/*TODO*/,  4,   1,    UNORM_24_DEPTH,         X8_D24_UNORM_PACK32,      Depth24Unorm_Stencil8, /* n/a */,             DEPTH_COMPONENT24,                   /* n/a */   )
#define UNORM_24_DEPTH_UINT_8_(impl)    impl(/*TODO*/,  8,   1,    UNORM_24_DEPTH_UINT_8,  D24_UNORM_S8_UINT,        Depth24Unorm_Stencil8, /* n/a */,             DEPTH24_STENCIL8,                    /* n/a */   )
#define SFLOAT_32_DEPTH_(impl)          impl(/*TODO*/,  4,   1,    SFLOAT_32_DEPTH,        D32_SFLOAT,               Depth32Float,          /* n/a */,             DEPTH_COMPONENT32F,                  /* n/a */   )
#define SFLOAT_32_DEPTH_UINT_8_(impl)   impl(/*TODO*/,  8,   1,    SFLOAT_32_DEPTH_UINT_8, D32_SFLOAT_S8_UINT,       Depth32Float_Stencil8, /* n/a */,             DEPTH32F_STENCIL8,                   /* n/a */   )
/*                                           type    size comps blender_enum            vk_enum                   mtl_pixel_enum         mtl_vertex_enum        gl_pixel_enum                        shader_enum  */
#define SNORM_DXT1_(impl)               impl(/* n/a */, 1,   1,    SNORM_DXT1,             BC1_RGBA_UNORM_BLOCK,     BC1_RGBA,              /* n/a */,             COMPRESSED_RGBA_S3TC_DXT1_EXT,       /* n/a */   )
#define SNORM_DXT3_(impl)               impl(/* n/a */, 1,   1,    SNORM_DXT3,             BC2_UNORM_BLOCK,          BC2_RGBA,              /* n/a */,             COMPRESSED_RGBA_S3TC_DXT3_EXT,       /* n/a */   )
#define SNORM_DXT5_(impl)               impl(/* n/a */, 1,   1,    SNORM_DXT5,             BC3_UNORM_BLOCK,          BC3_RGBA,              /* n/a */,             COMPRESSED_RGBA_S3TC_DXT5_EXT,       /* n/a */   )
#define SRGB_DXT1_(impl)                impl(/* n/a */, 1,   1,    SRGB_DXT1,              BC1_RGBA_SRGB_BLOCK,      BC1_RGBA_sRGB,         /* n/a */,             COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, /* n/a */   )
#define SRGB_DXT3_(impl)                impl(/* n/a */, 1,   1,    SRGB_DXT3,              BC2_SRGB_BLOCK,           BC2_RGBA_sRGB,         /* n/a */,             COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT, /* n/a */   )
#define SRGB_DXT5_(impl)                impl(/* n/a */, 1,   1,    SRGB_DXT5,              BC3_SRGB_BLOCK,           BC3_RGBA_sRGB,         /* n/a */,             COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, /* n/a */   )
/* clang-format on */

/** \} */

/* -------------------------------------------------------------------- */
/** \name Data Formats
 * \{ */

/**
 * Collection of all data formats.
 * Vertex formats and Texture formats are both subset of this.
 */
enum class DataFormat : uint8_t {
  Invalid = 0,

#define DECLARE(a, b, c, blender_enum, d, e, f, g, h) blender_enum,

#define GPU_DATA_FORMAT_EXPAND(impl) \
  VEC_X_(impl) \
  VEC_X_X_(impl) \
  VEC_X_X_X_(impl) \
  VEC_X_X_X_X_(impl) \
\
  SNORM_10_10_10_2_(impl) \
  UNORM_10_10_10_2_(impl) \
  UINT_10_10_10_2_(impl) \
\
  UFLOAT_11_11_10_(impl) \
  UFLOAT_9_9_9_EXP_5_(impl) \
\
  UNORM_16_DEPTH_(impl) \
  UNORM_24_DEPTH_(impl) /* TODO(fclem): Incompatible with metal, is emulated. To remove. */ \
  UNORM_24_DEPTH_UINT_8_(impl) \
  SFLOAT_32_DEPTH_(impl) \
  SFLOAT_32_DEPTH_UINT_8_(impl) \
\
  SRGBA_8_8_8_(impl) \
  SRGBA_8_8_8_8_(impl) \
\
  SNORM_DXT1_(impl) \
  SNORM_DXT3_(impl) \
  SNORM_DXT5_(impl) \
  SRGB_DXT1_(impl) \
  SRGB_DXT3_(impl) \
  SRGB_DXT5_(impl)

  GPU_DATA_FORMAT_EXPAND(DECLARE)

#undef DECLARE
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Utilities
 *
 * Allow querying informations about the format enum values.
 * \{ */

/* NOTE: Compressed format bytesize are rounded up as their actual value is fractional. */
inline int to_bytesize(const DataFormat format)
{
  switch (format) {
#define CASE(a, size, c, blender_enum, d, e, f, g, h) \
  case DataFormat::blender_enum: \
    return size;

    GPU_DATA_FORMAT_EXPAND(CASE)
#undef CASE

    case DataFormat::Invalid:
      break;
  }
  BLI_assert_unreachable();
  return -1;
}

inline int format_component_len(const DataFormat format)
{
  switch (format) {
#define CASE(a, b, comp, blender_enum, d, e, f, g, h) \
  case DataFormat::blender_enum: \
    return comp;

    GPU_DATA_FORMAT_EXPAND(CASE)
#undef CASE

    case DataFormat::Invalid:
      break;
  }
  BLI_assert_unreachable();
  return -1;
}

/** \} */

}  // namespace blender::gpu
