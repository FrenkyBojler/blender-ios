/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

namespace blender::xpbd {

/**
 * Reference to the data that is actually being simulated.
 */
struct GeometryRef {
  /** The position of each point. */
  MutableSpan<float3> positions;
  /** The linear velocity of each point. */
  MutableSpan<float3> velocities;
  /** Positions before time integration. */
  Span<float3> prev_positions;
  /* Inverse mass of each point. This is expected to be zero for pinned points. */
  Span<float> inverse_masses;

  /** Optional rotation data. */
  MutableSpan<math::Quaternion> rotations;
  /** Optional angular_velocity data. */
  MutableSpan<float3> angular_velocities;
  /** Rotations before time integration. */
  Span<math::Quaternion> prev_rotations;
  Span<float3> moments_of_inertia;
  Span<float3> inverse_moments_of_inertia;

  uint64_t size() const;
};

}  // namespace blender::xpbd
