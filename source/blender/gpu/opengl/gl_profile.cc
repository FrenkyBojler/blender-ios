/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Profiling annotations features for OpenGL backend.
 */

#include "gl_context.hh"
#include <utility>

namespace blender::gpu {

void GLContext::profile_context()
{
#ifdef WITH_TRACY
  profile_is_active_ = true;
  TracyGpuContext;
#endif
}

void GLContext::profile_collect()
{
#ifdef WITH_TRACY
  tracy::GpuCtx *ctx = tracy::GetGpuCtx().ptr;
  if (!ctx || !profile_is_active_) {
    return;
  }
  TracyGpuCollect;
#endif
}

void GLContext::profile_scope_begin(const PrfSourceLocation *loc)
{
#ifdef WITH_TRACY
  tracy::GpuCtx *ctx = tracy::GetGpuCtx().ptr;
  if (!ctx || !profile_is_active_) {
    return;
  }
  profile_scopes_.push_as(std::make_unique<GLProfileScope>(loc, true));
#endif
}

void GLContext::profile_scope_begin_transient(const PrfSourceLocation *loc)
{
#ifdef WITH_TRACY
  tracy::GpuCtx *ctx = tracy::GetGpuCtx().ptr;
  if (!ctx || !profile_is_active_) {
    return;
  }
  profile_scopes_.push_as(std::make_unique<GLProfileScope>(loc->line,
                                                           loc->file,
                                                           strlen(loc->file),
                                                           loc->function,
                                                           strlen(loc->function),
                                                           loc->name,
                                                           strlen(loc->name),
                                                           true));
#endif
}

void GLContext::profile_scope_end()
{
#ifdef WITH_TRACY
  tracy::GpuCtx *ctx = tracy::GetGpuCtx().ptr;
  if (!ctx || !profile_is_active_) {
    return;
  }
  profile_scopes_.pop();
#endif
}
}  // namespace blender::gpu
