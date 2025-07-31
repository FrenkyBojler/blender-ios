/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/common.h>

struct bNode;
struct bNodeTree;

struct Main;
struct Scene;

namespace blender::io::usd {

struct USDExportParams;
struct USDImportParams;

/* This struct contains all DomeLight attribute needed to
 * create a world environment */
struct USDImportDomeLightData {
  float intensity;
  pxr::GfVec3f color;
  pxr::SdfAssetPath tex_path;
  pxr::TfToken pole_axis;

  bool has_color;
  bool has_tex;
};

/**
 * If the Blender scene has an environment texture,
 * export it as a USD dome light.
 */
void world_material_to_dome_light(const USDExportParams &params,
                                  const Scene *scene,
                                  pxr::UsdStageRefPtr stage);

void dome_light_to_world_material(const USDImportParams &params,
                                  Scene *scene,
                                  Main *bmain,
                                  const USDImportDomeLightData &dome_light_data,
                                  const pxr::UsdPrim &prim,
                                  const pxr::UsdTimeCode time = 0.0);

bNode *find_world_output(const bNodeTree *nodetree);

/**
 * Helper struct for retrieving shader information when traversing a world material
 * node chain, provided as user data for #bke::node_chain_iterator().
 */
struct WorldNtreeSearchResults {
  /* Data passed to `file_path_getter_fn` */
  void *payload = nullptr;
  std::string (*file_path_getter_fn)(bNode *fromnode, void *payload) = nullptr;

  std::string file_path;

  float world_intensity = 0.0f;
  float world_color[3]{};
  float mapping_rot[3]{};
  float color_mult[3]{};

  bool background_found = false;
  bool env_tex_found = false;
  bool mult_found = false;
};

bool node_search(bNode *fromnode, bNode *tonode, void *userdata, bool reversed);

pxr::GfMatrix4d make_dome_light_transform(pxr::GfVec3f rot);

}  // namespace blender::io::usd
