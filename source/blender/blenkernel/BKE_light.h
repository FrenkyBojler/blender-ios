/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 * \brief General operations, lookup, etc. for blender lights.
 */

#include "BLI_compiler_attrs.h"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

struct Depsgraph;
struct Light;
struct Main;

Light *BKE_light_add(Main *bmain, const char *name) ATTR_WARN_UNUSED_RESULT;

void BKE_light_eval(Depsgraph *depsgraph, Light *la);

/* Maximum theoretical luminous efficacy (555nm monochromatic light) */
#define BKE_LIGHT_LUMINOUS_EFFICACY_MAX 683.0f

float BKE_light_power(const Light &light);
float BKE_light_radiometric_to_photometric_power(const Light &light, float power);
blender::float3 BKE_light_color(const Light &light);
float BKE_light_area(const Light &light, const blender::float4x4 &object_to_world);
