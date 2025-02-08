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

#include "node_geometry_util.hh"

namespace blender {

template<typename T> std::ostream &operator<<(std::ostream &stream, Vector<T> data);

template<typename T, int num>
std::ostream &operator<<(std::ostream &stream, const std::array<T, num> &data)
{
  stream << "{";
  for (const int64_t i : IndexRange(num)) {
    stream << data[i] << (num - 1 == i ? "" : "\t");
  }
  stream << "}";
  return stream;
}

template<typename T> std::ostream &operator<<(std::ostream &stream, const Span<T> span)
{
  for (const int64_t i : span.index_range()) {
    stream << span[i] << (span.size() - 1 == i ? "" : "\t");
  }
  return stream;
}

template<typename T> std::ostream &operator<<(std::ostream &stream, MutableSpan<T> span)
{
  stream << span.as_span();
  return stream;
}

template<typename T> std::ostream &operator<<(std::ostream &stream, Vector<T> data)
{
  stream << data.as_span();
  return stream;
}

template<typename T> std::ostream &operator<<(std::ostream &stream, Array<T> data)
{
  stream << data.as_span();
  return stream;
}

}  // namespace blender

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

template<typename InT, typename OutT, typename FuncT>
static void parallel_transform(const Span<InT> src,
                               const int grain_size,
                               MutableSpan<OutT> dst,
                               const FuncT func)
{
  BLI_assert(src.size() == dst.size());
  threading::parallel_for(src.index_range(), grain_size, [&](const IndexRange range) {
    const Span<InT> src_slice = src.slice(range);
    MutableSpan<OutT> dst_slice = dst.slice(range);
    std::transform(src_slice.begin(), src_slice.end(), dst_slice.begin(), func);
  });
}

static FunctionRef<void(int, MutableSpan<float>)> powered_rcp_for_values(const int power_value)
{
  switch (power_value) {
    case 0:
      return [](const int /*power_value*/, MutableSpan<float> values) { values.fill(1.0f); };
    case 1:
      return [](const int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [&](const float value) {
          return math::safe_rcp(value);
        });
      };
    case 2:
      return [](const int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [&](const float value) {
          return math::safe_rcp(value * value);
        });
      };
    case 3:
      return [](const int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [&](const float value) {
          return math::safe_rcp(value * value * value);
        });
      };
    case 4:
      return [](const int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [&](const float value) {
          const float squared = math::square(value);
          return math::safe_rcp(squared * squared);
        });
      };
    case 5:
      return [](const int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [&](const float value) {
          const float squared = math::square(value);
          return math::safe_rcp(squared * squared * value);
        });
      };
    case 6:
      return [](const int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [&](const float value) {
          const float squared = math::square(value);
          return math::safe_rcp(squared * squared * squared);
        });
      };
    case 7:
      return [](const int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [&](const float value) {
          const float squared = math::square(value);
          const float fourth_degree = math::square(squared);
          return math::safe_rcp(fourth_degree * squared * value);
        });
      };
    case 8:
      return [](const int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [&](const float value) {
          const float squared = math::square(value);
          const float fourth_degree = math::square(squared);
          return math::safe_rcp(fourth_degree * fourth_degree);
        });
      };
    case 9:
      return [](const int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [&](const float value) {
          const float squared = math::square(value);
          const float fourth_degree = math::square(squared);
          return math::safe_rcp(fourth_degree * fourth_degree * value);
        });
      };
    default:
      return [](const int power_value, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [&](const float value) {
          return math::pow<float>(math::safe_rcp(value), power_value);
        });
      };
  }
}

struct Item {
  int depth_i;
  int joint_i;
  int prefix_to_visit;
};

