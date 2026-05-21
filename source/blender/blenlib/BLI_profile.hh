/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * A tiny wrapper around the TracyClient library profiling API which takes care of including the
 * Tracy header and exposing it via BLI_PROFILE_* macros. When building without Tracy enabled
 * the macros are evaluated to no-op.
 *
 * Important considerations:
 * - Any named arguments should have a stable pointer to ensure Tracy associates names correctly.
 *   \see BLI_PROFILE_STABLE_IDENTIFIER
 * - Any macro that takes a (text, size) pair should *not* include the size including the null
 *   terminator (i.e. should be equivalent to strlen(text))
 *
 * \see Tracy.hpp for a full list of supported macros
 * \see https://github.com/wolfpld/tracy/releases/latest/download/tracy.pdf
 */

#pragma once

#ifdef WITH_TRACY
#  include <tracy/Tracy.hpp>

#  define BLI_PROFILE_STABLE_IDENTIFIER(variable_name, text) \
static const char *variable_name = text

/** Frame markers. */
#  define BLI_PROFILE_FRAME_MARK FrameMark
#  define BLI_PROFILE_FRAME_MARK_START(name) FrameMarkStart(name)
#  define BLI_PROFILE_FRAME_MARK_END(name) FrameMarkEnd(name)

/** Scoped zones, create a profiling zone lasting until end of current scope. */
#  define BLI_PROFILE_ZONE_SCOPED ZoneScoped
#  define BLI_PROFILE_ZONE_SCOPED_N(name) ZoneScopedN(name)
#  define BLI_PROFILE_ZONE_SCOPED_C(color) ZoneScopedC(color)
#  define BLI_PROFILE_ZONE_SCOPED_NC(name, color) ZoneScopedNC(name, color)

/** Set the zone name on a per-call basis. */
#  define BLI_PROFILE_ZONE_SET_NAME(text, size) ZoneName(text, size)
#  define BLI_PROFILE_ZONE_SET_NAME_FMT(fmt, ...) ZoneNameF(fmt, ##__VA_ARGS__)

/** Set the color of the zone. */
#  define BLI_PROFILE_ZONE_SET_COLOR(color) ZoneColor(color)

/** Attach a text string to the zone (e.g. filename, object name). */
#  define BLI_PROFILE_ZONE_ADD_TEXT(text, size) ZoneText(text, size)
#  define BLI_PROFILE_ZONE_ADD_TEXT_FMT(fmt, ...) ZoneTextF(fmt, ##__VA_ARGS__)

/** Attach a numeric value to a zone */
#  define BLI_PROFILE_ZONE_ADD_VALUE(value) ZoneValue(value)

/**
 * Named zones, zones attached to a specific variable, allowing multiple zones in a single scope.
 */
#  define BLI_PROFILE_ZONE_NAMED(variable_name) ZoneNamed(variable_name, true)
#  define BLI_PROFILE_ZONE_NAMED_N(variable_name, ui_name) ZoneNamedN(variable_name, ui_name, true)
#  define BLI_PROFILE_ZONE_NAMED_C(variable_name, color) ZoneNamedC(variable_name, color, true)
#  define BLI_PROFILE_ZONE_NAMED_NC(variable_name, ui_name, color) \
    ZoneNamedNC(variable_name, ui_name, color, true)

/* Named zone variants, taking the attached variable name as first argument. */
#  define BLI_PROFILE_ZONE_SET_NAME_Z(variable_name, text, size) \
    ZoneNameV(variable_name, text, size)
#  define BLI_PROFILE_ZONE_SET_NAME_FMT_Z(variable_name, fmt, ...) \
    ZoneNameVF(variable_name, fmt, ##__VA_ARGS__)
#  define BLI_PROFILE_ZONE_SET_COLOR_Z(variable_name, color) ZoneColorV(variable_name, color)
#  define BLI_PROFILE_ZONE_ADD_TEXT_Z(variable_name, text, size) \
    ZoneTextV(variable_name, text, size)
#  define BLI_PROFILE_ZONE_ADD_TEXT_FMT_Z(variable_name, fmt, ...) \
    ZoneTextVF(variable_name, fmt, ##__VA_ARGS__)
#  define BLI_PROFILE_ZONE_ADD_VALUE_Z(variable_name, value) ZoneValueV(variable_name, value)

#else

#  define BLI_PROFILE_STABLE_IDENTIFIER(variable_name, text)

#  define BLI_PROFILE_FRAME_MARK
#  define BLI_PROFILE_FRAME_MARK_START(name)
#  define BLI_PROFILE_FRAME_MARK_END(name)

#  define BLI_PROFILE_ZONE_SCOPED
#  define BLI_PROFILE_ZONE_SCOPED_N(name)
#  define BLI_PROFILE_ZONE_SCOPED_C(color)
#  define BLI_PROFILE_ZONE_SCOPED_NC(name, color)

#  define BLI_PROFILE_ZONE_NAMED(zone)
#  define BLI_PROFILE_ZONE_NAMED_N(zone, ui_name)
#  define BLI_PROFILE_ZONE_NAMED_C(zone, color)
#  define BLI_PROFILE_ZONE_NAMED_NC(zone, ui_name, color)

#  define BLI_PROFILE_ZONE_SET_NAME(text, size)
#  define BLI_PROFILE_ZONE_SET_NAME_FMT(fmt, ...)
#  define BLI_PROFILE_ZONE_ADD_TEXT(text, size)
#  define BLI_PROFILE_ZONE_ADD_TEXT_FMT(fmt, ...)
#  define BLI_PROFILE_ZONE_SET_COLOR(color)
#  define BLI_PROFILE_ZONE_ADD_VALUE(value)

#  define BLI_PROFILE_ZONE_SET_NAME_Z(zone, text, size)
#  define BLI_PROFILE_ZONE_SET_NAME_FMT_Z(zone, fmt, ...)
#  define BLI_PROFILE_ZONE_ADD_TEXT_Z(zone, text, size)
#  define BLI_PROFILE_ZONE_ADD_TEXT_FMT_Z(zone, fmt, ...)
#  define BLI_PROFILE_ZONE_SET_COLOR_Z(zone, color)
#  define BLI_PROFILE_ZONE_ADD_VALUE_Z(zone, value)

#  define BLI_PROFILE_MEMORY_ALLOC(ptr, size)
#  define BLI_PROFILE_MEMORY_FREE(ptr)

#endif
