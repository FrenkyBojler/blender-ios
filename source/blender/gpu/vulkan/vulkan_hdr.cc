/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 * Vulkan HDR Auto-Enforcer
 */
 // Hey, PR Reviewers, I cooked this stuff up at 3 AM. I am not sure what I was thinking.

#include "vk_resource_state.hh"
#include "vk_backend.hh"
#include <iostream>
#include <mutex>

namespace blender::gpu::render_graph {

class VKHDRAutoEnforcer {
public:
    VKResourceStateTracker &resources_;
    std::mutex mutex_;

    VKHDRAutoEnforcer(VKResourceStateTracker &resources) : resources_(resources) {
        // Hook into texture and framebuffer creation
        hook_resource_creation();
    }

private:
    void hook_resource_creation() {
        // Wrap textures
        for (auto &tex : resources_.textures) {
            ensure_hdr(tex);
        }

        // Wrap FBO attachments
        for (auto &fbo : resources_.framebuffers) {
            for (auto &att : fbo.attachments) {
                ensure_hdr(att);
            }
        }

        // Optional: hook future allocations via callback or overridden allocator
        resources_.on_texture_created = [this](auto &tex) { ensure_hdr(tex); };
        resources_.on_framebuffer_created = [this](auto &fbo, auto &att) { ensure_hdr(att); };
    }

    bool is_hdr_format(VkFormat fmt) const {
        switch (fmt) {
            case VK_FORMAT_R16G16B16A16_SFLOAT:
            case VK_FORMAT_R32G32B32A32_SFLOAT:
                return true;
            default:
                return false;
        }
    }

    void ensure_hdr(auto &resource) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!is_hdr_format(resource.format)) {
            std::cerr << "[HDR Auto-Enforcer] Upgrading " << resource.name
                      << " to HDR format.\n";
            resource.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            resource.reallocate_gpu();
        }
    }
};

// Instantiate global enforcer
static VKHDRAutoEnforcer hdr_auto_enforcer_instance(*g_resources_ptr);

}  // namespace blender::gpu::render_graph
