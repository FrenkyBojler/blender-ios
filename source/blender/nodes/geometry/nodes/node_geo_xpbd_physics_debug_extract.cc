/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geo_physics_solver_debug.hh"
#include "NOD_socket_usage_inference.hh"

#include "GEO_xpbd_constraint_sets_common.hh"

#include "RNA_access.hh"

#include "node_geometry_util.hh"

#define PROFILE_FUNCTION BLI_NOINLINE

namespace blender::nodes::node_geo_xpbd_debug_extract_cc {

using namespace physics_solver_debug;

enum class SubstepMode {
  /* Show a single substep. */
  Single = 0,
  /* Show all substeps. */
  All,
};

static EnumPropertyItem substep_mode_items[] = {
    {int(SubstepMode::Single), "SINGLE", 0, N_("Single Substep"), N_("Show a single step")},
    {int(SubstepMode::All), "ALL", 0, N_("All Substeps"), N_("Show all steps")},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class IterationMode {
  /* Show a single iteration. */
  Single = 0,
  /* Show all iterations. */
  All,
};

static EnumPropertyItem iteration_mode_items[] = {
    {int(IterationMode::Single),
     "SINGLE",
     0,
     N_("Single Iteration"),
     N_("Show a single iteration")},
    {int(IterationMode::All), "ALL", 0, N_("All Iterations"), N_("Show all iterations")},
    {0, nullptr, 0, nullptr, nullptr},
};

template<class ConstraintT>
static constexpr EnumPropertyItem constraint_set_enum_item(const int value)
{
  const char *name = ConstraintT::debug_name.c_str();
  return {value, name, 0, N_(name), N_("")};
}

static const int constraint_stage_start = 1000;
static EnumPropertyItem stage_items[] = {
    {int(physics_solver_debug::Stage::Init), "INIT", 0, N_("Init"), N_("")},
    {int(physics_solver_debug::Stage::Dynamics), "DYNAMICS", 0, N_("Dynamics"), N_("")},
    /* Use values starting at #constraint_stage_start below. */
    constraint_set_enum_item<xpbd::AlignPositionsConstraintSet>(1000),
    constraint_set_enum_item<xpbd::AttachUVSurfaceConstraintSet>(1001),
    constraint_set_enum_item<xpbd::CollisionPlaneConstraintSet>(1002),
    constraint_set_enum_item<xpbd::DistanceConstraintSet>(1003),
    constraint_set_enum_item<xpbd::MinimumDistanceConstraintSet>(1004),
    constraint_set_enum_item<xpbd::PinPositionConstraintSet>(1005),
    constraint_set_enum_item<xpbd::PinRotationConstraintSet>(1006),
    constraint_set_enum_item<xpbd::PressureConstraintSet>(1007),
    constraint_set_enum_item<xpbd::RodBendAndTwistConstraintSet>(1008),
    constraint_set_enum_item<xpbd::RodStretchAndShearConstraintSet>(1009),
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Bundle>("Debug Steps")
      .bundle_type(DebugBundle::get_bundle_type())
      .structure_type(StructureType::Single);
  b.add_input<decl::Menu>("Substep Mode")
      .static_items(substep_mode_items)
      .optional_label()
      .description("Mode of selecting substeps");
  b.add_input<decl::Int>("Substep Index")
      .min(0)
      .description("Index of substep to show")
      .usage_inference([](const socket_usage_inference::SocketUsageParams &params) {
        const std::optional<MenuValue> substep_mode =
            params.get_input("Substep Mode").get_if_primitive<MenuValue>();
        return !substep_mode || substep_mode->value == int(SubstepMode::Single);
      });

  b.add_input<decl::Menu>("Stage")
      .static_items(stage_items)
      .optional_label()
      .description("Solver stage to show");

  b.add_input<decl::Menu>("Iteration Mode")
      .static_items(iteration_mode_items)
      .optional_label()
      .description("Mode of selecting constraint iterations")
      .usage_inference([](const socket_usage_inference::SocketUsageParams &params) {
        const std::optional<MenuValue> stage =
            params.get_input("Stage").get_if_primitive<MenuValue>();
        return !stage || stage->value >= constraint_stage_start;
      });
  b.add_input<decl::Int>("Iteration Index")
      .min(0)
      .description("Constraint iteration to show")
      .usage_inference([](const socket_usage_inference::SocketUsageParams &params) {
        const std::optional<MenuValue> stage =
            params.get_input("Stage").get_if_primitive<MenuValue>();
        if (stage && stage->value < constraint_stage_start) {
          return false;
        }
        const std::optional<MenuValue> iter_mode =
            params.get_input("Iteration Mode").get_if_primitive<MenuValue>();
        return !iter_mode || iter_mode->value == int(IterationMode::Single);
      });

  b.add_output<decl::Geometry>("Geometry");
}

static void gather_substeps_for_stage(const DebugBundle &debug_bundle,
                                      const IndexMask &substeps_mask,
                                      const physics_solver_debug::Stage stage,
                                      bke::Instances &result)
{
  const int old_instances_num = result.instances_num();
  int new_instances_num = 0;
  substeps_mask.foreach_index([&](const int /*substep*/) { ++new_instances_num; });

  result.resize(old_instances_num + new_instances_num);
  MutableSpan<int> handles = result.reference_handles_for_write().slice(old_instances_num,
                                                                        new_instances_num);
  MutableSpan<float4x4> transforms = result.transforms_for_write().slice(old_instances_num,
                                                                         new_instances_num);

  int instance_i = 0;
  substeps_mask.foreach_index([&](const int substep) {
    const SubstepBundle &substep_bundle = debug_bundle.substeps[substep];
    const GeometrySet *geometry = nullptr;
    switch (stage) {
      case physics_solver_debug::Stage::Init: {
        geometry = &substep_bundle.init_stage.geometry;
        break;
      }
      case physics_solver_debug::Stage::Dynamics: {
        geometry = &substep_bundle.dynamics_stage.geometry;
        break;
      }
    }

    if (geometry) {
      const int handle = result.add_new_reference({*geometry});
      handles[instance_i] = handle;
      transforms[instance_i] = float4x4::identity();
    }
    ++instance_i;
  });
}

using IterationMaskFn = FunctionRef<IndexMask(IndexRange)>;

static void gather_substeps_for_constraints(const DebugBundle &debug_bundle,
                                            const IndexMask &substeps_mask,
                                            const StringRef constraint_debug_name,
                                            const IterationMaskFn iteration_mask_fn,
                                            bke::Instances &result)
{
  const int old_instances_num = result.instances_num();
  int new_instances_num = 0;
  substeps_mask.foreach_index([&](const int substep) {
    const SubstepBundle &substep_bundle = debug_bundle.substeps[substep];
    const IndexMask iteration_mask = iteration_mask_fn(
        substep_bundle.constraint_iterations.index_range());

    iteration_mask.foreach_index([&](const int iteration) {
      const ConstraintIterationBundle &iter_bundle =
          substep_bundle.constraint_iterations[iteration];
      /* Filter by constraint name, stored as the instanced geometries' name. */
      if (const bke::Instances *instances = iter_bundle.stages.get_instances()) {
        const Span<bke::InstanceReference> references = instances->references();
        const Span<int> handles = instances->reference_handles();
        for (const int i : handles.index_range()) {
          const int handle = handles[i];
          const GeometrySet &geometry = references[handle].geometry_set();
          if (StringRef(geometry.name).startswith(constraint_debug_name)) {
            ++new_instances_num;
          }
        }
      }
    });
  });

  result.resize(old_instances_num + new_instances_num);
  MutableSpan<int> result_handles = result.reference_handles_for_write().slice(old_instances_num,
                                                                               new_instances_num);
  MutableSpan<float4x4> result_transforms = result.transforms_for_write().slice(old_instances_num,
                                                                                new_instances_num);

  int instance_i = 0;
  substeps_mask.foreach_index([&](const int substep) {
    const SubstepBundle &substep_bundle = debug_bundle.substeps[substep];
    const IndexMask iteration_mask = iteration_mask_fn(
        substep_bundle.constraint_iterations.index_range());

    iteration_mask.foreach_index([&](const int iteration) {
      const ConstraintIterationBundle &iter_bundle =
          substep_bundle.constraint_iterations[iteration];
      /* Filter by constraint name, stored as the instanced geometries' name. */
      if (const bke::Instances *instances = iter_bundle.stages.get_instances()) {
        const Span<bke::InstanceReference> references = instances->references();
        const Span<int> handles = instances->reference_handles();
        for (const int i : handles.index_range()) {
          const int handle = handles[i];
          const GeometrySet &geometry = references[handle].geometry_set();
          if (StringRef(geometry.name).startswith(constraint_debug_name)) {
            /* Copy geometry instance. */
            const int new_handle = result.add_new_reference({geometry});
            result_handles[instance_i] = new_handle;
            result_transforms[instance_i] = float4x4::identity();
            ++instance_i;
          }
        }
      }
    });
  });
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const BundlePtr debug_bundle_ptr = params.extract_input<BundlePtr>("Debug Steps");
  if (!debug_bundle_ptr) {
    params.set_default_remaining_outputs();
    return;
  }
  BundleParseErrors parse_errors;
  std::optional<DebugBundle> debug_bundle = DebugBundle::parse(*debug_bundle_ptr, parse_errors);
  if (!debug_bundle) {
    // if (parse_errors.has_error()){
    //   params.error_message_add(...);
    // }
    params.set_default_remaining_outputs();
    return;
  }

  bke::Instances *result = new bke::Instances();

  IndexMask substeps_mask;
  const auto substep_mode = params.extract_input<SubstepMode>("Substep Mode");
  switch (substep_mode) {
    case SubstepMode::Single: {
      const IndexRange substeps_range = debug_bundle->substeps.index_range();
      const int substep_index = params.extract_input<int>("Substep Index");
      substeps_mask = substeps_range.contains(substep_index) ?
                          IndexRange::from_single(substep_index) :
                          IndexRange{};
      break;
    }
    case SubstepMode::All:
      substeps_mask = debug_bundle->substeps.index_range();
      break;
  }

  const MenuValue stage_menu = params.extract_input<MenuValue>("Stage");
  if (stage_menu.value >= constraint_stage_start) {
    const char *constraint_debug_name;
    if (RNA_enum_id_from_value(stage_items, stage_menu.value, &constraint_debug_name)) {
      const auto iter_mode = params.extract_input<IterationMode>("Iteration Mode");
      switch (iter_mode) {
        case IterationMode::Single: {
          const int iter_index = params.extract_input<int>("Iteration Index");
          gather_substeps_for_constraints(
              *debug_bundle,
              substeps_mask,
              constraint_debug_name,
              [iter_index](const IndexRange range) {
                return range.contains(iter_index) ? IndexRange::from_single(iter_index) :
                                                    IndexRange{};
              },
              *result);
          break;
        }
        case IterationMode::All:
          gather_substeps_for_constraints(
              *debug_bundle,
              substeps_mask,
              constraint_debug_name,
              [](const IndexRange range) { return range; },
              *result);
          break;
      }
    }
  }
  else {
    const auto stage = static_cast<physics_solver_debug::Stage>(stage_menu.value);
    gather_substeps_for_stage(*debug_bundle, substeps_mask, stage, *result);
  }

  params.set_output("Geometry", GeometrySet::from_instances(result));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeXPBDDebugExtract");
  ntype.ui_name = "XPBD Extract Debug Step";
  ntype.ui_description = "Extract a debug step from recorded data";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_xpbd_debug_extract_cc

namespace blender::nodes::physics_solver_debug {

template<typename NestedBundleT>
static void bundle_parse_bundle_member(const Bundle &bundle,
                                       const StringRef name,
                                       NestedBundleT &r_result,
                                       BundleParseErrors &r_errors)
{
  BundlePtr nested_bundle;
  bundle_parse_member(bundle, name, nested_bundle, r_errors);
  if (r_errors.has_error()) {
    return;
  }
  if (!nested_bundle) {
    return;
  }

  if (std::optional<NestedBundleT> parsed_result = NestedBundleT::parse(*nested_bundle, r_errors))
  {
    r_result = *parsed_result;
  }
  else {
    r_errors.wrong_members.append(name);
  }
}

template<typename NestedBundleT>
static void bundle_parse_list_member(const Bundle &bundle,
                                     const StringRef name,
                                     Vector<NestedBundleT> &r_result,
                                     BundleParseErrors &r_errors)
{
  ListPtr bundle_list;
  bundle_parse_member(bundle, name, bundle_list, r_errors);
  if (r_errors.has_error()) {
    return;
  }
  if (!bundle_list) {
    return;
  }

  r_result.resize(bundle_list->size());
  const VArray<BundlePtr> bundle_varray = bundle_list->varray<BundlePtr>();
  for (const int i : r_result.index_range()) {
    const BundlePtr &item = bundle_varray[i];
    if (std::optional<NestedBundleT> typed_bundle = NestedBundleT::parse(*item, r_errors)) {
      r_result[i] = std::move(*typed_bundle);
    }
    else {
      r_errors.wrong_members.append(name + "[" + std::to_string(i) + "]");
      r_result[i] = {};
    }
  }
}

const FlatBundleTypePtr &SolverStageBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(SolverStageBundle::name);
    b.add<decl::Geometry>("geometry");
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<SolverStageBundle> SolverStageBundle::parse(const Bundle &bundle,
                                                          BundleParseErrors &r_errors)
{
  SolverStageBundle typed_bundle;
  bundle_parse_member(bundle, "geometry", typed_bundle.geometry, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return typed_bundle;
}

BundlePtr SolverStageBundle::store() const
{
  BundlePtr bundle_ptr = Bundle::create();
  Bundle &bundle = const_cast<Bundle &>(*bundle_ptr);

  bundle.add("geometry", this->geometry);

  return bundle_ptr;
}

const FlatBundleTypePtr &ConstraintIterationBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(ConstraintIterationBundle::name);
    b.add<decl::Geometry>("stages");
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<ConstraintIterationBundle> ConstraintIterationBundle::parse(
    const Bundle &bundle, BundleParseErrors &r_errors)
{
  ConstraintIterationBundle typed_bundle;
  bundle_parse_member(bundle, "stages", typed_bundle.stages, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return typed_bundle;
}

BundlePtr ConstraintIterationBundle::store() const
{
  BundlePtr bundle_ptr = Bundle::create();
  Bundle &bundle = const_cast<Bundle &>(*bundle_ptr);

  bundle.add("stages", this->stages);

  return bundle_ptr;
}

const FlatBundleTypePtr &SubstepBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(SubstepBundle::name);
    b.add<decl::Bundle>("init_stage").structure_type(StructureType::Single);
    b.add<decl::Bundle>("dynamics_stage").structure_type(StructureType::Single);
    b.add<decl::Bundle>("constraint_iterations").structure_type(StructureType::List);
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<SubstepBundle> SubstepBundle::parse(const Bundle &bundle,
                                                  BundleParseErrors &r_errors)
{
  SubstepBundle result;
  bundle_parse_bundle_member(bundle, "init_stage", result.init_stage, r_errors);
  bundle_parse_bundle_member(bundle, "dynamics_stage", result.dynamics_stage, r_errors);
  bundle_parse_list_member(
      bundle, "constraint_iterations", result.constraint_iterations, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return result;
}

BundlePtr SubstepBundle::store() const
{
  BundlePtr bundle_ptr = Bundle::create();
  Bundle &bundle = const_cast<Bundle &>(*bundle_ptr);

  bundle.add("init_stage", this->init_stage.store());
  bundle.add("dynamics_stage", this->dynamics_stage.store());

  const int count = this->constraint_iterations.size();
  Array<BundlePtr> constraint_iterations_array(count);
  for (const int i : this->constraint_iterations.index_range()) {
    constraint_iterations_array[i] = this->constraint_iterations[i].store();
  }
  ListPtr constraint_iterations_list = List::from_container(
      std::move(constraint_iterations_array));

  bundle.add(
      "constraint_iterations",
      BundleItemSocketValue{bke::node_socket_type_find_static(SOCK_BUNDLE),
                            bke::SocketValueVariant::From(std::move(constraint_iterations_list))});

  return bundle_ptr;
}

const FlatBundleTypePtr &DebugBundle::get_bundle_type()
{
  static const FlatBundleTypePtr bundle_type = []() {
    FlatBundleTypeBuilder b(DebugBundle::name);
    b.add<decl::Bundle>("substeps").structure_type(StructureType::List);
    const FlatBundleTypePtr bundle_type = b.build();
    BundleTypeRegistry::register_type(bundle_type);
    return bundle_type;
  }();
  return bundle_type;
}

std::optional<DebugBundle> DebugBundle::parse(const Bundle &bundle, BundleParseErrors &r_errors)
{
  DebugBundle result;
  bundle_parse_list_member(bundle, "substeps", result.substeps, r_errors);
  if (r_errors.has_error()) {
    return std::nullopt;
  }
  return result;
}

BundlePtr DebugBundle::store() const
{
  const int count = this->substeps.size();
  Array<BundlePtr> substeps_array(count);
  for (const int i : this->substeps.index_range()) {
    substeps_array[i] = this->substeps[i].store();
  }
  ListPtr substeps_list = List::from_container(std::move(substeps_array));

  BundlePtr bundle_ptr = Bundle::create();
  Bundle &bundle = const_cast<Bundle &>(*bundle_ptr);
  bundle.add("substeps",
             BundleItemSocketValue{bke::node_socket_type_find_static(SOCK_BUNDLE),
                                   bke::SocketValueVariant::From(std::move(substeps_list))});

  return bundle_ptr;
}

}  // namespace blender::nodes::physics_solver_debug
