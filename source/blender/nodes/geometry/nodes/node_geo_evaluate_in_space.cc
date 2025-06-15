/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_timeit.hh"
#include <iostream>

#include "BKE_attribute_math.hh"

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_generic_array.hh"
#include "BLI_generic_virtual_array.hh"
#include "BLI_task_size_hints.hh"

#include "GEO_abstract_kd_bucket_hierarchy.hh"
#include "GEO_bounding_sphere.hh"
#include "GEO_fast_multipole_method.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

#include "NOD_rna_define.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_evaluate_in_space_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  b.add_input<decl::Vector>("Position").implicit_field(NODE_DEFAULT_INPUT_POSITION_FIELD);

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
  
  
  b.add_input<decl::Float>("Index W").default_value(1);
  b.add_input<decl::Float>("Distance W").default_value(0);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int16_t(CD_PROP_FLOAT);
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
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

class SpaceValueFieldInput final : public bke::GeometryFieldInput {
 private:
  Field<float3> positions_field_;
  GField value_field_;
  int power_value_;
  float precision_;
  float offset_value_;
  float index_w_;
  float distance_w_;

 public:
  SpaceValueFieldInput(Field<float3> positions_field,
                       GField value_field,
                       const int distance_power,
                       const float precision,
                       const float offset_value,
                       const float index_w,
                       const float distance_w)
      : bke::GeometryFieldInput(value_field.cpp_type(), "Space Value"),
        positions_field_(std::move(positions_field)),
        value_field_(std::move(value_field)),
        power_value_(distance_power),
        precision_(precision),
        offset_value_(offset_value),
        index_w_(index_w),
        distance_w_(distance_w)
  {
  }

