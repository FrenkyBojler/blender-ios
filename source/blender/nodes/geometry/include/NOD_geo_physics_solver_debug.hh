/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_geometry_set.hh"
#include "BKE_instances.hh"

#include "NOD_bundle_type_fwd.hh"
#include "NOD_geometry_nodes_bundle_fwd.hh"
#include "NOD_geometry_nodes_bundle_parse.hh"

namespace blender::nodes::physics_solver_debug {

class SolverStageBundle {
 public:
  static constexpr StringRefNull name = "Blender.PhysicsSolverDebugStage";

  bke::GeometrySet geometry;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<SolverStageBundle> parse(const Bundle &bundle, BundleParseErrors &r_errors);
  BundlePtr store() const;
};

class ConstraintIterationBundle {
 public:
  static constexpr StringRefNull name = "Blender.PhysicsSolverDebugConstraintIteration";

  bke::GeometrySet stages;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<ConstraintIterationBundle> parse(const Bundle &bundle,
                                                        BundleParseErrors &r_errors);
  BundlePtr store() const;
};

class SubstepBundle {
 public:
  static constexpr StringRefNull name = "Blender.PhysicsSolverDebugSubstep";

  SolverStageBundle init_stage;
  SolverStageBundle dynamics_stage;
  Vector<ConstraintIterationBundle> constraint_iterations;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<SubstepBundle> parse(const Bundle &bundle, BundleParseErrors &r_errors);
  BundlePtr store() const;
};

class DebugBundle {
 public:
  static constexpr StringRefNull name = "Blender.PhysicsSolverDebug";

  Vector<SubstepBundle> substeps;

  static const FlatBundleTypePtr &get_bundle_type();
  static std::optional<DebugBundle> parse(const Bundle &bundle, BundleParseErrors &r_errors);
  BundlePtr store() const;
};

enum class Stage {
  Init,
  Dynamics,
};

/* Temporary debug state to collect stages sequentially. */
class XPBDDebugRecorder {
 private:
  const int total_steps_;
  Map<std::string, DebugBundle> bundles_by_path_;
  std::mutex mutex_;

 public:
  XPBDDebugRecorder(int total_steps) : total_steps_(total_steps) {}

  const Map<std::string, DebugBundle> &steps() const
  {
    return bundles_by_path_;
  }

  bool has_paths() const
  {
    return !bundles_by_path_.is_empty();
  }

  void add_geometry_path(const StringRef path)
  {
    bundles_by_path_.add_new(path, DebugBundle{});
  }

  static StringRef stage_name(const Stage stage)
  {
    switch (stage) {
      case Stage::Init:
        return "Init";
      case Stage::Dynamics:
        return "Dynamics";
    }
    BLI_assert_unreachable();
    return "";
  }

  void start_substep(const StringRef path)
  {
    std::scoped_lock lock(mutex_);
    DebugBundle &step_bundle = this->get_step_bundle(path);
    step_bundle.substeps.append({});
  }

  void add_stage(const StringRef path, const Stage stage, bke::GeometrySet &&geometry)
  {
    geometry.ensure_owns_direct_data();

    std::scoped_lock lock(mutex_);
    BLI_assert(bundles_by_path_.contains(path));
    SolverStageBundle &stage_bundle = get_stage(this->current_step(path), stage);
    geometry.name = stage_name(stage);
    stage_bundle.geometry = std::move(geometry);
  }

  void start_constraint_iteration(const StringRef path)
  {
    std::scoped_lock lock(mutex_);
    SubstepBundle &step_bundle = this->current_step(path);
    step_bundle.constraint_iterations.append({});
  }

  void add_constraint_stage(bke::GeometrySet &&geometry,
                            const FunctionRef<bool(StringRef)> &paths_filter)
  {
    if (geometry.is_empty()) {
      return;
    }
    geometry.ensure_owns_direct_data();

    std::scoped_lock lock(mutex_);
    for (const auto &bundles_item : bundles_by_path_.items()) {
      if (paths_filter(bundles_item.key)) {
        ConstraintIterationBundle &iter_bundle =
            bundles_item.value.substeps.last().constraint_iterations.last();
        bke::Instances *instances = iter_bundle.stages.get_instances_for_write();
        if (instances) {
          instances->resize(instances->instances_num() + 1);
        }
        else {
          instances = new bke::Instances(1);
          iter_bundle.stages.replace_instances(instances);
        }
        const int handle = instances->add_new_reference({geometry});
        instances->reference_handles_for_write().last() = handle;
        instances->transforms_for_write().last() = float4x4::identity();
      }
    }
  }

 private:
  DebugBundle &get_step_bundle(const StringRef path)
  {
    return bundles_by_path_.lookup(path);
  }

  SubstepBundle &current_step(const StringRef path)
  {
    DebugBundle &step_bundle = this->get_step_bundle(path);
    BLI_assert(!step_bundle.substeps.is_empty());
    return step_bundle.substeps.last();
  }

  ConstraintIterationBundle &current_constraint_iteration(const StringRef path)
  {
    SubstepBundle &substep_bundle = this->current_step(path);
    /* Constraint iterations should be added using #start_constraint_iteration. */
    BLI_assert(!substep_bundle.constraint_iterations.is_empty());
    return substep_bundle.constraint_iterations.last();
  }

  static SolverStageBundle &get_stage(SubstepBundle &step_bundle, const Stage stage)
  {
    switch (stage) {
      case Stage::Init:
        return step_bundle.init_stage;
      case Stage::Dynamics:
        return step_bundle.dynamics_stage;
    }
    BLI_assert_unreachable();
    return step_bundle.init_stage;
  }
};

}  // namespace blender::nodes::physics_solver_debug
