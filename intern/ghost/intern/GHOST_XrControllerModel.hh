/* SPDX-FileCopyrightText: 2021-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

/* NOTE: Requires OpenXR headers to be included before this one for OpenXR types (XrInstance,
 * XrSession, etc.). */

#pragma once

#include <atomic>
#include <future>
#include <vector>

struct GHOST_XrControllerModelNode;

/**
 * Multi-vendor OpenXR interaction model extension implementation, returning a glTF render model.
 * XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION to retrieve the controller model objects.
 * XR_EXT_RENDER_MODEL_EXTENSION to define and load data from the retrieved model objects.
 */
class GHOST_XrControllerModel {
 public:
  GHOST_XrControllerModel(XrInstance instance, XrSpace base_space, const char *subaction_path_str);
  ~GHOST_XrControllerModel();

  void load(XrSession session);
  void updateComponents(XrSession session, XrTime display_time);
  void getData(GHOST_XrControllerModelData &r_data);

 private:
  XrPath subaction_path_ = XR_NULL_PATH;
  XrSpace reference_space_;

  std::vector<XrPath> toplevel_paths_;

  std::vector<XrRenderModelEXT> interaction_models_;
  std::vector<XrSpace> model_spaces_;
  std::vector<XrRenderModelPropertiesEXT> model_properties_;

  /* Merged geometry data from all interaction models. */
  std::vector<GHOST_XrControllerModelVertex> vertices_;
  std::vector<uint32_t> indices_;
  std::vector<GHOST_XrControllerModelComponent> components_;
  std::vector<GHOST_XrControllerModelNode> nodes_;

  /* Texture storage (raw encoded image data). */
  using XrTextureData = std::vector<uchar>;
  std::vector<XrTextureData> textures_;

  /* Per-model tracking for updates. */
  struct PerModelData {
    std::vector<XrRenderModelAssetNodePropertiesEXT> node_properties;
    std::vector<int32_t> node_state_indices; /* Maps XR node state index -> nodes_ index. */
    int32_t node_offset;                     /* Offset into merged nodes_. */
    int32_t component_offset;                /* Offset into merged components_. */
  };
  std::vector<PerModelData> per_model_data_;

  /* Asynchronous data loading. */
  std::future<void> load_task_;
  std::atomic<bool> data_loaded_ = false;

  /* Base pose for positioning the model (from render model space location). */
  GHOST_XrPose base_pose_ = {false, {0, 0, 0}, {1, 0, 0, 0}};
};
