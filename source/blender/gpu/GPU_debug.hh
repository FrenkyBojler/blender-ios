/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Helpers for GPU/draw debugging. Supports grouping and capture of GPU API calls in a
 * GPU frame capture tool via GPU_debug_* macros and gpu::Debug* objects.
 *
 * Examples:
 *
 * ### Frame capture ###
 *
 * Will trigger a capture inside e.g. Renderdoc or XCode. You must build with Renderdoc API
 * support for this to work; enable the `WITH_RENDERDOC` CMake flag.
 *
 * \code
 * #include "GPU_debug.hh"
 *
 * void render_function()
 * {
 *   GPU_debug_capture_scope();
 *   // Draw call submissions go here.
 * }
 * \endcode
 *
 * ### Scope capture ###
 *
 * A selective capture of named scopes can be sprinkled around the codebase. They are listed
 * inside XCode (Mac) when doing a Metal capture. On OpenGL/Vulkan, you can use the
 * `--debug-gpu-scope-capture <name>` launch argument to trigger a specific scope for capture. You
 * must build with Renderdoc API support for this to work; enable the `WITH_RENDERDOC` CMake flag.
 *
 * Scopes can be nested, but only one can be captured at a time.
 *
 * \code
 * #include "GPU_debug.hh"
 *
 * void render_function()
 * {
 *   GPU_debug_capture_scope("A");
 *   // Draw call submissions go here.
 *
 *   {
 *      GPU_debug_capture_scope("B");
 *      // More draw call submissions go here.
 *   }
 * }
 * \endcode
 *
 * ### Object variants ###
 *
 * `GPU_debug_group_scope` and `GPU_debug_capture_scope` are wrappers that only have lifetime in
 * the current scope. For more complex control flow, you can use the `gpu::DebugGroup` and
 * `gpu::DebugCapture` structs to retain capture across function calls. Note that
 * `gpu::DebugCapture` must be static.
 *
 * \code
 * #include "GPU_debug.hh"
 *
 * static gpu::DebugCapture capture;
 *
 * void render_begin_function() {
 *   // Optionally, name the capture for use with launch argument `--debug-gpu-scope-capture`.
 *   capture = "Render Pass";
 *   capture.begin();
 *   // Some draw call submissions here
 * }
 *
 * void render_end_function() {
 *   // Some draw call submissions here
 *   capture.end();
 * }
 * \endcode
 */

#pragma once

#include "BLI_index_range.hh"
#include <source_location>
#include <string>

namespace blender {

#define GPU_DEBUG_SHADER_COMPILATION_GROUP "Shader Compilation"
#define GPU_DEBUG_SHADER_SPECIALIZATION_GROUP "Shader Specialization"

/* Internal helpers. */
#define _GPU_DEBUG_CONCAT_EXPAND(prefix, suffix) prefix##suffix
#define _GPU_DEBUG_CONCAT(prefix, suffix) _GPU_DEBUG_CONCAT_EXPAND(prefix, suffix)

namespace gpu {

namespace detail {
/* Scope guard type, encapsulates gpu::DebugGroup and gpu::DebugCapture. */
template<typename T> struct Scope {
  T &debug_object_;

  Scope(T &object, const std::source_location location = std::source_location::current())
      : debug_object_(object)
  {
    debug_object_.begin(location);
  }

  ~Scope()
  {
    debug_object_.end();
  }
};
}  // namespace detail

/**
 * GPU debug grouping support.
 *
 * Allows annotated grouping of a region of GPU API calls under a given name, for display in
 * a GPU frame capture tool or profiling tool.
 *
 * \note As shorthand to group API calls under the current scope, use
 * `GPU_debug_group_scope(name);`.
 */
class DebugGroup {
  const char *name_ = nullptr;

 public:
  /**
   * Construct a GPU debug group with a given name.
   *
   * \param name: Unique group name displayed within capture tool.
   */
  DebugGroup(const char *name = nullptr) : name_(name) {}

  /**
   * Begin grouping of API calls within this group.
   *
   * \param location: Defaulted information about the caller source location.
   */
  void begin(const std::source_location location = std::source_location::current());

  /**
   * End grouping of GPU API calls within this group.
   */
  void end();