template<typename LeafFuncT, typename JointPredicateT, typename JointFuncT>
static void for_each_to_bottom_skip(const OffsetIndices<int> buckets_offsets,
                                    const int total_depth,
                                    const IndexRange range,
                                    const JointPredicateT &joint_predicate,
                                    const JointFuncT &joint_func,
                                    const LeafFuncT &leaf_func)
{
  Array<int, 0> indices(range.size());
  array_utils::fill_index_range<int>(indices, range.start());
  Vector<Item, 32> stack = {Item{0, 0, int(indices.size())}};
  while (!stack.is_empty()) {
    const Item item = stack.pop_last();
    const int depth_i = item.depth_i;
    const int joint_i = item.joint_i;
    const MutableSpan<int> to_visit = indices.as_mutable_span().take_front(item.prefix_to_visit);
    const IndexRange joints_range = geometry::akdbh::joints_range_at_depth(depth_i);
    const IndexRange joint_buckets = geometry::akdbh::joint_buckets_range_at_depth(
        total_depth, depth_i, joint_i);

    const auto end_of_prefix = std::stable_partition(
        to_visit.begin(), to_visit.end(), [&](const int i) -> bool {
          return joint_predicate(int(joints_range[joint_i]), i);
        });

    const Span<int> finished_indices = to_visit.drop_front(
        std::distance(to_visit.begin(), end_of_prefix));
    joint_func(buckets_offsets[joint_buckets], int(joints_range[joint_i]), finished_indices);

    const Span<int> next_indices = to_visit.take_front(
        std::distance(to_visit.begin(), end_of_prefix));
    if (next_indices.is_empty()) {
      continue;
    }

    if (depth_i == total_depth - 1) {
      leaf_func(buckets_offsets[joint_i], next_indices);
      continue;
    }

    stack.append({depth_i + 1, joint_i * 2 + 1, int(next_indices.size())});
    stack.append({depth_i + 1, joint_i * 2 + 0, int(next_indices.size())});
  }
}

static void sample_mean_average(const OffsetIndices<int> buckets_offsets,
                                const int total_depth,
                                const Span<float3> src_joints_centre,
                                const Span<float3> src_bucket_position,
                                const Span<float> src_joints_min_distance,
                                const GSpan src_joints_value,
                                const GSpan src_bucket_value,
                                const int power_value,
                                const float offset_value,
                                GMutableSpan dst_buckets_data)
{
  BLI_assert(src_joints_centre.size() == src_joints_min_distance.size());
  BLI_assert(src_joints_centre.size() == src_joints_value.size());
  BLI_assert(src_bucket_value.size() == dst_buckets_data.size());
  BLI_assert(src_bucket_value.size() == src_bucket_position.size());
  BLI_assert(dst_buckets_data.type() == src_joints_value.type());
  BLI_assert(dst_buckets_data.type() == src_bucket_value.type());

  const FunctionRef<void(int, MutableSpan<float>)> distance_invertion = powered_rcp_for_values(
      power_value);

  geometry::akdbh::to_static_type(src_joints_value.type(), [&](auto dummy) {
    using T = decltype(dummy);

    const Span<T> typed_src_joints_value = src_joints_value.typed<T>();
    const Span<T> typed_src_bucket_value = src_bucket_value.typed<T>();
    MutableSpan<T> typed_dst_buckets_data = dst_buckets_data.typed<T>();

    threading::parallel_for(
        src_bucket_position.index_range(), 1024 * 8, [&](const IndexRange range) {
          Vector<float, 0> buffer;
          buffer.reserve(range.size());

          for_each_to_bottom_skip(
              buckets_offsets,
              total_depth,
              range,
              [&](const int joint_index, const int value_i) -> bool {
                return math::distance(src_joints_centre[joint_index],
                                      src_bucket_position[value_i]) +
                           offset_value <=
                       src_joints_min_distance[joint_index];
              },
              [&](const IndexRange buckets_range,
                  const int joint_index,
                  const Span<int> value_indices) {
                buffer.resize(value_indices.size());
                for (const int value_i : value_indices.index_range()) {
                  const int value_index = value_indices[value_i];
                  buffer[value_i] = math::distance(src_joints_centre[joint_index],
                                                   src_bucket_position[value_index]) +
                                    offset_value;
                }

                distance_invertion(power_value, buffer.as_mutable_span());

                const float total_factor = buckets_range.size();
                for (const int value_i : value_indices.index_range()) {
                  const int value_index = value_indices[value_i];
                  typed_dst_buckets_data[value_index] += typed_src_joints_value[joint_index] *
                                                         buffer[value_i] * total_factor;
                }
              },
              [&](const IndexRange bucket_range, const Span<int> value_indices) {
                buffer.resize(bucket_range.size());
                for (const int value_i : value_indices) {
                  const float3 position = src_bucket_position[value_i];

                  for (const int index : bucket_range.index_range()) {
                    buffer[index] = math::distance(src_bucket_position[bucket_range[index]],
                                                   position) +
                                    offset_value;
                  }

                  distance_invertion(power_value, buffer.as_mutable_span());

                  for (const int i : bucket_range.index_range()) {
                    const int index = bucket_range[i];
                    const float relation_factor = buffer[i];
                    const float safe_relation_factor = index == value_i ? 0.0f : relation_factor;
                    typed_dst_buckets_data[value_i] += typed_src_bucket_value[index] *
                                                       safe_relation_factor;
                  }
                }
              });
        });
  });
}

