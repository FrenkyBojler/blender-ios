/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "PRF_profile_gpu_common.hh"

namespace blender::gpu {

/* Scope guard type, begins/ends a profiling zone on the underlying GPU context. */
struct ProfileScope {
  ProfileScope(const PrfSourceLocation *location, bool is_transient);
  ~ProfileScope();
};

/**
 * Mark GPU API calls under the current scope for timing in a profiling tool.
 *
 * \param name: Name used to identify the scope. Must be a compile-time string or ustring.
 * \param category: Type of ProfileCategory, used for color labeling.
 */
#define GPU_profile_scope(name, category) \
  static PrfSourceLocationUnique(name, category); \
  const gpu::ProfileScope _PRF_DEBUG_CONCAT(gpu_profile_scope_, __LINE__)( \
      &_GPU_DEBUG_CONCAT(gpu_profile_loc_, __LINE__), false);

/**
 * Mark GPU API calls under the current scope for timing in a profiling tool.
 *
 * \param name: Name used to identify the scope. String is copied to heap in the profiler API.
 * \note Currently, transient zones are not supported on Metal and will be ignored.
 * \note Currently, category colors are not supported on transient zones.
 */
#define GPU_profile_scope_transient(name) \
  const PrfSourceLocationUnique(name, ProfileCategory::Default); \
  const gpu::ProfileScope _PRF_DEBUG_CONCAT(gpu_profile_scope_, __LINE__)( \
      &_GPU_DEBUG_CONCAT(gpu_profile_loc_, __LINE__), true);

}  // namespace blender::gpu
