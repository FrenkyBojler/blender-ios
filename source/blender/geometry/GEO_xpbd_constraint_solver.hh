/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_cache_mutex.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_multi_value_map.hh"
#include "BLI_mutex.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::geometry::xpbd_constraint_solver {

/**
 * Mutable reference to the data that is actually being simulated.
 */
struct MutablePointsRef {
  MutableSpan<float3> positions;
  MutableSpan<math::Quaternion> rotations;

  uint64_t size() const;
};

struct PointsRef {
  Span<float3> positions;
  Span<math::Quaternion> rotations;

  PointsRef(const MutablePointsRef &other);
};

/**
 * Updater that writes the changes directly to the simulated points.
 */
class GaussSeidelUpdater {
 private:
  Span<MutablePointsRef> points_refs_;

 public:
  GaussSeidelUpdater(Span<MutablePointsRef> point_sets);
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
      NonDeterministicJacobianUpdater &updater, Span<PointsRef> points_refs) const = 0;
  virtual void evaluate_parallel_gauss_seidel(GaussSeidelUpdater &updater,
                                              Span<PointsRef> points_refs) const = 0;
  virtual void evaluate_serial_gauss_seidel(GaussSeidelUpdater &updater,
                                            Span<PointsRef> points_refs) const = 0;

  Span<int> get_affected_points_refs() const;
};

/**
 * Utility to implement a constraint evaluator that automatically works with multiple updaters like
 * #GaussSeidelUpdater.
 *
 * Child classes have to implement the templated #evaluate_single method.
 */
template<typename Child> class TemplatedConstraintSet : public ConstraintSet {
 protected:
  const int constraints_num_;
  const int grain_size_ = 256;

 private:
  mutable CacheMutex independent_masks_mutex_;
  mutable IndexMaskMemory independent_masks_memory_;
  mutable Vector<IndexMask> independent_masks_;

 public:
  TemplatedConstraintSet(int constraints_num, Vector<int> affected_points_refs);

  void evaluate_parallel_non_deterministic_jacobian(NonDeterministicJacobianUpdater &updater,
                                                    Span<PointsRef> points_refs) const override;
  void evaluate_parallel_gauss_seidel(GaussSeidelUpdater &updater,
                                      Span<PointsRef> points_refs) const override;
  void evaluate_serial_gauss_seidel(GaussSeidelUpdater &updater,
                                    Span<PointsRef> points_refs) const override;

  Span<IndexMask> get_independent_masks() const;

  virtual Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const = 0;

  /**
   * Evaluate a single constraint using the given updater. This has to be implemented on child
   * classes.
   */
  // template<typename UpdaterT>
  // void evaluate_single(
  //   UpdaterT &updater, const Span<PointsRef> points_refs, const int constraint_i) const;
};

Vector<IndexMask> unary_constraints_to_independent_masks(const Span<int> affected_points,
                                                         IndexMaskMemory &memory);
Vector<IndexMask> binary_constraints_to_independent_masks(const Span<int2> affected_points,
                                                          IndexMaskMemory &memory);
Vector<IndexMask> n_ary_constraints_to_independent_masks(const GroupedSpan<int> affected_points,
                                                         IndexMaskMemory &memory);
Vector<IndexMask> n_ary_constraints_to_independent_masks_multi(
    const GroupedSpan<int> affected_points,
    const GroupedSpan<int> effected_points_refs,
    IndexMaskMemory &memory);

/**
 * Slow but simple iterative Gauss Seidel solver. It evaluates each constraints serially without
 * any parallelism.
 */
void solve_gauss_seidel_one_at_a_time(Span<MutablePointsRef> points_refs,
                                      Span<const ConstraintSet *> constraint_sets);

/**
 * Fully parallel Jacobian solver, but it is not deterministic. This is mainly for testing
 * purposes.
 */
void solve_jacobian_non_deterministic(Span<MutablePointsRef> points_refs,
                                      Span<const ConstraintSet *> constraint_sets);

/**
 * A Gauss Seidel solver that attempts to parallelize the evaluation of constraints.
 */
void solve_gauss_seidel_parallel(Span<MutablePointsRef> points_refs,
                                 Span<const ConstraintSet *> constraint_sets);

/* -------------------------------------------------------------------- */
/** \name Inline Functions
 * \{ */

inline PointsRef::PointsRef(const MutablePointsRef &other)
    : positions(other.positions), rotations(other.rotations)
{
}

inline uint64_t MutablePointsRef::size() const
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

