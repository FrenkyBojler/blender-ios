/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>
#include <variant>

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "GEO_xpbd_constraint_set_params.hh"
#include "GEO_xpbd_geometry_ref.hh"
#include "GEO_xpbd_updater_gauss_seidel.hh"
#include "GEO_xpbd_updater_non_deterministic_jacobian.hh"
#include "GEO_xpbd_updater_velocity.hh"

namespace blender::xpbd {

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

  UpdaterVariant &updater()
  {
    return *updater_;
  }

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
  virtual StringRefNull debug_name() const = 0;

  Span<int> get_affected_geo_indices() const
  {
    return affected_geo_indices_;
  }
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

  Span<int> get_affected_geo_indices() const
  {
    return affected_geo_indices_;
  }
};

}  // namespace blender::xpbd
