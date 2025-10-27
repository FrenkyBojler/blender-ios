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

#include "CLG_log.h"


static CLG_LogRef LOG = {"gpu.vulkan"};

namespace blender::gpu {

VKFence::~VKFence()
{
  if (vk_event_ != VK_NULL_HANDLE) {
    CLOG_TRACE(&LOG, "%s: discarding event %llu", name.c_str(), vk_event_);
    VKDiscardPool::discard_pool_get().discard_event(vk_event_);
    vk_event_ = VK_NULL_HANDLE;
  }
}

void VKFence::signal()
{
  std::scoped_lock lock(mutex_);
  VkEvent previous_event = vk_event_;
  VKContext &context = *VKContext::get();
  VKDevice &device = VKBackend::get().device;
  VkEventCreateInfo vk_event_create_info = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, nullptr, 0};
  vkCreateEvent(device.vk_handle(), &vk_event_create_info, nullptr, &vk_event_);
  CLOG_TRACE(&LOG, "%s: created event %llu", name.c_str(), vk_event_);
  debug::object_label(vk_event_, name);
  BLI_assert(vk_event_ != VK_NULL_HANDLE);

  render_graph::VKRenderGraph &render_graph = context.render_graph();
  render_graph::VKSetEventNode::Data set_event = {vk_event_};
  render_graph.add_node(set_event);

  if (previous_event != VK_NULL_HANDLE) {
    context.discard_pool.discard_event(previous_event);
  }

  context.flush_render_graph(RenderGraphFlushFlags::SUBMIT |
                             RenderGraphFlushFlags::RENEW_RENDER_GRAPH);
}

void VKFence::wait()
{
  std::scoped_lock lock(mutex_);
  if (vk_event_ == VK_NULL_HANDLE) {
    return;
  }

  VKContext &context = *VKContext::get();
  render_graph::VKRenderGraph &render_graph = context.render_graph();
  render_graph::VKWaitEventNode::Data wait_event = {vk_event_};
  render_graph.add_node(wait_event);
  CLOG_TRACE(&LOG, "%s: add wait event %llu", name.c_str(), vk_event_);
}

}  // namespace blender::gpu
