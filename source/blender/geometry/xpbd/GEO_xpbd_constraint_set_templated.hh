/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_cache_mutex.hh"
#include "BLI_index_mask.hh"

#include "GEO_xpbd_constraint_set.hh"

namespace blender::xpbd {

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
  TemplatedConstraintSet(int constraints_num, Vector<int> affected_geo_indices)
      : ConstraintSet(std::move(affected_geo_indices)), constraints_num_(constraints_num)
  {
  }

  void reset_forces() override
  {
    const Child &self = static_cast<const Child &>(*this);
    for (const int constraint_i : IndexRange(constraints_num_)) {
      self.reset_force(constraint_i);
    }
  }

  void solve_step(SolveStrategy &strategy, const ConstraintSetParams &params) override
  {
    const Child &self = static_cast<const Child &>(*this);

    switch (strategy.type) {
      case SolveStrategyType::GaussSeidelOneAtATime: {
        auto &updater = std::get<GaussSeidelUpdater>(strategy.updater());
        for (const int constraint_i : IndexRange(constraints_num_)) {
          self.evaluate_single(updater, params, constraint_i);
        }
        break;
      }
      case SolveStrategyType::GaussSeidelParallel: {
        auto &updater = std::get<GaussSeidelUpdater>(strategy.updater());
        const Span<IndexMask> constraint_masks = this->get_independent_masks();
        for (const int color_i : constraint_masks.index_range()) {
          const IndexMask &constraint_mask = constraint_masks[color_i];
          constraint_mask.foreach_index(GrainSize(grain_size_), [&](const int constraint_i) {
            self.evaluate_single(updater, params, constraint_i);
          });
        }
        break;
      }
      case SolveStrategyType::JacobianNonDeterministic: {
        auto &updater = std::get<NonDeterministicJacobianUpdater>(strategy.updater());
        threading::parallel_for(
            IndexRange(constraints_num_), grain_size_, [&](const IndexRange range) {
              for (const int constraint_i : range) {
                self.evaluate_single(updater, params, constraint_i);
              }
            });
        break;
      }
    }
  }

  StringRefNull debug_name() const final
  {
    return Child::debug_name;
  }

  Span<IndexMask> get_independent_masks() const
  {
    independent_masks_mutex_.ensure([&]() {
      independent_masks_ = this->generate_independent_masks(independent_masks_memory_);
    });
    return independent_masks_;
  }

  virtual Vector<IndexMask> generate_independent_masks(IndexMaskMemory &memory) const = 0;
};

template<typename Child> class TemplatedVelocityConstraintSet : public VelocityConstraintSet {
 protected:
  const int constraint_num_;

 public:
  TemplatedVelocityConstraintSet(int constraints_num, Vector<int> affected_geo_indices)
      : VelocityConstraintSet(std::move(affected_geo_indices)), constraint_num_(constraints_num)
  {
  }

  void reset_forces() override
  {
    Child &self = static_cast<Child &>(*this);
    for (const int constraint_i : IndexRange(constraint_num_)) {
      self.reset_force(constraint_i);
    }
  }

  void solve_step(VelocityUpdater &updater, const ConstraintSetParams &params) override
  {
    Child &self = static_cast<Child &>(*this);
    for (const int constraint_i : IndexRange(constraint_num_)) {
      self.evaluate_single(updater, params, constraint_i);
    }
  }

  StringRef debug_name() const
  {
    return Child::debug_name;
  }
};

}  // namespace blender::xpbd
