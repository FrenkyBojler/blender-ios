/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <ostream>
#include <stdint.h>

#include "BLI_hash.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_struct_equality_utils.hh"

namespace blender {

/**
 * Motion Vector
 *
 * A vector that stores the velocity of an object in screen space from the current frame to the
 * previous frame and from the next frame to the current frame.
 */
class MotionVector {
 public:
  float2 previous = float2(0.0f);
  float2 next = float2(0.0f);

  MotionVector() = default;

  /** Construct a motion vector that is diagonal in direction and has the given speed for both the
   * previous and next velocities. */
  MotionVector(const float speed) : previous(speed), next(speed) {}

  /** Construct a motion vector that has the given velocity for both the previous and next
   * velocities. */
  MotionVector(const float2 velocity) : previous(velocity), next(velocity) {}

  /** Construct a motion vector that has the given previous and next velocities. */
  MotionVector(const float2 previous, const float2 next) : previous(previous), next(next) {}

  /** Conversion from pointers (from C-style vectors). */

  MotionVector(const float *ptr)
  {
    this->previous = float2(ptr);
    this->next = float2(ptr + 2);
  }

  /** C-style pointer dereference. */

  operator const float *() const
  {
    return reinterpret_cast<const float *>(this);
  }

  operator float *()
  {
    return reinterpret_cast<float *>(this);
  }

  friend std::ostream &operator<<(std::ostream &stream, const MotionVector &value)
  {
    stream << "previous" << value.previous << ", next" << value.next;
    return stream;
  }

  BLI_STRUCT_EQUALITY_OPERATORS_2(MotionVector, previous, next)

  uint64_t hash() const
  {
    return get_default_hash(this->previous, this->next);
  }
};

}  // namespace blender
