/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>
#include <variant>

#include "BLI_array.hh"
#include "BLI_function_ref.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_mutex.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "BKE_geometry_set.hh"

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
  Span<float3> inertias;
  Span<float3> inverse_inertias;

  uint64_t size() const;
};

using ConstraintGeometryFn = FunctionRef<bke::GeometrySet(int points_ref_index)>;
using SolverDebugStageFn = FunctionRef<void(
    StringRef name, Span<int> points_ref_indices, bke::GeometrySet &&constraint_geometry)>;

/** Provides access to the input data that should be considered by a constraint. */
class ConstraintSetParams {
 private:
  Span<GeometryRef> geometry_refs_;
  std::optional<SolverDebugStageFn> debug_stage_fn_;

 public:
  float delta_time;
  float compliance_term_factor;

  ConstraintSetParams(Span<GeometryRef> geometry_refs,
                      float delta_time,
                      std::optional<SolverDebugStageFn> debug_stage_fn);

  Span<GeometryRef> geometry_refs() const;

  const float3 &position(int geo_i, int point_i) const;
  const math::Quaternion &rotation(int geo_i, int point_i) const;
  const float3 &prev_position(int geo_i, int point_i) const;
  const math::Quaternion &prev_rotation(int geo_i, int point_i) const;
  const float3 &velocity(int geo_i, int point_i) const;
  const float3 &angular_velocity(int geo_i, int point_i) const;

  Span<float3> positions(int geo_i) const;
  Span<math::Quaternion> rotations(int geo_i) const;
  Span<float3> prev_positions(int geo_i) const;
  Span<math::Quaternion> prev_rotations(int geo_i) const;
  Span<float3> velocities(int geo_i) const;
  Span<float3> angular_velocities(int geo_i) const;

  float inverse_mass(int geo_i, int point_i) const;
  Span<float> inverse_masses(int geo_i) const;

  float3 inertia(int geo_i, int point_i) const;
  Span<float3> inertias(int geo_i) const;

  float3 inverse_inertia(int geo_i, int point_i) const;
  Span<float3> inverse_inertias(int geo_i) const;

  bool use_debug() const;
  void debug_stage(StringRef constraint_name,
                   Span<int> points_ref_indices,
                   bke::GeometrySet &&constraint_geometry) const;
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
  struct OffsetItem {
    Mutex linear_mutex;
    int linear_counter = 0;
    float3 linear_offset = float3(0.0f);
    Mutex rotation_mutex;
    int rotation_counter = 0;
    float4 rotation_offset = float4(0.0f);
  };

  struct GeometryItem {
    Array<OffsetItem> offsets;
    IndexRange range;
  };

 private:
  Span<GeometryRef> geometry_refs_;
  Array<GeometryItem> items_;

 public:
  NonDeterministicJacobianUpdater(Span<GeometryRef> geometry_refs);
  NonDeterministicJacobianUpdater(Span<GeometryRef> geometry_refs,
                                  const int geo_i,
                                  const IndexRange range);
  void update_position(const int geo_i, const int point_i, const float3 &offset);
  void update_rotation(const int geo_i, const int point_i, const math::Quaternion &offset);

  void apply();
};

using UpdaterVariant = std::variant<GaussSeidelUpdater, NonDeterministicJacobianUpdater>;

enum class SolveStrategyType {
  GaussSeidelOneAtATime,
  GaussSeidelParallel,
  JacobianNonDeterministic,
};

class SolveStrategy {
 private:
  std::optional<UpdaterVariant> updater_;

 public:
  SolveStrategyType type;

  SolveStrategy(SolveStrategyType type, Span<GeometryRef> geometry_refs);
  SolveStrategy(SolveStrategyType type,
                Span<GeometryRef> geometry_refs,
                const int geo_i,
                const IndexRange range);

  UpdaterVariant &updater();

  void apply();
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

  virtual void reset_forces() = 0;
  virtual void solve_step(SolveStrategy &method, const ConstraintSetParams &params) = 0;
  virtual StringRef debug_name() const = 0;

  Span<int> get_affected_geo_indices() const;
};

/**
 * Updater that writes the changes directly to the simulated points.
 */
class VelocityUpdater {
 private:
  Span<GeometryRef> geometry_refs_;

 public:
  VelocityUpdater(Span<GeometryRef> geometry_refs);
  void update_velocity(const int geo_i, const int point_i, const float3 &offset);
  void update_angular_velocity(const int geo_i, const int point_i, const float3 &offset);
};

class VelocityConstraintSet {
 protected:
  Vector<int> affected_geo_indices_;

 public:
  VelocityConstraintSet(Vector<int> affected_geo_indices)
      : affected_geo_indices_(std::move(affected_geo_indices))
  {
  }
  virtual ~VelocityConstraintSet() = default;

  virtual void reset_forces() = 0;
  virtual void solve_step(VelocityUpdater &updater, const ConstraintSetParams &params) = 0;

