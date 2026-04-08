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

namespace blender::gpu {

/**
 * Utility class to copy data from host to device and vise versa.
 *
 * This is a common as buffers on device are more performant than when located inside host memory.
 */
class VKStagingBuffer {
 private:
  /**
   * Reference to the device buffer.
   */
  const VKBuffer &device_buffer_;

  /**
   * The temporary buffer on host for the transfer. Also called the staging buffer.
   */
  inline static VKBuffer host_buffer_;

  inline static std::mutex allocator_mutex_;
  const VkDeviceSize HOST_BUFFER_SIZE = 128 * 1024 * 1024;

  inline static VkDeviceSize write_offset_ = 0;

  VkDeviceSize device_buffer_offset_;
  VkDeviceSize region_size_;
  VkDeviceSize local_offset_ = 0;

  VkDeviceSize oldest_actual_offset_ = 0;
  VkDeviceSize prev_timeline_ = 0;

  /* Saved timeline value to read data asynchronously */
  TimelineValue saved_timeline_value_ = 0;

 private:
  VkDeviceSize allocate_region(VkDeviceSize size, VkDeviceSize alignment);
 public:
  VKStagingBuffer(const VKBuffer &device_buffer,
                  VkDeviceSize device_buffer_offset = 0,
                  VkDeviceSize region_size = UINT64_MAX);

  /**
   * Copy the content of the host buffer to the device buffer.
   *
   * Must be called one time per class object.
   */
  void copy_to_device(VKContext &context, const void *data);

  /**
   * Copy the content of the device buffer to the host buffer.
   */
  void copy_from_device(VKContext &context);

  /**
   * Read data when the copy is over.
   */
  void read(void *data);

  bool is_allocated() const
  {
    return host_buffer_.is_allocated();
  }

  bool is_mapped() const
  {
    return host_buffer_.is_mapped();
  }

  /**
   * Free the host buffer.
   *
   * It must be called once when the application is closing.
   */
  static void free(VKDevice &device);
};
}  // namespace blender::gpu
