/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Profiling macros for Blender. Supports Tracy and Perfetto as backends.
 *
 * When building with Tracy (`WITH_TRACY`), the macros delegate to the Tracy API.
 * When building with Perfetto (`WITH_PERFETTO`), the macros delegate to the thin wrapper in
 * `intern/perfetto` (perfetto_trace.hh) which keeps the heavy Perfetto SDK out of every
 * translation unit.
 * When neither backend is enabled the macros expand to no-ops.
 *
 * Important considerations:
 * - Any `name` arguments should be `ustr`s to ensure their lifetime is managed appropriately
 *
 * \see Tracy.hpp for a full list of Tracy macros.
 * \see https://github.com/wolfpld/tracy/releases/latest/download/tracy.pdf
 * \see intern/perfetto/perfetto_trace.hh for the Perfetto wrapper API.
 */

#ifdef WITH_TRACY
#  include <tracy/Tracy.hpp>
#endif

#ifdef WITH_PERFETTO
#  include "perfetto_trace.hh"
#endif


namespace blender {
/**
 * Set of category colors, chosen with color-blindness in mind.
 */
enum class ProfileCategory : uint32_t {
  /**
   * \note Not pure black (0x000000) as Tracy uses that to indicate "no user provided color".
   */
  Default = 0x000001,
  Core = 0x0088FE,
  Draw = 0x00C49F,
  Editor = 0xFFBB28,
  Unused_1 = 0xFF8042,
  Unused_2 = 0x8884D8,
};

#ifdef WITH_TRACY

/** Frame markers. */
#  define BLI_profile_frame_mark FrameMark
#  define BLI_profile_frame_mark_start(name) FrameMarkStart(name.c_str())
#  define BLI_profile_frame_mark_end(name) FrameMarkEnd(name.c_str())

/** Profile the current scope, creating a Tracy zone. */
#  define BLI_profile_scope(category) ZoneScopedC(uint32_t(category))
#  define BLI_profile_scope_with_name(name, category) ZoneScopedNC(name, uint32_t(category))

/** Set the profiled zone's name on a per-call basis. */
#  define BLI_profile_scope_set_dynamic_name(fmt, ...) ZoneNameF(fmt, ##__VA_ARGS__)

/** Attach a text string to the current zone (e.g. filename, object name). */
#  define BLI_profile_scope_add_text(fmt, ...) ZoneTextF(fmt, ##__VA_ARGS__)

/** Attach a numeric value to the current zone. */
#  define BLI_profile_scope_add_value(value) ZoneValue(value)

/**
 * Profile the current scope, creating a Tracy zone.
 *
 * The zone is attached to the lifetime of `var` (e.g. for nested scopes).
 */
#  define BLI_profile_scope_var(var, category) ZoneNamedC(var, uint32_t(category), true)
#  define BLI_profile_scope_var_with_name(var, ui_name, category) \
    ZoneNamedNC(var, ui_name.c_str(), uint32_t(category), true)

/** Set the specified zone's name on a per-call basis. */
#  define BLI_profile_scope_var_set_dynamic_name(var, fmt, ...) ZoneNameVF(var, fmt, ##__VA_ARGS__)

/** Attach a text string to the specified zone (e.g. filename, object name). */
#  define BLI_profile_scope_var_add_text(var, fmt, ...) ZoneTextVF(var, fmt, ##__VA_ARGS__)

/** Attach a numeric value to the specified zone. */
#  define BLI_profile_scope_var_add_value(var, value) ZoneValueV(var, value)

#elif defined(WITH_PERFETTO)

/** Helper to concatenate tokens (needed to generate unique variable names). */
#  define BLI_PROFILE_CONCAT_INNER(a, b) a##b
#  define BLI_PROFILE_CONCAT(a, b) BLI_PROFILE_CONCAT_INNER(a, b)

/**
 * Map a `ProfileCategory` enum value to the Perfetto category string used in
 * PERFETTO_DEFINE_CATEGORIES (see intern/perfetto/perfetto_trace.cc).
 */
constexpr const char *bli_profile_category_str(::blender::ProfileCategory cat)
{
  switch (cat) {
    case ::blender::ProfileCategory::Core:
      return "core";
    case ::blender::ProfileCategory::Draw:
      return "draw";
    case ::blender::ProfileCategory::Editor:
      return "editor";
    case ::blender::ProfileCategory::Default:
    default:
      return "default";
  }
}

/** Frame markers. */
#  define BLI_profile_frame_mark ::blender::perfetto_frame_mark()
#  define BLI_profile_frame_mark_start(name) ::blender::perfetto_frame_mark_start((name).c_str())
#  define BLI_profile_frame_mark_end(name) ::blender::perfetto_frame_mark_end((name).c_str())

/**
 * Profile the current scope using a Perfetto RAII guard.
 * The guard is named `_bli_pscope_<line>` to avoid collisions in the same function.
 */
#  define BLI_profile_scope(category) \
    ::blender::PerfettoScope BLI_PROFILE_CONCAT(_bli_pscope_, __LINE__)( \
        ::blender::bli_profile_category_str(category), __func__)

/** `name` is expected to be a `const char*` string literal. */
#  define BLI_profile_scope_with_name(name, category) \
    ::blender::PerfettoScope BLI_PROFILE_CONCAT(_bli_pscope_, __LINE__)( \
        ::blender::bli_profile_category_str(category), name)

/**
 * Set the profiled scope's name dynamically.
 * With Perfetto the name is set at scope-begin time, so this is a no-op for now.
 * Use `BLI_profile_scope_with_name` to provide a name up-front.
 */
#  define BLI_profile_scope_set_dynamic_name(fmt, ...) ((void)0)

/** Attach a text annotation to the current scope. */
#  define BLI_profile_scope_add_text(fmt, ...) \
    ::blender::perfetto_scope_add_annotation("text", fmt)

/** Attach a numeric value to the current scope. */
#  define BLI_profile_scope_add_value(value) \
    ::blender::perfetto_scope_add_value("value", static_cast<uint64_t>(value))

/**
 * Profile the current scope, binding the RAII guard to `var`.
 * `var` can be used with the `_var_` family of macros below.
 */
#  define BLI_profile_scope_var(var, category) \
    ::blender::PerfettoScope var(::blender::bli_profile_category_str(category), __func__)

/** `ui_name` is expected to be a `const char*` string literal. */
#  define BLI_profile_scope_var_with_name(var, ui_name, category) \
    ::blender::PerfettoScope var(::blender::bli_profile_category_str(category), ui_name)

/**
 * The `_var_` annotation macros operate on the scope bound to `var`.
 * With Perfetto the annotations are emitted on the current thread track regardless of `var`,
 * so `var` is accepted but unused (the annotations still go to the right slice because Perfetto
 * matches them to the innermost open slice on the thread).
 */
#  define BLI_profile_scope_var_set_dynamic_name(var, fmt, ...) ((void)(var), (void)0)

#  define BLI_profile_scope_var_add_text(var, fmt, ...) \
    ((void)(var), ::blender::perfetto_scope_add_annotation("text", fmt))

#  define BLI_profile_scope_var_add_value(var, value) \
    ((void)(var), ::blender::perfetto_scope_add_value("value", static_cast<uint64_t>(value)))

#else

#  define BLI_profile_frame_mark
#  define BLI_profile_frame_mark_start(name)
#  define BLI_profile_frame_mark_end(name)

#  define BLI_profile_scope(category)
#  define BLI_profile_scope_with_name(name, category)

#  define BLI_profile_scope_set_dynamic_name(fmt, ...)
#  define BLI_profile_scope_add_text(fmt, ...)
#  define BLI_profile_scope_add_value(value)

#  define BLI_profile_scope_var(var, category)
#  define BLI_profile_scope_var_with_name(var, ui_name, category)

#  define BLI_profile_scope_var_set_dynamic_name(var, fmt, ...)
#  define BLI_profile_scope_var_add_text(var, fmt, ...)
#  define BLI_profile_scope_var_add_value(var, value)

#endif

}  // namespace blender
