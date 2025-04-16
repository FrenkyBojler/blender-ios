/* SPDX-FileCopyrightText: 2021 NVIDIA Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_reader_domelight.hh"
#include "usd_light_convert.hh"

#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/domeLight_1.h>

namespace blender::io::usd {

void USDDomeLightReader::create_world_material(Scene *scene, Main *bmain)
{
  dome_light_to_world_material(import_params_, scene, bmain, light_);
}

}  // namespace blender::io::usd