  Span<int> get_affected_geo_indices() const;
};

/**
 * Slow but simple iterative Gauss Seidel solver. It evaluates each constraints serially without
 * any parallelism.
 */
void solve_gauss_seidel_one_at_a_time(ConstraintSetParams &params,
                                      Span<ConstraintSet *> constraint_sets);

/**
 * Fully parallel Jacobian solver, but it is not deterministic. This is mainly for testing
 * purposes.
 */
void solve_jacobian_non_deterministic(ConstraintSetParams &params,
                                      Span<ConstraintSet *> constraint_sets);

/**
 * A Gauss Seidel solver that attempts to parallelize the evaluation of constraints.
 */
void solve_gauss_seidel_parallel(ConstraintSetParams &params,
                                 Span<ConstraintSet *> constraint_sets);

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

inline void NonDeterministicJacobianUpdater::update_position(const int geo_i,
                                                             const int point_i,
                                                             const float3 &offset)
{
  GeometryItem &geo_item = items_[geo_i];
  OffsetItem &item = geo_item.offsets[point_i - geo_item.range.start()];
  std::lock_guard lock(item.linear_mutex);
  item.linear_counter++;
  item.linear_offset += offset;
}

inline void NonDeterministicJacobianUpdater::update_rotation(const int geo_i,
                                                             const int point_i,
                                                             const math::Quaternion &offset)
{
  GeometryItem &geo_item = items_[geo_i];
  OffsetItem &item = geo_item.offsets[point_i - geo_item.range.start()];
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

inline VelocityUpdater::VelocityUpdater(Span<GeometryRef> geometry_refs)
    : geometry_refs_(geometry_refs)
{
}

inline void VelocityUpdater::update_velocity(const int geo_i,
                                             const int point_i,
                                             const float3 &offset)
{
  geometry_refs_[geo_i].velocities[point_i] += offset;
}

inline void VelocityUpdater::update_angular_velocity(const int geo_i,
                                                     const int point_i,
                                                     const float3 &offset)
{
  geometry_refs_[geo_i].angular_velocities[point_i] += offset;
}

inline UpdaterVariant &SolveStrategy::updater()
{
  return *updater_;
}

inline Span<int> ConstraintSet::get_affected_geo_indices() const
{
  return affected_geo_indices_;
}

inline ConstraintSetParams::ConstraintSetParams(Span<GeometryRef> geometry_refs,
                                                const float delta_time,
                                                std::optional<SolverDebugStageFn> debug_stage_fn)
    : geometry_refs_(geometry_refs),
      debug_stage_fn_(debug_stage_fn),
      delta_time(delta_time),
      compliance_term_factor(math::safe_rcp(delta_time * delta_time))
{
}

inline Span<GeometryRef> ConstraintSetParams::geometry_refs() const
{
  return geometry_refs_;
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

inline const float3 &ConstraintSetParams::prev_position(const int geo_i, const int point_i) const
{
  return geometry_refs_[geo_i].prev_positions[point_i];
}

inline const math::Quaternion &ConstraintSetParams::prev_rotation(const int geo_i,
                                                                  const int point_i) const
{
  return geometry_refs_[geo_i].prev_rotations[point_i];
}

inline const float3 &ConstraintSetParams::velocity(const int geo_i, const int point_i) const
{
  return geometry_refs_[geo_i].velocities[point_i];
}

inline const float3 &ConstraintSetParams::angular_velocity(const int geo_i,
                                                           const int point_i) const
{
  return geometry_refs_[geo_i].angular_velocities[point_i];
}

inline Span<float3> ConstraintSetParams::positions(const int geo_i) const
{
  return geometry_refs_[geo_i].positions;
}

inline Span<math::Quaternion> ConstraintSetParams::rotations(const int geo_i) const
{
  return geometry_refs_[geo_i].rotations;
}

inline Span<float3> ConstraintSetParams::prev_positions(const int geo_i) const
{
  return geometry_refs_[geo_i].prev_positions;
}

inline Span<math::Quaternion> ConstraintSetParams::prev_rotations(const int geo_i) const
{
  return geometry_refs_[geo_i].prev_rotations;
}

inline Span<float3> ConstraintSetParams::velocities(const int geo_i) const
{
  return geometry_refs_[geo_i].velocities;
}

inline Span<float3> ConstraintSetParams::angular_velocities(const int geo_i) const
{
  return geometry_refs_[geo_i].angular_velocities;
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

inline bool ConstraintSetParams::use_debug() const
{
  return debug_stage_fn_.has_value();
}

inline void ConstraintSetParams::debug_stage(StringRef constraint_name,
                                             const Span<int> points_ref_indices,
                                             bke::GeometrySet &&constraint_geometry) const
{
  if (debug_stage_fn_) {
    (*debug_stage_fn_)(constraint_name, points_ref_indices, std::move(constraint_geometry));
  }
}

/** \} */

}  // namespace blender::xpbd
