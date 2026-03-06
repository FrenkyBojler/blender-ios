/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include <deque>

#include "vk_buffer.hh"
#include "vk_common.hh"
#include "vk_backend.hh"

namespace blender::gpu {

/**
 * Utility class to copy data from host to device and vise versa.
 *
 * This is a common as buffers on device are more performant than when located inside host memory.
 */
class VKStagingBuffer {
 private:
  /**
   * The command buffer descriptor block.
   */
  struct CommandBufferDescriptor {
    /* The command buffer to send the copy operation to the transfer queue */
    VkCommandBuffer command_buffer;
    /* The semaphore that is being waited by the flush_render_graph and signaled by the transfer
     * queue */
    VkSemaphore binary_semaphore;
    /* The timeline value instead of multiple fences */
    uint64_t timeline_value = 0;
  };

  /**
   * The region of data that indicates the busy region.
   */
  struct HostBufferRegion {
    /* Where it starts from */
    VkDeviceSize start;
    /* Its size */
    VkDeviceSize size;
    /* The timeline semaphore's value that has to be set in order to use this region again */
    uint64_t timeline_value;
  };

 public:
  /**
   * Direction of the transfer.
   */
  enum class Direction {
    /**
     * Transferring data from host to device.
     */
    HostToDevice,
    /**
     * Transferring data from device to host.
     */
    DeviceToHost,
  };
 private:
	/**
	 * Reference to the device buffer.
	 */
	const VKBuffer &device_buffer_;

	/**
	 * The temporary buffer on host for the transfer. Also called the staging buffer.
	 */
	inline static VKBuffer host_buffer_;

  /**
   * The temporary buffer's size
   */
  static constexpr size_t HOST_BUFFER_SIZE = 128 * 1024 * 1024;

	VkDeviceSize device_buffer_offset_;
	VkDeviceSize region_size_;

  inline static VkQueue transfer_queue_ = VK_NULL_HANDLE;

  inline static std::deque<HostBufferRegion> host_buffer_regions_;

	inline static VkCommandPool command_pool_ = VK_NULL_HANDLE;

  static Vector<CommandBufferDescriptor> &staging_command_buffers_unused()
  {
    static Vector<CommandBufferDescriptor> vec;
    return vec;
  }

  static Vector<CommandBufferDescriptor> &staging_command_buffers_in_use()
  {
    static Vector<CommandBufferDescriptor> vec;
    return vec;
  }

  inline static bool is_first_run_ = true;
  inline static bool is_generic_queue_needed_ = false;

  inline static VkSemaphore render_graph_finished_semaphore_ = VK_NULL_HANDLE;
  inline static VkSemaphore timeline_semaphore_ = VK_NULL_HANDLE;
  inline static uint64_t timeline_value_ = 0;

  inline static VkDeviceSize current_start_region_ = 0;

  Direction direction_;

  /**
   * Destroys all resources that have been created
   */
  static void cleanup(VkDevice device);

  static CommandBufferDescriptor staging_command_buffer_descriptor_get();
  VkDeviceSize update_host_buffer_regions();

  /* Used when the read function needs to access the current start region. */
  VkDeviceSize local_start_region_ = 0;

 public:
  VKStagingBuffer(const VKBuffer &device_buffer,
                  Direction direction,
                  VkDeviceSize device_buffer_offset = 0,
                  VkDeviceSize region_size = UINT64_MAX);
  /**
   * Initialize VKStagingBuffer's global resources.
   * Must be called once during Vulkan device initialization.
   */
  static void init(VKDevice &device);
  /**
   * Deinitialize VKStagingBuffer's global resources.
   * Must be called once during Vulkan device deinitialization.
   */
  static void deinit(VKDevice& device);


  /**
   * Write data to the host buffer and copy the content of
   * the host buffer to the device buffer.
   *
   * This operation is asynchronous. It does not lock the current thread.
   */
  void copy_to_device(const void *data);

  /**
   * Copy the content of the device buffer to the host buffer
   * and write it to the destination buffer.
   *
   * This operation is asynchronous. It does not lock the current thread.
   * NOTE: `read` method must be called after this operation.
   */
  void copy_from_device();

  /** Wait and read data from the host buffer */
  void read(void *data) const;

  inline bool is_allocated() const {
    return host_buffer_.is_allocated();
  }

  inline bool is_mapped() const {
    return host_buffer_.is_mapped();
  }

	VkDeviceSize size_in_bytes_get() const
	{
		return region_size_;
	}
};
}  // namespace blender::gpu
