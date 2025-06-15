/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute_math.hh"

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_generic_array.hh"
#include "BLI_generic_span.hh"
#include "BLI_generic_virtual_array.hh"
#include "BLI_task_size_hints.hh"
#include "BLI_virtual_array.hh"

#include "GEO_abstract_kd_bucket_hierarchy.hh"
#include "GEO_bounding_sphere.hh"
#include "GEO_fast_multipole_method.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

#include "NOD_rna_define.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sample_in_space_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  b.add_input<decl::Geometry>("Source");
  b.add_input<decl::Vector>("Position").implicit_field_on_all(NODE_DEFAULT_INPUT_POSITION_FIELD);

  if (node != nullptr) {
    const eCustomDataType data_type = eCustomDataType(node->custom1);
    b.add_input(data_type, "Value").field_on_all().hide_value();
  }

  b.add_input<decl::Vector>("Sample Position")
      .supports_field()
      .implicit_field(NODE_DEFAULT_INPUT_POSITION_FIELD);

  b.add_input<decl::Int>("Power").default_value(2).min(0).hide_value();
  b.add_input<decl::Float>("Error").min(1.0f).default_value(2.0f);
  b.add_input<decl::Float>("Offset");

  if (node != nullptr) {
    const eCustomDataType data_type = eCustomDataType(node->custom1);
    b.add_output(data_type, "Value").dependent_field({3});
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int16_t(CD_PROP_FLOAT);
  node->custom2 = int16_t(bke::AttrDomain::Point);
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
  layout->prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
}

static bool component_is_available(const GeometrySet &geometry,
                                   const GeometryComponent::Type type,
                                   const AttrDomain domain)
{
  if (!geometry.has(type)) {
    return false;
  }
  const GeometryComponent &component = *geometry.get_component(type);
  return component.attribute_domain_size(domain) != 0;
}

static const GeometryComponent *find_source_component(const GeometrySet &geometry,
                                                      const AttrDomain domain)
{
  /* Choose the other component based on a consistent order, rather than some more complicated
   * heuristic. This is the same order visible in the spreadsheet and used in the ray-cast node. */
  static const Array<GeometryComponent::Type> supported_types = {
      GeometryComponent::Type::Mesh,
      GeometryComponent::Type::PointCloud,
      GeometryComponent::Type::Curve,
      GeometryComponent::Type::Instance};
  for (const GeometryComponent::Type src_type : supported_types) {
    if (component_is_available(geometry, src_type, domain)) {
      return geometry.get_component(src_type);
    }
  }

  return nullptr;
}

static void cloud_radii_to_min_distance(const Span<float> src_radii,
                                        const int distance_power,
                                        const float precision,
                                        MutableSpan<float> dst_radii)
{
  threading::parallel_for(src_radii.index_range(), 1024 * 8, [&](const IndexRange range) {
    for (const int i : range) {
      dst_radii[i] = geometry::fmm::minimal_dinstance_to_claster(
          src_radii[i], distance_power, precision);
    }
  });
}

class GradientSumFunction : public mf::MultiFunction {
 private:
  mf::Signature signature_;

  int power_value_;
  float offset_value_;

  int total_depth_;

  Array<int, 0> offset_indices_;

  std::array<Array<float, 0>, 3> bucket_positions_;
  Array<Array<float, 0>, 3> bucket_values_;

  Array<float3, 0> joints_positions_;
  Array<Array<float, 0>, 3> joints_values_;

  Array<float, 0> joints_min_distance_;

