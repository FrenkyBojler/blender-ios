/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "GEO_xpbd_constraint_set.hh"

namespace blender::xpbd {

/**
 * Utility to implement a constraint evaluator that automatically works with multiple updaters like
 * #GaussSeidelUpdater.
 *
 * Child classes have to implement the templated #solve_single method.
 */
template<typename Child> class TemplatedConstraintSet : public ConstraintSet {
 public:
  TemplatedConstraintSet(const int constraints_num, Vector<int> affected_geo_indices)
      : ConstraintSet(constraints_num, std::move(affected_geo_indices))
  {
    supports_warm_start_ = requires { Child::template warm_start<GaussSeidelUpdater>; };
  }

  void reset_forces(const IndexMask &mask) override
  {
    const Child &self = static_cast<const Child &>(*this);
    mask.foreach_index([&](const int64_t constraint_i) { self.reset_force(constraint_i); });
  }

  void solve_sequential(const ConstraintSetParams &params,
                        GaussSeidelUpdater &updater,
                        const IndexMask &mask) override
  {
    const Child &self = static_cast<const Child &>(*this);
    mask.foreach_index(
        [&](const int64_t constraint_i) { self.solve_single(params, updater, constraint_i); });
  }

  void warm_start_sequential(const ConstraintSetParams &params,
                             GaussSeidelUpdater &updater,
                             const IndexMask &mask) override
  {
    if constexpr (requires { Child::template warm_start<GaussSeidelUpdater>; }) {
      const Child &self = static_cast<const Child &>(*this);
      mask.foreach_index(
          [&](const int64_t constraint_i) { self.warm_start(params, updater, constraint_i); });
    }
    else {
      this->reset_forces(mask);
    }
  }

  StringRefNull debug_name() const final
  {
    return Child::debug_name;
  }
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

  void solve_sequential(const ConstraintSetParams &params, VelocityUpdater &updater) override
  {
    Child &self = static_cast<Child &>(*this);
    for (const int constraint_i : IndexRange(constraint_num_)) {
      self.solve_single(params, updater, constraint_i);
    }
  }

  StringRef debug_name() const
  {
    return Child::debug_name;
  }
};

}  // namespace blender::xpbd
