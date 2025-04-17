/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <pxr/usd/usd/common.h>

struct Main;
struct Scene;

namespace blender::io::usd {

struct USDExportParams;
struct USDImportParams;

struct USDImportDomeLightAttr {
  float intensity;
  bool has_color;
  pxr::GfVec3f color;
  bool has_tex;
  pxr::SdfAssetPath tex_path;
};

void world_material_to_dome_light(const USDExportParams &params,
                                  const Scene *scene,
                                  pxr::UsdStageRefPtr stage);

void dome_light_to_world_material(const USDImportParams &params,
                                  Scene *scene,
                                  Main *bmain,
                                  const USDImportDomeLightAttr &dome_light_attr,
                                  const pxr::UsdPrim &prim,
                                  const double motionSampleTime = 0.0);

}  // namespace blender::io::usd
