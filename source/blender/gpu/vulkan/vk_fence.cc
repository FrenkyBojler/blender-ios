/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_fence.hh"
#include "vk_backend.hh"
#include "vk_common.hh"
#include "vk_context.hh"

namespace blender::gpu {

VKFence::~VKFence()
{
  if (vk_fence_ == VK_NULL_HANDLE) {
    const VKDevice &device = VKBackend::get().device;
    vkDestroyFence(device.vk_handle(), vk_fence_, nullptr);
    vk_fence_ = VK_NULL_HANDLE;
  }
}

void VKFence::signal()
{
  const VKDevice &device = VKBackend::get().device;
  if (vk_fence_ == VK_NULL_HANDLE) {
    VkFenceCreateInfo vk_fence_create_info = {
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    vkCreateFence(device.vk_handle(), &vk_fence_create_info, nullptr, &vk_fence_);
  }
  vkWaitForFences(device.vk_handle(), 1, &vk_fence_, true, UINT64_MAX);
  vkResetFences(device.vk_handle(), 1, &vk_fence_);

  VKContext &context = *VKContext::get();
  context.flush_render_graph(RenderGraphFlushFlags::SUBMIT |
                                 RenderGraphFlushFlags::RENEW_RENDER_GRAPH,
                             VK_PIPELINE_STAGE_NONE,
                             VK_NULL_HANDLE,
                             VK_NULL_HANDLE,
                             vk_fence_);
}

void VKFence::wait()
{
  if (vk_fence_ == VK_NULL_HANDLE) {
    return;
  }
  const VKDevice &device = VKBackend::get().device;
  vkWaitForFences(device.vk_handle(), 1, &vk_fence_, true, UINT64_MAX);
}

}  // namespace blender::gpu
