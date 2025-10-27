/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_state_private.hh"

#include "vk_common.hh"

#include "BLI_mutex.hh"

namespace blender::gpu {

class VKFence : public Fence {
 private:
  VkEvent vk_event_ = VK_NULL_HANDLE;
  Mutex mutex_;

  ~VKFence();

 public:
  VKFence(const char *name) : Fence(name) {}
  void signal() override;
  void wait() override;
};

}  // namespace blender::gpu
