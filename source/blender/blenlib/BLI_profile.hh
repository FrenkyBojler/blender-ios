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
 * - Any `name` arguments should be `ustr`s to ensure their lifetime is managed appropriately
 * - Any macro that takes a (text, size) pair should *not* include the size including the null
 *   terminator (i.e. should be equivalent to strlen(text))
 *
 * \see Tracy.hpp for a full list of supported macros
 * \see https://github.com/wolfpld/tracy/releases/latest/download/tracy.pdf
 */

#pragma once

#ifdef WITH_TRACY
#  include <tracy/Tracy.hpp>

/** Frame markers. */
#  define BLI_PROFILE_FRAME_MARK FrameMark
#  define BLI_PROFILE_FRAME_MARK_START(name) FrameMarkStart(name.c_str())
#  define BLI_PROFILE_FRAME_MARK_END(name) FrameMarkEnd(name.c_str())

/** Scoped zones, create a profiling zone lasting until end of current scope. */
#  define BLI_PROFILE_ZONE_SCOPED ZoneScoped
#  define BLI_PROFILE_ZONE_SCOPED_N(name) ZoneScopedN(name.c_str())

/** Set the zone name on a per-call basis. */
#  define BLI_PROFILE_ZONE_SET_NAME(text, size) ZoneName(text, size)
#  define BLI_PROFILE_ZONE_SET_NAME_FMT(fmt, ...) ZoneNameF(fmt, ##__VA_ARGS__)

/** Attach a text string to the zone (e.g. filename, object name). */
#  define BLI_PROFILE_ZONE_ADD_TEXT(text, size) ZoneText(text, size)
#  define BLI_PROFILE_ZONE_ADD_TEXT_FMT(fmt, ...) ZoneTextF(fmt, ##__VA_ARGS__)

/** Attach a numeric value to a zone */
#  define BLI_PROFILE_ZONE_ADD_VALUE(value) ZoneValue(value)

/**
 * Named zones, zones attached to a specific variable, allowing multiple zones in a single scope.
 */
#  define BLI_PROFILE_ZONE_NAMED(var) ZoneNamed(var, true)
#  define BLI_PROFILE_ZONE_NAMED_N(var, ui_name) ZoneNamedN(var, ui_name.c_str(), true)

/* Named zone variants, taking the attached variable name as first argument. */
#  define BLI_PROFILE_ZONE_SET_NAME_Z(var, text, size) \
    ZoneNameV(var, text, size)
#  define BLI_PROFILE_ZONE_SET_NAME_FMT_Z(var, fmt, ...) \
    ZoneNameVF(var, fmt, ##__VA_ARGS__)
#  define BLI_PROFILE_ZONE_ADD_TEXT_Z(var, text, size) \
    ZoneTextV(var, text, size)
#  define BLI_PROFILE_ZONE_ADD_TEXT_FMT_Z(var, fmt, ...) \
    ZoneTextVF(var, fmt, ##__VA_ARGS__)
#  define BLI_PROFILE_ZONE_ADD_VALUE_Z(var, value) ZoneValueV(var, value)

#else

#  define BLI_PROFILE_FRAME_MARK
#  define BLI_PROFILE_FRAME_MARK_START(name)
#  define BLI_PROFILE_FRAME_MARK_END(name)

#  define BLI_PROFILE_ZONE_SCOPED
#  define BLI_PROFILE_ZONE_SCOPED_N(name)

#  define BLI_PROFILE_ZONE_SET_NAME(text, size)
#  define BLI_PROFILE_ZONE_SET_NAME_FMT(fmt, ...)
#  define BLI_PROFILE_ZONE_ADD_TEXT(text, size)
#  define BLI_PROFILE_ZONE_ADD_TEXT_FMT(fmt, ...)
#  define BLI_PROFILE_ZONE_ADD_VALUE(value)

#  define BLI_PROFILE_ZONE_NAMED(var)
#  define BLI_PROFILE_ZONE_NAMED_N(var, ui_name)

#  define BLI_PROFILE_ZONE_SET_NAME_Z(var, text, size)
#  define BLI_PROFILE_ZONE_SET_NAME_FMT_Z(var, fmt, ...)
#  define BLI_PROFILE_ZONE_ADD_TEXT_Z(var, text, size)
#  define BLI_PROFILE_ZONE_ADD_TEXT_FMT_Z(var, fmt, ...)
#  define BLI_PROFILE_ZONE_ADD_VALUE_Z(var, value)

#endif