inline GaussSeidelUpdater::GaussSeidelUpdater(Span<MutablePointsRef> point_sets)
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

template<typename PointID, typename GetConstraintPointsFn>
inline int color_constraints(GetConstraintPointsFn &&get_constraint_points_fn,
                             MutableSpan<int> r_colors)
{
  const int constraints_num = r_colors.size();
  MultiValueMap<PointID, int> constraints_by_point;
  for (const int constraint_i : IndexRange(constraints_num)) {
    for (const PointID point_id : get_constraint_points_fn(constraint_i)) {
      constraints_by_point.add(point_id, constraint_i);
    }
  }
  int colors_num = 0;
  for (const int constraint_i : IndexRange(constraints_num)) {
    Vector<int> used_colors;
    for (const PointID point_id : get_constraint_points_fn(constraint_i)) {
      for (const int other_constraint_i : constraints_by_point.lookup(point_id)) {
        if (other_constraint_i >= constraint_i) {
          continue;
        }
        used_colors.append_non_duplicates(r_colors[other_constraint_i]);
      }
    }
    int best_color = 0;
    while (used_colors.contains(best_color)) {
      best_color++;
    }
    r_colors[constraint_i] = best_color;
    colors_num = std::max(colors_num, best_color + 1);
  }
  return colors_num;
}

template<typename PointID, typename GetConstraintPointsFn>
inline Vector<IndexMask> detect_independent_constraints(
    GetConstraintPointsFn &&get_constraint_points_fn,
    const int constraints_num,
    IndexMaskMemory &memory)
{
  if (constraints_num == 0) {
    return {};
  }
  Array<int> colors(constraints_num);
  const int colors_num = color_constraints<PointID>(get_constraint_points_fn, colors);
  Array<Vector<int>> masks_indices(colors_num);
  for (const int constraint_i : IndexRange(constraints_num)) {
    masks_indices[colors[constraint_i]].append(constraint_i);
  }
  Vector<IndexMask> masks;
  for (const int color_i : IndexRange(colors_num)) {
    const IndexMask mask = IndexMask::from_indices<int>(masks_indices[color_i], memory);
    masks.append(mask);
  }
  return masks;
}

inline Span<int> ConstraintSet::get_affected_points_refs() const
{
  return affected_points_refs_;
}

template<typename Child>
inline Span<IndexMask> TemplatedConstraintSet<Child>::get_independent_masks() const
{
  independent_masks_mutex_.ensure(
      [&]() { independent_masks_ = this->generate_independent_masks(independent_masks_memory_); });
  return independent_masks_;
}

template<typename Child>
inline TemplatedConstraintSet<Child>::TemplatedConstraintSet(int constraints_num,
                                                             Vector<int> affected_points_refs)
    : ConstraintSet(std::move(affected_points_refs)), constraints_num_(constraints_num)
{
}

template<typename Child>
inline void TemplatedConstraintSet<Child>::evaluate_parallel_non_deterministic_jacobian(
    NonDeterministicJacobianUpdater &updater, const Span<PointsRef> points_refs) const
{
  const Child &self = static_cast<const Child &>(*this);
  threading::parallel_for(IndexRange(constraints_num_), grain_size_, [&](const IndexRange range) {
    for (const int constraint_i : range) {
      self.evaluate_single(updater, points_refs, constraint_i);
    }
  });
}

template<typename Child>
inline void TemplatedConstraintSet<Child>::evaluate_parallel_gauss_seidel(
    GaussSeidelUpdater &updater, const Span<PointsRef> points_refs) const
{
  const Child &self = static_cast<const Child &>(*this);
  const Span<IndexMask> constraint_masks = this->get_independent_masks();
  for (const int color_i : constraint_masks.index_range()) {
    const IndexMask &constraint_mask = constraint_masks[color_i];
    constraint_mask.foreach_index(GrainSize(grain_size_), [&](const int constraint_i) {
      self.evaluate_single(updater, points_refs, constraint_i);
    });
  }
}

template<typename Child>
inline void TemplatedConstraintSet<Child>::evaluate_serial_gauss_seidel(
    GaussSeidelUpdater &updater, const Span<PointsRef> points_refs) const
{
  const Child &self = static_cast<const Child &>(*this);
  for (const int constraint_i : IndexRange(constraints_num_)) {
    self.evaluate_single(updater, points_refs, constraint_i);
  }
}

/** \} */

}  // namespace blender::geometry::xpbd_constraint_solver