static float minimal_dinstance_to(const float radius,
                                  const int distance_power,
                                  const float precision)
{
  BLI_assert(precision > 1.0f);
  /**
   * Centre of the sphere with a points inside the sphere and some of the points are the most near
   * and far to the sampler:
   *
   * 1 / (#distance + #radius) <= 1 / (#distance - #radius).
   *
   * They are equal at ~infinite distance. But with error they can be treat as equal much near
   * To approximate this use some factor (1 <= #precision <= infinite) to say how large error is
   * acceptable:
   *
   * 1 / (#distance + #radius) >= 1 / (#distance - #radius) * #precision.
   *
   * Version in arbitrary degree of the distance to each point:
   *
   * (1 / (#distance + #radius)) ^ #distance_power >= #precision * (1 / (#distance - #radius)) ^
   * #distance_power.
   * */
  const float precision_root = math::pow<float>(precision, math::rcp<float>(distance_power));
  return -((1.0f + precision_root) / (1.0f - precision_root) * radius);
}

static void cloud_radii_to_min_distance(const Span<float> src_radii,
                                        const int distance_power,
                                        const float precision,
                                        MutableSpan<float> dst_radii)
{
  parallel_transform<float, float>(src_radii, 1024 * 8, dst_radii, [&](const float radius) {
    return minimal_dinstance_to(radius, distance_power, precision);
  });
}

static std::pair<float3, float> min_packing_sphere(const Span<float3> points)
{
  const auto search_other = [&](const float3 start_point) {
    float distance = 0.0f;
    float3 position = start_point;
    for (const float3 point : points) {
      const float new_distance = math::distance(start_point, point);
      if (distance <= new_distance) {
        position = point;
        distance = new_distance;
      }
    }
    return position;
  };

  const float3 start_b = search_other(points.first());
  const float3 start_c = search_other(start_b);

  float3 centre = math::midpoint(start_b, start_c);
  float radius = math::distance(centre, start_c);
  for (const float3 point : points) {
    const float new_radius = math::distance(centre, point);
    if (new_radius <= radius) {
      continue;
    }

    centre = math::midpoint(centre + math::normalize(centre - point) * radius, point);
    radius = (radius + new_radius) * 0.5f;
  }

  return std::pair<float3, float>(centre, radius);
}

static std::pair<float3, float> concatenate_spheres(const float3 a_centre,
                                                    const float3 b_centre,
                                                    const float a_radius,
                                                    const float b_radius)
{
  const float3 segment = math::normalize(a_centre - b_centre);
  const float3 a_extremum = a_centre + segment * a_radius;
  const float3 b_extremum = b_centre - segment * b_radius;
  return {math::midpoint(a_extremum, b_extremum), math::distance(a_extremum, b_extremum) * 0.5f};
}

