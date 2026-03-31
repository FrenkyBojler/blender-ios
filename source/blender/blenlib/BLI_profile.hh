/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * A tiny wrapper around the TracyClient library profiling API which takes care of including the
 * Tracy header and exposing it via BLI_profile_* macros. When building without Tracy enabled
 * the macros are evaluated to no-op.
 */

#pragma once

#ifdef WITH_TRACY_CLIENT
#  include <tracy/tracy/Tracy.hpp>
#endif

/* TODO: WIP incomplete API. See Tracy.hpp header for a complete list of implementable macros. */

#ifdef WITH_TRACY_CLIENT
/* Frame markers. */
#  define BLI_profile_frame_mark FrameMark
#  define BLI_profile_frame_mark_start(name) FrameMarkStart(name)
#  define BLI_profile_frame_mark_end(name) FrameMarkEnd(name)

/* Scoped zones, create a profiling zone lasting until end of current scope. */
#  define BLI_profile_zone_scoped ZoneScoped
#  define BLI_profile_zone_scoped_n(name) ZoneScopedN(name)
#  define BLI_profile_zone_scoped_c(color) ZoneScopedC(color)
#  define BLI_profile_zone_scoped_nc(name, color) ZoneScopedNC(name, color)

/* Named zones, zones attached to a specific string, allowing multiple zones in a single scope. */
#  define BLI_profile_zone_named(zone) ZoneNamed(zone, true)
#  define BLI_profile_zone_named_n(zone, ui_name) ZoneNamedN(zone_name, ui_name, true)
#  define BLI_profile_zone_named_c(zone, color) ZoneNamedC(zone_name, color, true)
#  define BLI_profile_zone_named_nc(zone, ui_name, color) ZoneNamedNC(zone_name, ui_name, color, true)

/* Set dynamic zone name, text, color, and value. */
#  define BLI_profile_zone_set_name(text, size) ZoneName(text, size)
#  define BLI_profile_zone_set_name_fmt(fmt, ...) ZoneNameF(fmt, ##__VA_ARGS__)
#  define BLI_profile_zone_set_text(text, size) ZoneText(text, size)
#  define BLI_profile_zone_set_text_fmt(fmt, ...) ZoneTextF(fmt, ##__VA_ARGS__)
#  define BLI_profile_zone_set_color(color) ZoneColor(color)
#  define BLI_profile_zone_set_value(value) ZoneValue(value)

/* Named zone variants, taking zone name as first argument. */
#  define BLI_profile_zone_set_name_z(zone, text, size) ZoneNameV(zone, text, size)
#  define BLI_profile_zone_set_name_fmt_z(zone, fmt, ...) ZoneNameVF(zone, fmt, ##__VA_ARGS__)
#  define BLI_profile_zone_set_text_z(zone, text, size) ZoneTextV(zone, text, size)
#  define BLI_profile_zone_set_text_fmt_z(zone, fmt, ...) ZoneTextVF(zone, fmt, ##__VA_ARGS__)
#  define BLI_profile_zone_set_color_z(zone, color) ZoneColorV(zone, color)
#  define BLI_profile_zone_set_value_z(zone, value) ZoneValueV(zone, value)

/* Memory allocation profiling. */
#  define BLI_profile_memory_alloc(ptr, size) TracyAlloc(ptr, size)
#  define BLI_profile_memory_free(ptr) TracyFree(ptr)

/* Set current thread name. */
#  define BLI_profile_set_thread_name(name) tracy::SetThreadName(name)
#  define BLI_profile_set_thread_name_with_hint(name, hint) tracy::SetThreadNameWithHint(name, hint)

#else
#  define BLI_profile_zone_scoped
#  define BLI_profile_zone_scoped_n(name)
#  define BLI_profile_zone_scoped_c(color)
#  define BLI_profile_zone_scoped_nc(name, color)

#  define BLI_profile_zone_named(zone_name)
#  define BLI_profile_zone_named_n(zone_name, ui_name)
#  define BLI_profile_zone_named_c(zone, color)
#  define BLI_profile_zone_named_nc(zone, ui_name, color)

#  define BLI_profile_zone_set_name(text, size)
#  define BLI_profile_zone_set_name_fmt(fmt, ...)
#  define BLI_profile_zone_set_text(text, size)
#  define BLI_profile_zone_set_text_fmt(fmt, ...)
#  define BLI_profile_zone_set_color(color)
#  define BLI_profile_zone_set_value(value)

#  define BLI_profile_zone_set_name_z(zone, text, size)
#  define BLI_profile_zone_set_name_fmt_z(zone, fmt, ...)
#  define BLI_profile_zone_set_text_z(zone, text, size)
#  define BLI_profile_zone_set_text_fmt_z(zone, fmt, ...)
#  define BLI_profile_zone_set_color_z(zone, color)
#  define BLI_profile_zone_set_value_z(zone, value)

#  define BLI_profile_frame_mark
#  define BLI_profile_frame_mark_start(name)
#  define BLI_profile_frame_mark_end(name)

#  define BLI_profile_memory_alloc(ptr, size)
#  define BLI_profile_memory_alloc_n(ptr, size, name)
#  define BLI_profile_memory_free(ptr, size)
#  define BLI_profile_memory_free_n(ptr, size, name)

#  define BLI_profile_set_thread_name(name)
#  define BLI_profile_set_thread_name_with_hint(name, hint)
#endif
