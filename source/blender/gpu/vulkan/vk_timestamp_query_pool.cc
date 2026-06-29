/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "CLG_log.h"

#include "BLI_assert.hh"

#include "vk_backend.hh"
#include "vk_context.hh"
#include "vk_timestamp_query_pool.hh"

namespace blender::gpu {

static CLG_LogRef LOG = {"gpu.vulkan"};

VKTimestampQueryPool::VKTimestampQueryPool(unsigned int num_queries_max)
    : num_queries_max_(num_queries_max)
{
  const VKDevice &device = VKBackend::get().device;
  /* Needs device.physical_device_properties_get().limits.timestampComputeAndGraphics */
  VkQueryPoolCreateInfo queryPoolCreateInfo = {};
  queryPoolCreateInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  queryPoolCreateInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
  queryPoolCreateInfo.queryCount = num_queries_max_;
  VkResult result = vkCreateQueryPool(
      device.vk_handle(), &queryPoolCreateInfo, nullptr, &query_pool_);
  BLI_assert(result == VK_SUCCESS);

#ifdef _WIN32
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  timer_freq_ = frequency.QuadPart;
#endif

  const VKExtensions &extensions = device.extensions_get();
  if (extensions.calibrated_timestamps) {
    uint32_t num_time_domains = 0;
    std::vector<VkTimeDomainKHR> time_domains{};
    auto res = device.functions.vkGetPhysicalDeviceCalibrateableTimeDomains(
        device.physical_device_get(), &num_time_domains, nullptr);
    if (res != VK_SUCCESS) {
      CLOG_ERROR(&LOG, "Calibrateable time domains query failed.");
    }
    time_domains.resize(num_time_domains);
    res = device.functions.vkGetPhysicalDeviceCalibrateableTimeDomains(
        device.physical_device_get(), &num_time_domains, time_domains.data());
    if (res != VK_SUCCESS) {
      CLOG_ERROR(&LOG, "Calibrateable time domains query failed.");
    }
    bool has_device_time_domain = false;
    bool has_host_time_domain = false;
    for (VkTimeDomainKHR time_domain : time_domains) {
      if (time_domain == VK_TIME_DOMAIN_DEVICE_KHR) {
        has_device_time_domain = true;
      }
#ifdef _WIN32
      if (time_domain == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR) {
        has_host_time_domain = true;
      }
#else
      if (time_domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR) {
        has_host_time_domain = true;
      }
#endif
    }
    if (!has_device_time_domain) {
      CLOG_ERROR(&LOG, "Necessary device timestamp domain not supported.");
    }
    if (!has_host_time_domain) {
      CLOG_ERROR(&LOG, "Necessary host timestamp domain not supported.");
    }
  }
}

void VKTimestampQueryPool::reset()
{
  /* Needs hostQueryReset */
  /*VKContext &context = *VKContext::get();
  const VKDevice &device = VKBackend::get().device;
  vkResetQueryPool(device.vk_handle(), query_pool_, 0, num_queries_max_);*/

  VKContext &context = *VKContext::get();
  render_graph::VKResetQueryPoolNode::Data reset_query_pool = {};
  reset_query_pool.vk_query_pool = query_pool_;
  reset_query_pool.first_query = 0;
  reset_query_pool.query_count = num_queries_max_;
  context.render_graph().add_node(reset_query_pool);
}

void VKTimestampQueryPool::write_timestamp(unsigned int index)
{
  VKContext &context = *VKContext::get();
  render_graph::VKWriteTimestampNode::CreateInfo write_timestamp_node = {};
  write_timestamp_node.vk_query_pool = query_pool_;
  write_timestamp_node.query_index = index;
  context.render_graph().add_node(write_timestamp_node);
}

void VKTimestampQueryPool::read_timestamps_sync(unsigned int index_first,
                                                unsigned int num_queries,
                                                uint64_t *timestamps_device)
{
  const VKDevice &device = VKBackend::get().device;
  VkResult result = vkGetQueryPoolResults(device.vk_handle(),
                                          query_pool_,
                                          0,
                                          num_queries,
                                          num_queries * sizeof(uint64_t),
                                          timestamps_device,
                                          sizeof(uint64_t),
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
  BLI_assert(result == VK_SUCCESS);

  /*vkResetQueryPool(device.vk_handle(), query_pool_, index_first, num_queries);*/

  VKContext &context = *VKContext::get();
  render_graph::VKResetQueryPoolNode::Data reset_query_pool = {};
  reset_query_pool.vk_query_pool = query_pool_;
  reset_query_pool.first_query = index_first;
  reset_query_pool.query_count = num_queries;
  context.render_graph().add_node(reset_query_pool);
}

uint64_t VKTimestampQueryPool::convert_timestamp_device_to_host_domain(uint64_t timestamp_device)
{
  const VKDevice &device = VKBackend::get().device;
  const VKExtensions &extensions = device.extensions_get();
  if (!extensions.calibrated_timestamps) {
    return timestamp_device;
  }

  VkCalibratedTimestampInfoKHR calibrated_timestamp_infos[2];
  VkCalibratedTimestampInfoKHR &calibratedTimestampInfoDevice = calibrated_timestamp_infos[0];
  calibratedTimestampInfoDevice.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
  calibratedTimestampInfoDevice.pNext = nullptr;
  calibratedTimestampInfoDevice.timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
  VkCalibratedTimestampInfoKHR &calibratedTimestampInfoHost = calibrated_timestamp_infos[1];
  calibratedTimestampInfoHost.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
  calibratedTimestampInfoHost.pNext = nullptr;
#ifdef _WIN32
  calibratedTimestampInfoHost.timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
#else
  calibratedTimestampInfoHost.timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
#endif

  uint64_t calibrated_timestamps[2];
  uint64_t max_deviation = 0;
  VkResult result = device.functions.vkGetCalibratedTimestamps(
      device.vk_handle(), 2, calibrated_timestamp_infos, calibrated_timestamps, &max_deviation);
  BLI_assert(result == VK_SUCCESS);

  double timestamp_period_device = device.physical_device_properties_get().limits.timestampPeriod;
  uint64_t calib_timestamp_device = calibrated_timestamps[0];
  uint64_t calib_timestamp_host = calibrated_timestamps[1];
  /* time_offset_device is in nanoseconds. */
  auto time_offset_device = int64_t(
      double(int64_t(timestamp_device) - int64_t(calib_timestamp_device)) *
      timestamp_period_device);
  int64_t timestamp_converted;
#ifdef _WIN32
  /* We lose precision by the conversion of absolute timestamps to double, so avoid this if
   * possible. */
  timestamp_converted = convert_performance_counter_to_nanoseconds(calib_timestamp_host) +
                        time_offset_device;
#else
  timestamp_converted = int64_t(calib_timestamp_host) + time_offset_device;
#endif
  if (timestamp_converted < 0) {
    timestamp_converted = 0;
  }
  return uint64_t(timestamp_converted);
}

uint64_t VKTimestampQueryPool::get_host_timestamp_now()
{
#ifdef _WIN32
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  auto timestamp_host = convert_performance_counter_to_nanoseconds(counter.QuadPart);
  return uint64_t(timestamp_host);
#else
  struct timespec tv{};
  clock_gettime(CLOCK_MONOTONIC, &tv);
  return uint64_t(tv.tv_nsec + tv.tv_sec * 1000000000ll);
#endif
}

#ifdef _WIN32
int64_t VKTimestampQueryPool::convert_performance_counter_to_nanoseconds(int64_t pc_time)
{
  /* We lose precision by the conversion of absolute timestamps to double, so avoid this if
   * possible. */
  int64_t time_nanoseconds;
  if (1000000000LL % timer_freq_ == 0) {
    time_nanoseconds = int64_t(pc_time) * (1000000000LL / timer_freq_);
  }
  else {
    time_nanoseconds = int64_t(double(pc_time) * (1000000000.0 / double(timer_freq_)));
  }
  return time_nanoseconds;
}
#endif

}  // namespace blender::gpu
