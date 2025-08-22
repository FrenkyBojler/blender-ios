/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_quaternion.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_mutex.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::xpbd {

/**
 * Reference to the data that is actually being simulated.
 */
struct GeometryRef {
  /** The position of each points. */
  MutableSpan<float3> positions;
  /* Inverse mass of each point. This is expected to be zero for pinned points. */
  Span<float> inverse_masses;

  /** Optional rotation data. */
  MutableSpan<math::Quaternion> rotations;
  Span<float3> inertias;
  Span<float3> inverse_inertias;

  uint64_t size() const;
};

/** Provides access to the input data that should be considered by a constraint. */
class ConstraintSetParams {
 private:
  Span<GeometryRef> geometry_refs_;

 public:
  ConstraintSetParams(Span<GeometryRef> geometry_refs);

  const float3 &position(int geo_i, int point_i) const;
  const math::Quaternion &rotation(int geo_i, int point_i) const;

  Span<float3> positions(int geo_i) const;
  Span<math::Quaternion> rotations(int geo_i) const;

  float inverse_mass(int geo_i, int point_i) const;
  Span<float> inverse_masses(int geo_i) const;

  float3 inertia(int geo_i, int point_i) const;
  Span<float3> inertias(int geo_i) const;

  float3 inverse_inertia(int geo_i, int point_i) const;
  Span<float3> inverse_inertias(int geo_i) const;
};

/**
 * Updater that writes the changes directly to the simulated points.
 */
class GaussSeidelUpdater {
 private:
  Span<GeometryRef> geometry_refs_;

 public:
  GaussSeidelUpdater(Span<GeometryRef> geometry_refs);
  void update_position(const int geo_i, const int point_i, const float3 &offset);
  void update_rotation(const int geo_i, const int point_i, const math::Quaternion &offset);
};

/**
 * Updater that writes that accumulates all changes into a separate array. This is
 * non-deterministic because float addition is not commutative. It mainly exists for testing
 * purposes.
 */
class NonDeterministicJacobianUpdater {
 public:
  struct Item {
    Mutex linear_mutex;
    int linear_counter = 0;
    float3 linear_offset = float3(0.0f);
    Mutex rotation_mutex;
    int rotation_counter = 0;
    float4 rotation_offset = float4(0.0f);
  };

 private:
  Span<MutableSpan<Item>> offsets_;

 public:
  NonDeterministicJacobianUpdater(Span<MutableSpan<Item>> offsets);
  void update_position(const int geo_i, const int point_i, const float3 &offset);
  void update_rotation(const int geo_i, const int point_i, const math::Quaternion &offset);
};

/**
 * Base class for constraint evaluators. It evaluate a batch of constraints and writes back the
 * results using a passed in "updater".
 *
 * Use #TemplatedConstraintSet to instantiate the constraint evaluation for each updater
 * automatically. This avoids having to implement separate Jacobian and Gauss Seidel code paths for
 * such constraints.
 */
class ConstraintSet {
 protected:
  Vector<int> affected_geo_indices_;

 public:
  ConstraintSet(Vector<int> affected_geo_indices);

  virtual ~ConstraintSet() = default;

  /**
   * Simplest evaluation method. It evaluates all the constraints serially, one at a time. This is
   * mainly meant for debugging purposes.
   */
  virtual void evaluate_gauss_seidel_one_at_a_time(GaussSeidelUpdater &updater,
                                                   ConstraintSetParams &params) const = 0;

  /**
   * Evaluates the constraints in parallel if possible. Internally, it may use
   * constraint-graph-coloring or other strategies to split up the constraints into independent
   * sets that can be solved in parallel.
   */
  virtual void evaluate_gauss_seidel_parallel(GaussSeidelUpdater &updater,
                                              ConstraintSetParams &params) const = 0;

  /**
   * Evaluates the constraints in parallel.
   * TODO: Replace this with a deterministic Jacobian solver.
   */
  virtual void evaluate_jacobian_non_deterministic_parallel(
      NonDeterministicJacobianUpdater &updater, ConstraintSetParams &params) const = 0;

  Span<int> get_affected_geo_indices() const;
};

/**
 * Slow but simple iterative Gauss Seidel solver. It evaluates each constraints serially without
 * any parallelism.
 */
void solve_gauss_seidel_one_at_a_time(Span<GeometryRef> geometry_refs,
                                      Span<const ConstraintSet *> constraint_sets);

/**
 * Fully parallel Jacobian solver, but it is not deterministic. This is mainly for testing
 * purposes.
 */
void solve_jacobian_non_deterministic(Span<GeometryRef> geometry_refs,
                                      Span<const ConstraintSet *> constraint_sets);

/**
 * A Gauss Seidel solver that attempts to parallelize the evaluation of constraints.
 */
void solve_gauss_seidel_parallel(Span<GeometryRef> geometry_refs,
                                 Span<const ConstraintSet *> constraint_sets);

/* -------------------------------------------------------------------- */
/** \name Inline Functions
 * \{ */

inline uint64_t GeometryRef::size() const
{
  return this->positions.size();
}

inline math::Quaternion apply_rotation_offset(math::Quaternion rotation, float4 offset)
{
  return math::normalize(math::Quaternion(float4(rotation) + offset));
}

inline NonDeterministicJacobianUpdater::NonDeterministicJacobianUpdater(
    Span<MutableSpan<Item>> offsets)
    : offsets_(offsets)
{
}

inline void NonDeterministicJacobianUpdater::update_position(const int geo_i,
                                                             const int point_i,
                                                             const float3 &offset)
{
  Item &item = offsets_[geo_i][point_i];
  std::lock_guard lock(item.linear_mutex);
  item.linear_counter++;
  item.linear_offset += offset;
}

inline void NonDeterministicJacobianUpdater::update_rotation(const int geo_i,
                                                             const int point_i,
                                                             const math::Quaternion &offset)
{
  Item &item = offsets_[geo_i][point_i];
  std::lock_guard lock(item.rotation_mutex);
  item.rotation_counter++;
  item.rotation_offset += float4(offset);
}

inline GaussSeidelUpdater::GaussSeidelUpdater(Span<GeometryRef> geometry_refs)
    : geometry_refs_(geometry_refs)
{
}

inline void GaussSeidelUpdater::update_position(const int geo_i,
                                                const int point_i,
                                                const float3 &offset)
{
  geometry_refs_[geo_i].positions[point_i] += offset;
}

inline void GaussSeidelUpdater::update_rotation(const int geo_i,
                                                const int point_i,
                                                const math::Quaternion &offset)
{
  math::Quaternion &rotation = geometry_refs_[geo_i].rotations[point_i];
  rotation = apply_rotation_offset(rotation, float4(offset));
}

inline Span<int> ConstraintSet::get_affected_geo_indices() const
{
  return affected_geo_indices_;
}

inline ConstraintSetParams::ConstraintSetParams(Span<GeometryRef> geometry_refs)
    : geometry_refs_(geometry_refs)
{
}

inline const float3 &ConstraintSetParams::position(const int geo_i, const int point_i) const
{
  return geometry_refs_[geo_i].positions[point_i];
}

inline const math::Quaternion &ConstraintSetParams::rotation(const int geo_i,
                                                             const int point_i) const
{
  return geometry_refs_[geo_i].rotations[point_i];
}

inline Span<float3> ConstraintSetParams::positions(const int geo_i) const
{
  return geometry_refs_[geo_i].positions;
}

inline Span<math::Quaternion> ConstraintSetParams::rotations(const int geo_i) const
{
  return geometry_refs_[geo_i].rotations;
}

inline float ConstraintSetParams::inverse_mass(const int geo_i, const int point_i) const
{
  return geometry_refs_[geo_i].inverse_masses[point_i];
}

inline Span<float> ConstraintSetParams::inverse_masses(const int geo_i) const
{
  return geometry_refs_[geo_i].inverse_masses;
}

inline float3 ConstraintSetParams::inertia(const int geo_i, const int point_i) const
{
  return geometry_refs_[geo_i].inertias[point_i];
}

inline Span<float3> ConstraintSetParams::inertias(const int geo_i) const
{
  return geometry_refs_[geo_i].inertias;
}

inline float3 ConstraintSetParams::inverse_inertia(const int geo_i, const int point_i) const
{
  return geometry_refs_[geo_i].inverse_inertias[point_i];
}

inline Span<float3> ConstraintSetParams::inverse_inertias(const int geo_i) const
{
  return geometry_refs_[geo_i].inverse_inertias;
}

/** \} */

}  // namespace blender::xpbd
