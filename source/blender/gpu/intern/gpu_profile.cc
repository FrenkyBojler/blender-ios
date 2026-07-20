/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "GPU_profile.hh"
#include "gpu_context_private.hh"

namespace blender::gpu {

ProfileScope::ProfileScope(const PrfSourceLocation *location, bool is_transient)
{
#if defined(WITH_TRACY) && defined(WITH_TRACY_GPU)
  /* No context active. */
  Context *ctx = Context::get();
  if (!ctx) {
    return;
  }
  if (is_transient) {
    ctx->profile_scope_begin_transient(location);
  }
  else {
    ctx->profile_scope_begin(location);
  }
#endif
}

ProfileScope::~ProfileScope()
{
#if defined(WITH_TRACY) && defined(WITH_TRACY_GPU)
  /* No context active. */
  Context *ctx = Context::get();
  if (!ctx) {
    return;
  }
  ctx->profile_scope_end();
#endif
}

}  // namespace blender::gpu
