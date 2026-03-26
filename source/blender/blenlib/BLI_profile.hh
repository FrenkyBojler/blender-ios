/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * A tiny wrapper around the TracyClient library profiling API which takes care of including the
 * Tracy header and wrapping its macros inside of BLI_PROFILE_* macros.
 * When building without TracyClient enabled the macros are evaluated to no-op.
 */

#pragma once

#ifdef WITH_TRACY_CLIENT
#  include <tracy/tracy/Tracy.hpp>
#endif

/* Scoped zones, create a profiling zone lasting until end of current scope. */
#ifdef WITH_TRACY_CLIENT
#  define BLI_profile_zone_scoped ZoneScoped
#  define BLI_profile_zone_scoped_n(name) ZoneScopedN(name)
#else
#  define BLI_profile_zone_scoped
#  define BLI_profile_zone_scoped_n(name)
#endif

/* Set dynamic zone name, text. TODO: Add color / value. */
#ifdef WITH_TRACY_CLIENT
#  define BLI_profile_zone_set_name(text, size) ZoneName(text, size)
#  define BLI_profile_zone_set_name_fmt(fmt, ...) ZoneNameF(fmt, ##__VA_ARGS__)
#  define BLI_profile_zone_set_text(text, size) ZoneText(text, size)
#  define BLI_profile_zone_set_text_fmt(fmt, ...) ZoneTextF(fmt, ##__VA_ARGS__)
#else
#  define BLI_profile_zone_set_name(text, size)
#  define BLI_profile_zone_set_name_fmt(fmt, ...)
#  define BLI_profile_zone_set_text(text, size)
#  define BLI_profile_zone_set_text_fmt(fmt, ...)
#endif

/* Frame markers to delimit profiler frames. */
#ifdef WITH_TRACY_CLIENT
#  define BLI_profile_frame_mark FrameMark
#  define BLI_profile_frame_mark_start(name) FrameMarkStart(name)
#  define BLI_profile_frame_mark_end(name) FrameMarkEnd(name)
#else
#  define BLI_profile_frame_mark
#  define BLI_profile_frame_mark_start(name)
#  define BLI_profile_frame_mark_end(name)
#endif

/* Memory allocation profiling. */
#ifdef WITH_TRACY_CLIENT
#  define BLI_profile_memory_alloc(ptr, size) TracyAlloc(ptr, size)
#  define BLI_profile_memory_alloc_n(ptr, size, name) TracyAllocN(ptr, size, name)
#  define BLI_profile_memory_free(ptr) TracyFree(ptr)
#  define BLI_profile_memory_free_n(ptr, name) TracyFreeN(ptr, name)
#else
#  define BLI_profile_memory_alloc(ptr, size)
#  define BLI_profile_memory_alloc_n(ptr, size, name)
#  define BLI_profile_memory_free(ptr, size)
#  define BLI_profile_memory_free_n(ptr, size, name)
#endif

/* Thread naming for identify threads in the profiler UI. */
#ifdef WITH_TRACY_CLIENT
#  define BLI_profile_set_thread_name(name) tracy::SetThreadName(name)
#  define BLI_profile_set_thread_name_with_hint(name, hint) tracy::SetThreadNameWithHint(name, hint)
#else
#  define BLI_profile_set_thread_name(name)
#  define BLI_profile_set_thread_name_with_hint(name, hint)
#endif