  /**
   * Create a scope guard to group all GPU API calls until the end of the current scope.
   *
   * \param location: Defaulted information about the caller source location.
   * \return A scope guard encapsulating this DebugGroup.
   * \note As shorthand, use `GPU_debug_group_scope(name)`.
   */
  detail::Scope<DebugGroup> scope_guard(
      const std::source_location location = std::source_location::current())
  {
    return {*this, location};
  }
};

/**
 * GPU debug capture support.
 *
 * Allows for the deferred capture of a region of GPU API calls within an external GPU frame
 * capture tool. DebugCapture can create named and unnamed regions. If a capture is unnamed, it
 * will trigger immediately within the used capture tool. If it is named, it will trigger upon
 * request only with the launch argument `--debug-gpu-scope-capture name`.
 *
 * \note A *named* debug capture object should be created a single time and made static.
 * \note As shorthand to capture API calls under the current scope, use
 * `GPU_debug_capture_scope();` or `GPU_debug_optional_capture_scope(name)`.
 */
class DebugCapture {
  void *capture_p_ = nullptr;

 public:
  /**
   * Construct a GPU debug capture with a given name.
   *
   * \param name: Unique but optional name used to identify the capture scope. If no argument
   *              is provided, the capture will trigger immediately.
   */
  DebugCapture(const char *name = nullptr);

  /**
   * Begin capture of GPU API calls within this group.
   *
   * \param location: Defaulted information about the caller source location.
   */
  void begin(const std::source_location location = std::source_location::current());

  /**
   * End capture of GPU API calls within this group.
   */
  void end();

  /**
   * Create a scope guard to capture all GPU API calls until the end of the current scope.
   *
   * \param location: Defaulted information about the caller source location.
   * \return A scope guard encapsulating this DebugCapture.
   * \note As shorthand, use `GPU_debug_capture_scope()` or
   * `GPU_debug_optional_capture_scope(name)`.
   */
  detail::Scope<DebugCapture> scope_guard(
      const std::source_location location = std::source_location::current())
  {
    return {*this, location};
  }
};
}  // namespace gpu

/**
 * Perform grouping of GPU API calls under the current scope by the given name, for display in a
 * GPU frame capture tool or profiling tool.
 *
 * \param name: Unique group name displayed within capture tool.
 * \note This is equivalent to: ```
 *   gpu::DebugGroup group = name;
 *   const auto scope_guard = group.scope_guard();
 * ```
 */
#define GPU_debug_group_scope(name) \
  static gpu::DebugGroup _GPU_DEBUG_CONCAT(gpu_debug_group_, __LINE__)(name); \
  const auto _GPU_DEBUG_CONCAT( \
      gpu_debug_group_scope_guard_, \
      __LINE__) = _GPU_DEBUG_CONCAT(gpu_debug_group_, __LINE__).scope_guard();

/**
 * Perform deferrred capture of GPU API calls under the current scope within an external GPU frame
 * capture tool.
 *
 * \note This is equivalent to: ```
 *   static gpu::DebugCapture capture;
 *   const auto scope_guard = capture.scope_guard();
 * ```
 */
#define GPU_debug_capture_scope() \
  static gpu::DebugCapture _GPU_DEBUG_CONCAT(gpu_debug_capture_, __LINE__)(); \
  const auto _GPU_DEBUG_CONCAT( \
      gpu_debug_capture_scope_guard_, \
      __LINE__) = _GPU_DEBUG_CONCAT(gpu_debug_capture_, __LINE__).scope_guard();

/**
 * Perform deferred capture of GPU API calls under the current scope within an external GPU frame
 * capture tool. The capture is named, and triggrs only upon request with the launch argument
 * `--debug-gpu-scope-capture name`.
 *
 * \param name: Unique name used to identify the capture scope.
 * \note This is equivalent to: ```
 *   static gpu::DebugCapture capture = name;
 *   const auto scope_guard = capture.scope_guard();
 * ```
 */
#define GPU_debug_optional_capture_scope(name) \
  static gpu::DebugCapture _GPU_DEBUG_CONCAT(gpu_debug_capture_, __LINE__)(name); \
  const auto _GPU_DEBUG_CONCAT( \
      gpu_debug_capture_scope_guard_, \
      __LINE__) = _GPU_DEBUG_CONCAT(gpu_debug_capture_, __LINE__).scope_guard();

/**
 * Create a formatted string displaying the current grouping hierarchy in the format
 * `Group1 > Group2 > ... > GroupN`. C-style method.
 *
 * \param name_buf_len: Length of the char buffer to the string write into.
 * \param r_name_buf: Char buffer to write the string into.
 */
void GPU_debug_get_groups_names(int name_buf_len, char *r_name_buf);

/**
 * Create a formatted string displaying the current grouping hierarchy in the format
 * `Group1 > Group2 > ... > GroupN`.
 *
 * \param levels: Index range of the hierarchy levels to output.
 * \return A formatted string of the hierarchy.
 */
std::string GPU_debug_get_groups_names(IndexRange levels = IndexRange(0, 9999));

/**
 * Return true if inside a debug group with the given name.
 */
bool GPU_debug_group_match(const char *ref);

}  // namespace blender
