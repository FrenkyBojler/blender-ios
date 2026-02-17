/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#pragma once

#include <concepts>

namespace blender {

/** Potentially use multiple threads to execute the function. */
struct ExecuteParallel {
  static constexpr bool is_parallel = true;
};

/** Execute the function in the current thread. */
struct ExecuteSerial {
  static constexpr bool is_parallel = false;
};

/**
 * Potentially use multiple threads to execute the function, with a configurable grain size to
 * influence the parallel task size.
 */
struct ExecuteParallelGrainSize {
  static constexpr bool is_parallel = true;
  int grain_size = 1;
};

/**
 * Argument used to control whether a function should use parallel execution or not.
 * \note For a version that doesn't require constexpr and can be passed to non-template functions,
 * see #ExecutionModeVariant.
 */
template<typename T>
concept ExecutionMode = requires {
  {
    T::is_parallel
  } -> std::convertible_to<bool>;
};

}  // namespace blender