  GVArray get_varray_for_context(const bke::GeometryFieldContext &context,
                                 const IndexMask & /*mask*/) const final
  {
    std::cout << "\n";
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
    OffsetIndices<int> base_offsets;

    // {
    //   base_offsets = akdbh::fill_bucket_offsets_trivial(domain_size, start_indices
    // }

    Array<int, 0> indices(domain_size);
    // {
    //   SCOPED_TIMER_AVERAGED("from_positions");
    //   akdbh::from_positions(positions, base_offsets, total_depth, indices);
    // }

    {
      SCOPED_TIMER_AVERAGED("from_positions_non_uniform");
      akdbh::from_positions_non_uniform(positions, total_depth, start_indices, indices, index_w_, distance_w_);
      base_offsets = OffsetIndices<int>(start_indices.as_span());
    }

    Array<float3, 0> bucket_positions(domain_size);
    GArray<> bucket_values(data_type, domain_size);

    {
      SCOPED_TIMER_AVERAGED("gather");
      array_utils::gather(
          Span<float3>(positions), indices.as_span(), bucket_positions.as_mutable_span());
      bke::attribute_math::gather(src_values, indices.as_span(), bucket_values.as_mutable_span());
    }

    GArray<> joints_values(data_type, total_joints);
    akdbh::mean_sums(base_offsets, total_depth, bucket_values, joints_values);

    const int data_axes_count = bucket_values.type().is<float3>() ? 3 : 1;
    BLI_assert(bucket_values.type().is<float3>() || bucket_values.type().is<float>());

    Array<Array<float, 0>, 3> bucket_values_splited(data_axes_count);
    {
      SCOPED_TIMER_AVERAGED("split buckets values");
      for (const int axis_i : IndexRange(data_axes_count)) {
        bucket_values_splited[axis_i].reinitialize(domain_size);
      }
      threading::parallel_for(IndexRange(domain_size), 4096, [&](const IndexRange range) {
        if (bucket_values.type().is<float3>()) {
          for (const int i : range) {
            bucket_values_splited[0][i] = bucket_values.as_span().typed<float3>()[i].x;
            bucket_values_splited[1][i] = bucket_values.as_span().typed<float3>()[i].y;
            bucket_values_splited[2][i] = bucket_values.as_span().typed<float3>()[i].z;
          }
        }
        else {
          for (const int i : range) {
            bucket_values_splited[0][i] = bucket_values.as_span().typed<float>()[i];
          }
        }
      });
    }

    Array<Array<float, 0>, 3> joints_values_splited(data_axes_count);
    {
      SCOPED_TIMER_AVERAGED("split joint values");
      for (const int axis_i : IndexRange(data_axes_count)) {
        joints_values_splited[axis_i].reinitialize(total_joints);
      }
      threading::parallel_for(IndexRange(total_joints), 4096, [&](const IndexRange range) {
        if (joints_values.type().is<float3>()) {
          for (const int i : range) {
            joints_values_splited[0][i] = joints_values.as_span().typed<float3>()[i].x;
            joints_values_splited[1][i] = joints_values.as_span().typed<float3>()[i].y;
            joints_values_splited[2][i] = joints_values.as_span().typed<float3>()[i].z;
          }
        }
        else {
          for (const int i : range) {
            joints_values_splited[0][i] = joints_values.as_span().typed<float>()[i];
          }
        }
      });
    }

    Array<float3, 0> joints_positions(total_joints);
    Array<float, 0> joints_min_distance(total_joints);
    {
      SCOPED_TIMER_AVERAGED("joints_packing_spheres");
      bounding::joints_packing_spheres(base_offsets,
                                       total_depth,
                                       bucket_positions,
                                       joints_positions,
                                       joints_min_distance.as_mutable_span());
    }
    {
      SCOPED_TIMER_AVERAGED("cloud_radii_to_min_distance");
      cloud_radii_to_min_distance(joints_min_distance.as_span(),
                                  power_value_,
                                  precision_,
                                  joints_min_distance.as_mutable_span());
    }

    std::array<Array<float, 0>, 3> bucket_positions_splited;
    {
      SCOPED_TIMER_AVERAGED("split positions");
      bucket_positions_splited[0].reinitialize(bucket_positions.size());
      bucket_positions_splited[1].reinitialize(bucket_positions.size());
      bucket_positions_splited[2].reinitialize(bucket_positions.size());
      threading::parallel_for(bucket_positions.index_range(), 4096, [&](const IndexRange range) {
        for (const int i : range) {
          bucket_positions_splited[0][i] = bucket_positions[i].x;
          bucket_positions_splited[1][i] = bucket_positions[i].y;
          bucket_positions_splited[2][i] = bucket_positions[i].z;
        }
      });
    }

    Array<Array<float, 0>, 3> sampled_bucket_values_splitted(data_axes_count);
    for (const int axis_i : IndexRange(data_axes_count)) {
      sampled_bucket_values_splitted[axis_i].reinitialize(domain_size);
    }

    {
      SCOPED_TIMER_AVERAGED("akdbh_accumulate_in");

      const std::array<Span<float>, 3> bucket_positions = {bucket_positions_splited[0].as_span(),
                                                           bucket_positions_splited[1].as_span(),
                                                           bucket_positions_splited[2].as_span()};

      Array<Span<float>, 3> joints_values(data_axes_count);
      for (const int axis_i : IndexRange(data_axes_count)) {
        joints_values[axis_i] = joints_values_splited[axis_i].as_span();
      }

      Array<Span<float>, 3> bucket_values(data_axes_count);
      for (const int axis_i : IndexRange(data_axes_count)) {
        bucket_values[axis_i] = bucket_values_splited[axis_i].as_span();
      }

      threading::parallel_for(
          IndexRange(domain_size),
          1024,
          [&](const IndexRange range) {
            const std::array<Span<float>, 3> sample_bucket_positions = {
                bucket_positions[0].slice(range),
                bucket_positions[1].slice(range),
                bucket_positions[2].slice(range)};
            Array<MutableSpan<float>, 3> sampled_bucket_values(data_axes_count);
            for (const int axis_i : IndexRange(data_axes_count)) {
              sampled_bucket_values[axis_i] =
                  sampled_bucket_values_splitted[axis_i].as_mutable_span().slice(range);
            }

            fmm::akdbh_accumulate_in(base_offsets,
                                     total_depth,
                                     joints_min_distance,
                                     joints_positions,
                                     joints_values.as_span(),
                                     bucket_positions,
                                     bucket_values.as_span(),
                                     power_value_,
                                     offset_value_,
                                     sample_bucket_positions,
                                     sampled_bucket_values.as_span(),
                                     range);
          },
          threading::detail::TaskSizeHints_Static(total_depth));
    }

    GArray<> sampled_bucket_values(data_type, domain_size);
    data_type.value_initialize_n(sampled_bucket_values.data(), sampled_bucket_values.size());

    threading::parallel_for(IndexRange(domain_size), 4096, [&](const IndexRange range) {
      if (sampled_bucket_values.type().is<float3>()) {
        for (const int i : range) {
          sampled_bucket_values.as_mutable_span().typed<float3>()[i].x =
              sampled_bucket_values_splitted[0][i];
          sampled_bucket_values.as_mutable_span().typed<float3>()[i].y =
              sampled_bucket_values_splitted[1][i];
          sampled_bucket_values.as_mutable_span().typed<float3>()[i].z =
              sampled_bucket_values_splitted[2][i];
        }
      }
      else {
        for (const int i : range) {
          sampled_bucket_values.as_mutable_span().typed<float>()[i] =
              sampled_bucket_values_splitted[0][i];
        }
      }
    });

    GArray<> dst_values(data_type, domain_size);
    {
      SCOPED_TIMER_AVERAGED("scatter");
      geometry::akdbh::to_static_type(data_type, [&](auto dummy) {
        using T = decltype(dummy);
        array_utils::scatter<T>(sampled_bucket_values.as_span().typed<T>(),
                                indices.as_span(),
                                dst_values.as_mutable_span().typed<T>());
      });
    }

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

  const float index_w = params.extract_input<float>("Index W");
  const float distance_w = params.extract_input<float>("Distance W");

  params.set_output("Value",
                    GField(std::make_shared<SpaceValueFieldInput>(std::move(position_field),
                                                                  std::move(value_field),
                                                                  power_value,
                                                                  precision_value,
                                                                  offset_value,
                                                                  index_w,
                                                                  distance_w)));
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
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_evaluate_in_space_cc
