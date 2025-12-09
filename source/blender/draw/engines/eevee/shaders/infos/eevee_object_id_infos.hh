/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_object_id_out)
DEFINE("MAT_OBJECT_ID")
/* Output 0 is for velocity. */
FRAGMENT_OUT(1, uint, out_object_id)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(eevee_object_id)
DEFINE("OBJECT_ID_TEX")
SAMPLER(OBJECT_ID_TEX_SLOT, usampler2D, object_id_tx)
GPU_SHADER_CREATE_END()
