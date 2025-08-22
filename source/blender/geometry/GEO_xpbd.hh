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
 * Mutable reference to the data that is actually being simulated.
 */
struct PointsRef {
  MutableSpan<float3> positions;
  MutableSpan<math::Quaternion> rotations;
  Span<float> inverse_masses;
  Span<float3> inertias;
  Span<float3> inverse_inertias;

  uint64_t size() const;
};

class ConstraintSetParams {
 private:
  Span<PointsRef> points_refs_;

 public:
  ConstraintSetParams(Span<PointsRef> points_refs);

  const float3 &position(int points_ref_i, int point_i) const;
  const math::Quaternion &rotation(int points_ref_i, int point_i) const;

  Span<float3> positions(int points_ref_i) const;
  Span<math::Quaternion> rotations(int points_ref_i) const;

  float inverse_mass(int points_ref_i, int point_i) const;
  Span<float> inverse_masses(int points_ref_i) const;

  float3 inertia(int points_ref_i, int point_i) const;
  Span<float3> inertias(int points_ref_i) const;

  float3 inverse_inertia(int points_ref_i, int point_i) const;
  Span<float3> inverse_inertias(int points_ref_i) const;
};

/**
 * Updater that writes the changes directly to the simulated points.
 */
class GaussSeidelUpdater {
 private:
  Span<PointsRef> points_refs_;

 public:
  GaussSeidelUpdater(Span<PointsRef> point_sets);
  void update_position(const int points_ref_i, const int point_i, const float3 &offset);
  void update_rotation(const int points_ref_i, const int point_i, const math::Quaternion &offset);
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
  void update_position(const int points_ref_i, const int point_i, const float3 &offset);
  void update_rotation(const int points_ref_i, const int point_i, const math::Quaternion &offset);
};

/**
 * Base class for constraint evaluators. It evaluates batches of constraints and writes back the
 * results using a passed in "updater".
 *
 * Use #TemplatedConstraintSet to instantiate the constraint evaluation for each updater
 * automatically. This avoids having to implement separate Jacobian and Gauss Seidel code paths for
 * such constraints.
 */
class ConstraintSet {
 protected:
  Vector<int> affected_points_refs_;

 public:
  ConstraintSet(Vector<int> affected_points_refs);

  virtual ~ConstraintSet() = default;

  virtual void evaluate_parallel_non_deterministic_jacobian(
      NonDeterministicJacobianUpdater &updater, ConstraintSetParams &params) const = 0;
  virtual void evaluate_parallel_gauss_seidel(GaussSeidelUpdater &updater,
                                              ConstraintSetParams &params) const = 0;
  virtual void evaluate_serial_gauss_seidel(GaussSeidelUpdater &updater,
                                            ConstraintSetParams &params) const = 0;

  Span<int> get_affected_points_refs() const;
};

/**
 * Slow but simple iterative Gauss Seidel solver. It evaluates each constraints serially without
 * any parallelism.
 */
void solve_gauss_seidel_one_at_a_time(Span<PointsRef> points_refs,
                                      Span<const ConstraintSet *> constraint_sets);

/**
 * Fully parallel Jacobian solver, but it is not deterministic. This is mainly for testing
 * purposes.
 */
void solve_jacobian_non_deterministic(Span<PointsRef> points_refs,
                                      Span<const ConstraintSet *> constraint_sets);

/**
 * A Gauss Seidel solver that attempts to parallelize the evaluation of constraints.
 */
void solve_gauss_seidel_parallel(Span<PointsRef> points_refs,
                                 Span<const ConstraintSet *> constraint_sets);

/* -------------------------------------------------------------------- */
/** \name Inline Functions
 * \{ */

inline uint64_t PointsRef::size() const
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

inline void NonDeterministicJacobianUpdater::update_position(const int points_ref_i,
                                                             const int point_i,
                                                             const float3 &offset)
{
  Item &item = offsets_[points_ref_i][point_i];
  std::lock_guard lock(item.linear_mutex);
  item.linear_counter++;
  item.linear_offset += offset;
}

inline void NonDeterministicJacobianUpdater::update_rotation(const int points_ref_i,
                                                             const int point_i,
                                                             const math::Quaternion &offset)
{
  Item &item = offsets_[points_ref_i][point_i];
  std::lock_guard lock(item.rotation_mutex);
  item.rotation_counter++;
  item.rotation_offset += float4(offset);
}

inline GaussSeidelUpdater::GaussSeidelUpdater(Span<PointsRef> point_sets)
    : points_refs_(point_sets)
{
}

inline void GaussSeidelUpdater::update_position(const int points_ref_i,
                                                const int point_i,
                                                const float3 &offset)
{
  points_refs_[points_ref_i].positions[point_i] += offset;
}

inline void GaussSeidelUpdater::update_rotation(const int points_ref_i,
                                                const int point_i,
                                                const math::Quaternion &offset)
{
  math::Quaternion &rotation = points_refs_[points_ref_i].rotations[point_i];
  rotation = apply_rotation_offset(rotation, float4(offset));
}

inline Span<int> ConstraintSet::get_affected_points_refs() const
{
  return affected_points_refs_;
}

inline ConstraintSetParams::ConstraintSetParams(Span<PointsRef> points_refs)
    : points_refs_(points_refs)
{
}

inline const float3 &ConstraintSetParams::position(const int points_ref_i, const int point_i) const
{
  return points_refs_[points_ref_i].positions[point_i];
}

inline const math::Quaternion &ConstraintSetParams::rotation(const int points_ref_i,
                                                             const int point_i) const
{
  return points_refs_[points_ref_i].rotations[point_i];
}

inline Span<float3> ConstraintSetParams::positions(int points_ref_i) const
{
  return points_refs_[points_ref_i].positions;
}

inline Span<math::Quaternion> ConstraintSetParams::rotations(int points_ref_i) const
{
  return points_refs_[points_ref_i].rotations;
}

inline float ConstraintSetParams::inverse_mass(int points_ref_i, int point_i) const
{
  return points_refs_[points_ref_i].inverse_masses[point_i];
}

inline Span<float> ConstraintSetParams::inverse_masses(int points_ref_i) const
{
  return points_refs_[points_ref_i].inverse_masses;
}

inline float3 ConstraintSetParams::inertia(int points_ref_i, int point_i) const
{
  return points_refs_[points_ref_i].inertias[point_i];
}

inline Span<float3> ConstraintSetParams::inertias(int points_ref_i) const
{
  return points_refs_[points_ref_i].inertias;
}

inline float3 ConstraintSetParams::inverse_inertia(int points_ref_i, int point_i) const
{
  return points_refs_[points_ref_i].inverse_inertias[point_i];
}

inline Span<float3> ConstraintSetParams::inverse_inertias(int points_ref_i) const
{
  return points_refs_[points_ref_i].inverse_inertias;
}

/** \} */

}  // namespace blender::xpbd
