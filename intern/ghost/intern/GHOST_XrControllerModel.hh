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
 * OpenXR glTF controller model base class for different OpenXR extensions.
 */
class GHOST_XrControllerModel {
 public:
  virtual ~GHOST_XrControllerModel() = default;

  virtual void load(XrSession session) = 0;
  virtual void updateComponents(XrSession session, XrTime display_time) = 0;
  virtual void getData(GHOST_XrControllerModelData &r_data) = 0;

 protected:
  std::future<void> load_task_;
  std::atomic<bool> data_loaded_ = false;
};

/**
 * Multi-vendor OpenXR controller model extension implementation.
 * XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION to retrieve the controller model objects.
 * XR_EXT_RENDER_MODEL_EXTENSION to define and load data from the retrieved model objects.
 */
class GHOST_XrControllerModelEXT : public GHOST_XrControllerModel {
 public:
  GHOST_XrControllerModelEXT(XrInstance instance, XrSpace base_space);
  ~GHOST_XrControllerModelEXT() override;

  void load(XrSession session) override;
  void updateComponents(XrSession session, XrTime display_time) override;
  void getData(GHOST_XrControllerModelData &r_data) override;

 private:
  XrSpace reference_space_;

  std::vector<XrRenderModelEXT> interaction_models_;
  std::vector<XrSpace> model_spaces_;
  std::vector<XrRenderModelPropertiesEXT> model_properties_;

  void loadControllerModel(XrSession session);
};

/**
 * Microsoft OpenXR controller model extension implementation
 * XR_MSFT_CONTROLLER_MODEL_EXTENSION_NAME
 */
class GHOST_XrControllerModelMSFT : public GHOST_XrControllerModel {
 public:
  GHOST_XrControllerModelMSFT(XrInstance instance, const char *subaction_path);
  ~GHOST_XrControllerModelMSFT() override;

  void load(XrSession session) override;
  void updateComponents(XrSession session, XrTime display_time) override;
  void getData(GHOST_XrControllerModelData &r_data) override;

 private:
  XrPath subaction_path_ = XR_NULL_PATH;
  XrControllerModelKeyMSFT model_key_ = XR_NULL_CONTROLLER_MODEL_KEY_MSFT;

  std::vector<GHOST_XrControllerModelVertex> vertices_;
  std::vector<uint32_t> indices_;
  std::vector<GHOST_XrControllerModelComponent> components_;
  std::vector<GHOST_XrControllerModelNode> nodes_;
  /** Maps node states to nodes. */
  std::vector<int32_t> node_state_indices_;

  void loadControllerModel(XrSession session);
};
