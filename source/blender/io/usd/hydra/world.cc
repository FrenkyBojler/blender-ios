/* SPDX-FileCopyrightText: 2011-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "world.hh"
#include "usd_private.hh"

#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usdLux/tokens.h>

#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_world_types.h"

#include "BLI_math_vector.h"
#include "BLI_math_rotation.h"

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_studiolight.h"

#include "NOD_shader.h"

#include "hydra_scene_delegate.hh"
#include "image.hh"
#include "usd_light_convert.hh"

/* TODO: add custom `tftoken` "transparency"? */

/* NOTE: opacity and blur aren't supported by USD */

namespace blender::io::hydra {

WorldData::WorldData(HydraSceneDelegate *scene_delegate, pxr::SdfPath const &prim_id)
    : LightData(scene_delegate, nullptr, prim_id)
{
  prim_type_ = pxr::HdPrimTypeTokens->domeLight;
}

void WorldData::init()
{
  data_.clear();

  float intensity = 1.0f;
  pxr::SdfAssetPath texture_file;

  if (scene_delegate_->shading_settings.use_scene_world) {
    const World *world = scene_delegate_->scene->world;
    pxr::GfVec3f color(1.0f, 1.0f, 1.0f);
    ID_LOG("%s", world->id.name);

    world->nodetree->ensure_topology_cache();

    /* TODO: Create nodes parsing system */

    const bNode *output = usd::find_world_output(world->nodetree);
    if (!output) {
      return;
    }

    usd::WorldNtreeSearchResults res;
    res.payload = scene_delegate_;
    res.file_path_getter_fn = [](bNode *fromnode, void *payload) -> std::string {
      if (!(fromnode && payload))
        return "";

      BLI_assert(fromnode->type_legacy == SH_NODE_TEX_ENVIRONMENT);

      NodeTexImage *tex = static_cast<NodeTexImage *>(fromnode->storage);
      Image *image = (Image *)fromnode->id;
      if (!image) {
        return "";
      }

      const HydraSceneDelegate *scene_delegate = reinterpret_cast<const HydraSceneDelegate *>(payload);
      return cache_or_get_image_file(scene_delegate->bmain, scene_delegate->scene, image, &tex->iuser);
    };
    bke::node_chain_iterator(world->nodetree, output, usd::node_search, &res, true);

    intensity = res.world_intensity;
    color = pxr::GfVec3f(res.world_color);
    if (!res.file_path.empty()) {
      texture_file = pxr::SdfAssetPath(res.file_path, res.file_path);
    }
    mapping_rot_ = pxr::GfVec3f{res.mapping_rot};

    if (texture_file.GetAssetPath().empty()) {
      float fill_color[4] = {color[0], color[1], color[2], 1.0f};
      std::string image_path = blender::io::usd::cache_image_color(fill_color);
      texture_file = pxr::SdfAssetPath(image_path, image_path);
    }
  }
  else {
    ID_LOG("studiolight: %s", scene_delegate_->shading_settings.studiolight_name.c_str());

    StudioLight *sl = BKE_studiolight_find(
        scene_delegate_->shading_settings.studiolight_name.c_str(),
        STUDIOLIGHT_ORIENTATIONS_MATERIAL_MODE);
    if (sl != nullptr && sl->flag & STUDIOLIGHT_TYPE_WORLD) {
      texture_file = pxr::SdfAssetPath(sl->filepath, sl->filepath);
      /* coefficient to follow Cycles result */
      intensity = scene_delegate_->shading_settings.studiolight_intensity / 2;
    }
  }

  data_[pxr::UsdLuxTokens->orientToStageUpAxis] = true;
  data_[pxr::HdLightTokens->intensity] = intensity;
  data_[pxr::HdLightTokens->color] = pxr::GfVec3f(1.0f, 1.0f, 1.0f);
  data_[pxr::HdLightTokens->textureFile] = texture_file;

  write_transform();
}

void WorldData::update()
{
  ID_LOG("");

  if (!scene_delegate_->shading_settings.use_scene_world ||
      (scene_delegate_->shading_settings.use_scene_world && scene_delegate_->scene->world))
  {
    init();
    if (data_.empty()) {
      remove();
      return;
    }
    insert();
    scene_delegate_->GetRenderIndex().GetChangeTracker().MarkSprimDirty(prim_id,
                                                                        pxr::HdLight::AllDirty);
  }
  else {
    remove();
  }
}

void WorldData::write_transform()
{
  transform = usd::make_dome_light_transform(mapping_rot_);
  if (!scene_delegate_->shading_settings.use_scene_world) {
    transform *= pxr::GfMatrix4d().SetRotate(
        pxr::GfRotation(pxr::GfVec3d(0.0, 0.0, -1.0),
                        RAD2DEGF(scene_delegate_->shading_settings.studiolight_rotation)));
  }
}

}  // namespace blender::io::hydra