 public:
  GradientSumFunction(const GeometrySet &geometry_set,
                      const bke::AttrDomain domain,
                      const float precision,
                      const int power_value,
                      const float offset_value,
                      Field<float3> position_field,
                      GField value_field)
      : power_value_(power_value), offset_value_(offset_value)
  {/*
    const CPPType &data_type = value_field.cpp_type();
    mf::SignatureBuilder builder("Space Value", signature_);
    builder.single_input<float3>("Position");
    builder.single_output("Value", data_type);
    this->set_signature(&signature_);

    const GeometryComponent *source_component = find_source_component(geometry_set, domain);
    if (source_component == nullptr) {
      throw std::runtime_error("no component with choosen domain");
    }

    const int domain_size = source_component->attributes()->domain_size(domain);
    const bke::GeometryFieldContext context(*source_component, domain);
    fn::FieldEvaluator evaluator(context, domain_size);
    evaluator.add(std::move(position_field));
    evaluator.add(std::move(value_field));
    evaluator.evaluate();
    const VArraySpan<float3> positions = evaluator.get_evaluated<float3>(0);
    const GVArray src_values = evaluator.get_evaluated(1);

    using namespace blender::geometry;

    total_depth_ = akdbh::total_depth_from_total(domain_size);
    const int total_buckets = akdbh::total_buckets_for(total_depth_);
    const int total_joints = akdbh::total_joints_for_depth(total_depth_);

    offset_indices_.reinitialize(total_buckets + 1);
    const OffsetIndices<int> base_offsets = akdbh::fill_bucket_offsets_trivial(domain_size, offset_indices_);

    Array<int> indices(domain_size);
    akdbh::from_positions(positions, base_offsets, total_depth_, indices);

    Array<float3> bucket_positions;
    bucket_positions.reinitialize(domain_size);
    array_utils::gather(
        Span<float3>(positions), indices.as_span(), bucket_positions.as_mutable_span());
    {
      bucket_positions_[0].reinitialize(domain_size);
      bucket_positions_[1].reinitialize(domain_size);
      bucket_positions_[2].reinitialize(domain_size);
      threading::parallel_for(IndexRange(domain_size), 4096, [&](const IndexRange range) {
        for (const int i : range) {
          bucket_positions_[0][i] = bucket_positions[i].x;
          bucket_positions_[1][i] = bucket_positions[i].y;
          bucket_positions_[2][i] = bucket_positions[i].z;
        }
      });
    }

    BLI_assert(data_type.is<float>() || data_type.is<float3>());
    const int data_axes_count = data_type.is<float3>() ? 3 : 1;

    GArray<> bucket_values(data_type, domain_size);
    bke::attribute_math::gather(src_values, indices.as_span(), bucket_values.as_mutable_span());
    bucket_values_.reinitialize(data_axes_count);
    {
      for (const int axis_i : IndexRange(data_axes_count)) {
        bucket_values_[axis_i].reinitialize(domain_size);
      }
      threading::parallel_for(IndexRange(domain_size), 4096, [&](const IndexRange range) {
        if (data_type.is<float3>()) {
          for (const int i : range) {
            bucket_values_[0][i] = bucket_values.as_span().typed<float3>()[i].x;
            bucket_values_[1][i] = bucket_values.as_span().typed<float3>()[i].y;
            bucket_values_[2][i] = bucket_values.as_span().typed<float3>()[i].z;
          }
        }
        else {
          for (const int i : range) {
            bucket_values_[0][i] = bucket_values.as_span().typed<float>()[i];
          }
        }
      });
    }

    GArray<> joints_values(data_type, total_joints);
    akdbh::mean_sums(base_offsets, total_depth_, bucket_values_, joints_values);
    joints_values_.reinitialize(data_axes_count);
    {
      for (const int axis_i : IndexRange(data_axes_count)) {
        joints_values_[axis_i].reinitialize(total_joints);
      }
      threading::parallel_for(IndexRange(total_joints), 4096, [&](const IndexRange range) {
        if (data_type.is<float3>()) {
          for (const int i : range) {
            joints_values_[0][i] = joints_values.as_span().typed<float3>()[i].x;
            joints_values_[1][i] = joints_values.as_span().typed<float3>()[i].y;
            joints_values_[2][i] = joints_values.as_span().typed<float3>()[i].z;
          }
        }
        else {
          for (const int i : range) {
            joints_values_[0][i] = joints_values.as_span().typed<float>()[i];
          }
        }
      });
    }

    joints_positions_.reinitialize(total_joints);
    joints_min_distance_.reinitialize(total_joints);
    bounding::joints_packing_spheres(base_offsets,
                                     total_depth_,
                                     bucket_positions_,
                                     joints_positions_,
                                     joints_min_distance_.as_mutable_span());

    cloud_radii_to_min_distance(joints_min_distance_.as_span(),
                                power_value_,
                                precision,
                                joints_min_distance_.as_mutable_span());*/
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {/*
    const VArraySpan<float3> positions = params.readonly_single_input<float3>(0, "Position");
    GMutableSpan results = params.uninitialized_single_output(1, "Value");

    Array<float3> task_positions(mask.size());
    array_utils::gather(positions, mask, task_positions.as_mutable_span());
    GArray<> task_results(results.type(), mask.size());

    results.type().value_initialize_n(task_results.data(), task_results.size());

    using namespace blender::geometry;
    // fmm::akdbh_accumulate_in(OffsetIndices<int>(offset_indices_),
    //                          total_depth_,
    //                          joints_min_distance_,
    //                          joints_positions_,
    //                          joints_values_,
    //                          bucket_positions_,
    //                          bucket_values_,
    //                          power_value_,
    //                          offset_value_,
    //                          task_positions,
    //                          task_results,
    //                          std::nullopt);

    geometry::akdbh::to_static_type(results.type(), [&](auto dummy) {
      using T = decltype(dummy);
      array_utils::scatter<T>(task_results.as_span().typed<T>(), mask, results.typed<T>());
    });*/
  }

  ExecutionHints get_execution_hints() const override
  {
    ExecutionHints hints;
    hints.min_grain_size = 1024 * 16 / math::max(1, total_depth_);
    return hints;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const GeometrySet geometry = params.extract_input<bke::GeometrySet>("Source");
  Field<float3> position_field = params.extract_input<Field<float3>>("Position");
  GField value_field = params.extract_input<GField>("Value");

  const bke::AttrDomain domain = bke::AttrDomain(params.node().custom2);

  const int power_value = params.extract_input<int>("Power");
  const float precision_value = params.extract_input<float>("Error");
  const float offset_value = params.extract_input<float>("Offset");

  std::unique_ptr<GradientSumFunction> space_fn;
  try {
    space_fn = std::make_unique<GradientSumFunction>(geometry,
                                                     domain,
                                                     precision_value,
                                                     power_value,
                                                     offset_value,
                                                     std::move(position_field),
                                                     std::move(value_field));
  }
  catch (const std::runtime_error &) {
    params.set_default_remaining_outputs();
    return;
  }

  Field<float3> sample_position_field = params.extract_input<Field<float3>>("Sample Position");
  std::shared_ptr<FieldOperation> sample_space_op = FieldOperation::Create(
      std::move(space_fn), {std::move(sample_position_field)});

  params.set_output("Value", GField(sample_space_op, 0));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "data_type",
      "Data Type",
      "",
      rna_enum_attribute_type_items,
      NOD_inline_enum_accessors(custom1),
      CD_PROP_FLOAT,
      [](bContext * /*C*/, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        return enum_items_filter(rna_enum_attribute_type_items,
                                 [](const EnumPropertyItem &item) -> bool {
                                   return ELEM(item.value, CD_PROP_FLOAT, CD_PROP_FLOAT3);
                                 });
      });
  RNA_def_node_enum(srna,
                    "domain",
                    "Domain",
                    "",
                    rna_enum_attribute_domain_items,
                    NOD_inline_enum_accessors(custom2),
                    int(AttrDomain::Point));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSampleInSpace");
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.ui_name = "Sample in Space";
  ntype.geometry_node_execute = node_geo_exec;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sample_in_space_cc
