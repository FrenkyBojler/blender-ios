/* SPDX-FileCopyrightText: 2018 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_fx
 */

#include "BLI_assert.h"

#include "BKE_shader_fx.hh"

#include "FX_shader_types.hh"

namespace blender {

void shaderfx_type_init(ShaderFxTypeInfo *types[])
{
#define INIT_FX_TYPE(typeName) \
  do { \
    types[eShaderFxType_##typeName] = &shaderfx_Type_##typeName; \
    BLI_assert(shaderfx_Type_##typeName.copy_data != nullptr); \
    BLI_assert(shaderfx_Type_##typeName.new_data != nullptr); \
    BLI_assert(shaderfx_Type_##typeName.free_data != nullptr); \
  } while (false)
  INIT_FX_TYPE(Blur);
  INIT_FX_TYPE(Colorize);
  INIT_FX_TYPE(Flip);
  INIT_FX_TYPE(Glow);
  INIT_FX_TYPE(Pixel);
  INIT_FX_TYPE(Rim);
  INIT_FX_TYPE(Shadow);
  INIT_FX_TYPE(Swirl);
  INIT_FX_TYPE(Wave);
#undef INIT_FX_TYPE
}

}  // namespace blender
