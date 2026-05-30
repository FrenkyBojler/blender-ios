/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/unique_ptr.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

class Device;
class DeviceInfo;
class Profiler;
class Stats;

bool device_optix_init(bool *r_meets_driver_requirement = nullptr);

unique_ptr<Device> device_optix_create(const DeviceInfo &info,
                                       Stats &stats,
                                       Profiler &profiler,
                                       bool headless);

void device_optix_info(const vector<DeviceInfo> &cuda_devices, vector<DeviceInfo> &devices);

/** Generate proper OptiX DeviceInfo based on an existing CUDA DeviceInfo for the same device.
 * Does not require usage of the OptiX API. */
bool optix_device_info_from_cuda(const DeviceInfo &cuda_device, DeviceInfo *r_optix_device);

CCL_NAMESPACE_END
