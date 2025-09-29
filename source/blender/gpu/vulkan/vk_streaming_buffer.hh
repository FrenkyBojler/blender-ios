

#pragma once

#include "vk_common.hh"
#include "vk_staging_buffer.hh"
#include "render_graph/vk_render_graph.hh"

namespace blender::gpu {
    class VKBuffer;
    class VKContext;
 
    class VKStreamingBuffer {
        std::optional<std::unique_ptr<VKBuffer>> host_buffer_;
        VkBuffer vk_buffer_dst_;
        VkDeviceSize vk_buffer_size_;
        VkDeviceSize offset_ = 0;
        render_graph::NodeHandle copy_buffer_handle_ = 0;

        public:
        VKStreamingBuffer(VKBuffer&buffer);
        ~VKStreamingBuffer();
        VkDeviceSize update(VKContext&context, const void*data, size_t data_size);

        VkBuffer vk_buffer_dst() {
            return vk_buffer_dst_;
        }
    };
}