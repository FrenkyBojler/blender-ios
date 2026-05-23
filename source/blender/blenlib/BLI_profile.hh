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
 *
 * \see Tracy.hpp for a full list of supported macros
 * \see https://github.com/wolfpld/tracy/releases/latest/download/tracy.pdf
 */

#pragma once

/**
 * Set of category colors, chosen with color-blindness in mind.
 */
enum class Category : uint32_t {
  Default = 0x000001,
  Core = 0x0088FE,
  Draw = 0x00C49F,
  Editor = 0xFFBB28,
  Unused_1 = 0xFF8042,
  Unused_2 = 0x8884D8,
};

#ifdef WITH_TRACY
#  include <tracy/Tracy.hpp>

/** Frame markers. */
#  define PROFILE_FRAME_MARK FrameMark
#  define PROFILE_FRAME_MARK_START(name) FrameMarkStart(name.c_str())
#  define PROFILE_FRAME_MARK_END(name) FrameMarkEnd(name.c_str())

/** Profile the current scope, creating a Tracy zone. */
#  define PROFILE_SCOPE ZoneScoped
#  define PROFILE_SCOPE_WITH_NAME(name) ZoneScopedN(name)

/** Set the category (color) of the current zone. */
#  define PROFILE_SCOPE_SET_CATEGORY(category) ZoneColor(uint32_t(category))

/** Set the profiled zone's name on a per-call basis. */
#  define PROFILE_SCOPE_SET_DYNAMIC_NAME(fmt, ...) ZoneNameF(fmt, ##__VA_ARGS__)

/** Attach a text string to the current zone (e.g. filename, object name). */
#  define PROFILE_SCOPE_ADD_TEXT(fmt, ...) ZoneTextF(fmt, ##__VA_ARGS__)

/** Attach a numeric value to the current zone. */
#  define PROFILE_SCOPE_ADD_VALUE(value) ZoneValue(value)

/**
 * Profile the current scope, creating a Tracy zone.
 *
 * The zone is attached to the lifetime of `var` (e.g. for nested scopes).
 */
#  define PROFILE_SCOPE_VAR(var) ZoneNamed(var, true)
#  define PROFILE_SCOPE_VAR_WITH_NAME(var, ui_name) ZoneNamedN(var, ui_name.c_str(), true)

/** Set the category of the specified zone. */
#  define PROFILE_SCOPE_VAR_SET_CATEGORY(var, category) ZoneColorV(var, uint32_t(category))

/** Set the specified zone's name on a per-call basis. */
#  define PROFILE_SCOPE_VAR_SET_DYNAMIC_NAME(var, fmt, ...) ZoneNameVF(var, fmt, ##__VA_ARGS__)

/** Attach a text string to the specified zone (e.g. filename, object name). */
#  define PROFILE_SCOPE_VAR_ADD_TEXT(var, fmt, ...) ZoneTextVF(var, fmt, ##__VA_ARGS__)

/** Attach a numeric value to the specified zone */
#  define PROFILE_SCOPE_VAR_ADD_VALUE(var, value) ZoneValueV(var, value)

#else

#  define PROFILE_FRAME_MARK
#  define PROFILE_FRAME_MARK_START(name)
#  define PROFILE_FRAME_MARK_END(name)

#  define PROFILE_SCOPE
#  define PROFILE_SCOPE_WITH_NAME(name)
#  define PROFILE_SCOPE_SET_CATEGORY(category)

#  define PROFILE_SCOPE_SET_DYNAMIC_NAME(fmt, ...)
#  define PROFILE_SCOPE_ADD_TEXT(fmt, ...)
#  define PROFILE_SCOPE_ADD_VALUE(value)

#  define PROFILE_SCOPE_VAR(var)
#  define PROFILE_SCOPE_VAR_WITH_NAME(var, ui_name)
#  define PROFILE_SCOPE_VAR_SET_CATEGORY(var, category)

#  define PROFILE_SCOPE_VAR_SET_DYNAMIC_NAME(var, fmt, ...)
#  define PROFILE_SCOPE_VAR_ADD_TEXT(var, fmt, ...)
#  define PROFILE_SCOPE_VAR_ADD_VALUE(var, value)

#endif
