/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute_math.hh"

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_function_ref.hh"
#include "BLI_generic_array.hh"
#include "BLI_generic_span.hh"
#include "BLI_generic_virtual_array.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_base.hh"

#include "GEO_abstract_kd_bucket_hierarchy.hh"
#include "GEO_fast_multipole_method.hh"
#include "GEO_bounding_sphere.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_evaluate_in_space_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  b.add_input<decl::Vector>("Position").implicit_field(implicit_field_inputs::position);

  if (node != nullptr) {
    const eCustomDataType data_type = eCustomDataType(node->custom1);
    b.add_input(data_type, "Value").supports_field().hide_value();
  }

  b.add_input<decl::Int>("Power").default_value(2).min(0).hide_value();
  b.add_input<decl::Float>("Error").min(1.0f).default_value(2.0f);
  b.add_input<decl::Float>("Offset");

  if (node != nullptr) {
    const eCustomDataType data_type = eCustomDataType(node->custom1);
    b.add_output(data_type, "Value").field_source_reference_all();
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = CD_PROP_FLOAT;
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void cloud_radii_to_min_distance(const Span<float> src_radii,
                                        const int distance_power,
                                        const float precision,
                                        MutableSpan<float> dst_radii)
{
  threading::parallel_for(src_radii.index_range(), 1024 * 8, [&](const IndexRange range) {
    for (const int i : range) {
      dst_radii[i] = geometry::fmm::minimal_dinstance_to_claster(src_radii[i], distance_power, precision);
    }
  });
}

class SpaceValueFieldInput final : public bke::GeometryFieldInput {
 private:
  Field<float3> positions_field_;
  GField value_field_;
  int distance_power_;
  float precision_;
  float offset_value_;

 public:
  SpaceValueFieldInput(Field<float3> positions_field,
                       GField value_field,
                       const int distance_power,
                       const float precision,
                       const float offset_value)
      : bke::GeometryFieldInput(value_field.cpp_type(), "Space Value"),
        positions_field_(std::move(positions_field)),
        value_field_(std::move(value_field)),
        distance_power_(distance_power),
        precision_(precision),
        offset_value_(offset_value)
  {
  }

  GVArray get_varray_for_context(const bke::GeometryFieldContext &context,
                                 const IndexMask & /*mask*/) const final
  {
    if (!context.attributes()) {
      return {};
    }
    const int domain_size = context.attributes()->domain_size(context.domain());
    fn::FieldEvaluator evaluator{context, domain_size};
    evaluator.add(positions_field_);
    evaluator.add(value_field_);
    evaluator.evaluate();
    const VArraySpan<float3> positions = evaluator.get_evaluated<float3>(0);
    const GVArray src_values = evaluator.get_evaluated(1);

    const CPPType &data_type = src_values.type();

    using namespace blender::geometry;

    const int total_depth = akdbh::total_depth_from_total(positions.size());
    const int total_buckets = akdbh::total_buckets_for(total_depth);
    const int total_joints = akdbh::total_joints_for_depth(total_depth);

    Array<int, 0> start_indices(total_buckets + 1);
    const OffsetIndices<int> base_offsets = akdbh::fill_bucket_offsets_trivial(domain_size, start_indices);

    Array<int, 0> indices(domain_size);
    akdbh::from_positions(positions, base_offsets, total_depth, indices);

    Array<float3, 0> bucket_positions(domain_size);
    GArray<> bucket_values(data_type, domain_size);

    array_utils::gather(Span<float3>(positions), indices.as_span(), bucket_positions.as_mutable_span());
    bke::attribute_math::gather(src_values, indices.as_span(), bucket_values.as_mutable_span());

    GArray<> joints_values(data_type, total_joints);
    akdbh::mean_sums(base_offsets, total_depth, bucket_values, joints_values);

    Array<float3, 0> joints_positions(total_joints);
    Array<float, 0> joints_min_distance_reduced(total_joints);
    bounding::joints_packing_spheres(base_offsets, total_depth, bucket_positions, joints_positions, joints_min_distance_reduced);

    cloud_radii_to_min_distance(joints_min_distance_reduced, distance_power_, precision_, joints_min_distance_reduced);

    threading::parallel_for(IndexRange(total_joints), 1024 * 16, [&](const IndexRange range) {
      for (const int i : range) {
        joints_min_distance_reduced[i] -= offset_value_;
      }
    });

    GArray<> sampled_bucket_values(data_type, domain_size);
    data_type.value_initialize_n(sampled_bucket_values.data(), sampled_bucket_values.size());
    fmm::akdbh_sample_value(base_offsets,
                            total_depth,
                            joints_positions,
                            bucket_positions,
                            joints_min_distance_reduced,
                            joints_values.as_span(),
                            bucket_values.as_span(),
                            distance_power_,
                            offset_value_,
                            sampled_bucket_values.as_mutable_span());

    GArray<> dst_values(data_type, domain_size);
    geometry::akdbh::to_static_type(data_type, [&](auto dummy) {
      using T = decltype(dummy);
      array_utils::scatter<T>(sampled_bucket_values.as_span().typed<T>(),
                              indices.as_span(),
                              dst_values.as_mutable_span().typed<T>());
    });

    return GVArray::ForGArray(std::move(dst_values));
  }

 public:
  void for_each_field_input_recursive(FunctionRef<void(const FieldInput &)> fn) const
  {
    positions_field_.node().for_each_field_input_recursive(fn);
    value_field_.node().for_each_field_input_recursive(fn);
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<float3> position_field = params.extract_input<Field<float3>>("Position");
  GField value_field = params.extract_input<GField>("Value");

  const int power_value = params.extract_input<int>("Power");
  const float precision_value = params.extract_input<float>("Error");
  const float offset_value = params.extract_input<float>("Offset");

  params.set_output("Value",
                    GField(std::make_shared<SpaceValueFieldInput>(std::move(position_field),
                                                                  std::move(value_field),
                                                                  power_value,
                                                                  precision_value,
                                                                  offset_value)));
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
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeFieldInSpace");
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.ui_name = "Field in Space";
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(&ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_evaluate_in_space_cc
