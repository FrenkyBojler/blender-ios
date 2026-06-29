/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_assert.h"
#include "BLI_enum_flags.hh"

#include "GPU_vertex_buffer.hh"

#include "gpu_framebuffer_private.hh"

namespace blender::gpu {

enum class IntelGpuArch : uint32_t {
  Gen9AndOlder,
  Gen11,
  Gen12,
  Xe = Gen12,
  Xe2,
  Xe3AndNewer,
};

IntelGpuArch get_intel_gpu_arch(uint32_t device_id);

}  // namespace blender::gpu
