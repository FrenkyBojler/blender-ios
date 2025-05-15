/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "camera_delegate.hh"

#include "DNA_scene_types.h"
#include "DNA_camera_types.h"

#include "BKE_idprop.hh"

#include <pxr/imaging/hd/camera.h>

namespace blender::render::hydra {

static pxr::VtValue vt_value(const IDProperty *prop)
{
  switch (prop->type) {
    case IDP_INT:
      return pxr::VtValue{IDP_Int(prop)};
    case IDP_FLOAT:
      return pxr::VtValue{IDP_Float(prop)};
    case IDP_BOOLEAN:
      return pxr::VtValue{bool(IDP_Bool(prop))};
  }
  return pxr::VtValue{};
}

CameraDelegate::CameraDelegate(pxr::HdRenderIndex *render_index, pxr::SdfPath const &delegate_id)
    : pxr::HdxFreeCameraSceneDelegate{render_index, delegate_id}
{
}

void CameraDelegate::set_camera_params(const Scene *scene)
{
  if (!scene || !scene->camera) {
    return;
  }

  auto *camera = static_cast<const Camera *>(scene->camera->data);
  if (!camera) {
    return;
  }

  const IDProperty *props = camera->id.properties;
  if (!props) {
    return;
  }

  for (auto *prop = static_cast<const IDProperty *>(props->data.group.first); prop; prop = prop->next) {
    if (const auto value = vt_value(prop); !value.IsEmpty()) {
      set_camera_param_value(pxr::TfToken{prop->name}, value);
    }
  }
}

void CameraDelegate::set_camera_param_value(const pxr::TfToken &key, pxr::VtValue val)
{
  auto dirty_bits = pxr::HdCamera::Clean;

  auto [it, is_new_item] = custom_attributes_.try_emplace(key, val);
  if (is_new_item) {
    dirty_bits = pxr::HdCamera::DirtyParams;
  }
  else if (it->second != val) {
    it->second = val;
    dirty_bits = pxr::HdCamera::DirtyParams;
  }

  if (dirty_bits)
    GetRenderIndex().GetChangeTracker().MarkSprimDirty(GetCameraId(), dirty_bits);
}

pxr::VtValue CameraDelegate::GetCameraParamValue(pxr::SdfPath const &id, pxr::TfToken const &key)
{
  const auto it = custom_attributes_.find(key);
  if (it != custom_attributes_.cend()) {
    return it->second;
  }

  return pxr::HdxFreeCameraSceneDelegate::GetCameraParamValue(id, key);
}

}  // namespace blender::render::hydra