static void packing_spheres_exact(const OffsetIndices<int> buckets_offsets,
                                  const int total_depth,
                                  const Span<float3> src_bucket_points,
                                  MutableSpan<float3> dst_joints_centre,
                                  MutableSpan<float> dst_joints_radii)
{
  geometry::akdbh::for_each_leaf(
      buckets_offsets,
      total_depth,
      GrainSize(4096),
      [&](const IndexRange bucket_range, const int joint_index, const int /*depth_i*/) {
        const auto [centre, radius] = min_packing_sphere(src_bucket_points.slice(bucket_range));
        dst_joints_centre[joint_index] = centre;
        dst_joints_radii[joint_index] = radius;
      });

  geometry::akdbh::for_each_to_top(buckets_offsets,
                                   total_depth,
                                   GrainSize(4096),
                                   [&](const IndexRange buckets_range,
                                       const int joint_index,
                                       const int2 /*sub_joints*/,
                                       const int /*depth_i*/) {
                                     const auto [centre, radius] = min_packing_sphere(
                                         src_bucket_points.slice(buckets_range));
                                     dst_joints_centre[joint_index] = centre;
                                     dst_joints_radii[joint_index] = radius;
                                   });
}

static void packing_spheres(const OffsetIndices<int> buckets_offsets,
                            const int total_depth,
                            const Span<float3> src_bucket_points,
                            MutableSpan<float3> dst_joints_centre,
                            MutableSpan<float> dst_joints_radii)
{
  geometry::akdbh::for_each_leaf(
      buckets_offsets,
      total_depth,
      GrainSize(4096),
      [&](const IndexRange bucket_range, const int joint_index, const int /*depth_i*/) {
        const auto [centre, radius] = min_packing_sphere(src_bucket_points.slice(bucket_range));
        dst_joints_centre[joint_index] = centre;
        dst_joints_radii[joint_index] = radius;
      });

  geometry::akdbh::for_each_to_top(buckets_offsets,
                                   total_depth,
                                   GrainSize(4096),
                                   [&](const IndexRange /*buckets_range*/,
                                       const int joint_index,
                                       const int2 sub_joints,
                                       const int /*depth_i*/) {
                                     const auto [centre, radius] = concatenate_spheres(
                                         dst_joints_centre[sub_joints[0]],
                                         dst_joints_centre[sub_joints[1]],
                                         dst_joints_radii[sub_joints[0]],
                                         dst_joints_radii[sub_joints[1]]);
                                     dst_joints_centre[joint_index] = centre;
                                     dst_joints_radii[joint_index] = radius;
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
    const OffsetIndices<int> base_offsets = akdbh::fill_bucket_offsets_trivial(domain_size,
                                                                               start_indices);

    Array<int, 0> indices(domain_size);
    akdbh::from_positions(positions, base_offsets, total_depth, indices);

    Array<float3, 0> bucket_positions(domain_size);
    GArray<> bucket_values(data_type, domain_size);

    array_utils::gather(
        Span<float3>(positions), indices.as_span(), bucket_positions.as_mutable_span());
    bke::attribute_math::gather(src_values, indices.as_span(), bucket_values.as_mutable_span());

    GArray<> joints_values(data_type, total_joints);

    akdbh::mean_sums(base_offsets, total_depth, bucket_values, joints_values);
    akdbh::normalize_for_size(base_offsets, total_depth, joints_values);

    Array<float3, 0> joints_positions(total_joints);
    Array<float, 0> joints_min_radii(total_joints);
    packing_spheres(
        base_offsets, total_depth, bucket_positions, joints_positions, joints_min_radii);

    Array<float, 0> joints_min_distance(total_joints);
    cloud_radii_to_min_distance(
        joints_min_radii, distance_power_, precision_, joints_min_distance);

    GArray<> sampled_bucket_values(data_type, domain_size);
    data_type.value_initialize_n(sampled_bucket_values.data(), sampled_bucket_values.size());
    sample_mean_average(base_offsets,
                        total_depth,
                        joints_positions,
                        bucket_positions,
                        joints_min_distance,
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
