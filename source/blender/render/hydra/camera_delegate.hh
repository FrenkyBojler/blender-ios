/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <pxr/imaging/hdx/freeCameraSceneDelegate.h>

struct Scene;

namespace blender::render::hydra {

class CameraDelegate : public pxr::HdxFreeCameraSceneDelegate {
 public:
  CameraDelegate(pxr::HdRenderIndex *render_index, pxr::SdfPath const &delegate_id);
  ~CameraDelegate() override = default;

  void set_camera_params(const Scene *scene);

  pxr::VtValue GetCameraParamValue(pxr::SdfPath const &id, pxr::TfToken const &key) override;

 private:
  void set_camera_param_value(const pxr::TfToken &key, pxr::VtValue val);

 private:
  std::unordered_map<pxr::TfToken, pxr::VtValue, pxr::TfToken::HashFunctor> custom_attributes_;
};

}  // namespace blender::render::hydra
