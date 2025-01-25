/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

// debug includes

#include <limits>
#include <type_traits>

#include "DNA_pointcloud_types.h"

#include "BKE_geometry_fields.hh"
#include "BKE_geometry_set.hh"
#include "BKE_instances.hh"

#include "GEO_mesh_primitive_uv_sphere.hh"
#include "GEO_transform.hh"

#include "BLI_math_quaternion_types.hh"
#include "BLI_rand.hh"
#include "BLI_timeit.hh"

// debug includes

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_binary_search.hh"
#include "BLI_function_ref.hh"
#include "BLI_generic_span.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_base.hh"
#include "BLI_math_bits.h"
#include "BLI_sort.hh"
#include "BLI_task.hh"
#include "BLI_task_size_hints.hh"
#include "BLI_virtual_array.hh"

#include "BLI_map.hh"

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

namespace blender::nodes::node_geo_sample_in_space_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  b.add_input<decl::Geometry>("Geometry");
  b.add_input<decl::Vector>("Position").implicit_field_on_all(implicit_field_inputs::position);

  if (node != nullptr) {
    const eCustomDataType data_type = eCustomDataType(node->custom1);
    b.add_input(data_type, "Value").field_on_all().hide_value();
  }

  b.add_input<decl::Vector>("Sample Position")
      .supports_field()
      .implicit_field(implicit_field_inputs::position);

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
  node->custom1 = CD_PROP_FLOAT;
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
  uiItemR(layout, ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
}

#if (0)

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
  akdbt::for_each_leaf(
      buckets_offsets,
      total_depth,
      GrainSize(4096),
      [&](const IndexRange bucket_range, const int joint_index, const int /*depth_i*/) {
        const auto [centre, radius] = min_packing_sphere(src_bucket_points.slice(bucket_range));
        dst_joints_centre[joint_index] = centre;
        dst_joints_radii[joint_index] = radius;
      });

  akdbt::for_each_to_top(buckets_offsets,
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
  akdbt::for_each_leaf(
      buckets_offsets,
      total_depth,
      GrainSize(4096),
      [&](const IndexRange bucket_range, const int joint_index, const int /*depth_i*/) {
        const auto [centre, radius] = min_packing_sphere(src_bucket_points.slice(bucket_range));
        dst_joints_centre[joint_index] = centre;
        dst_joints_radii[joint_index] = radius;
      });

  akdbt::for_each_to_top(buckets_offsets,
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

class GradientSumFunction : public mf::MultiFunction {
 private:
  int power_value_;
  float offset_value_;

  mf::Signature signature_;

  int total_depth;
  int total_buckets;
  int total_joints;

  Array<int> start_indices;
  Array<int> indices;
  Array<float3> bucket_positions;
  Array<float3> bucket_values;
  Array<float3> joints_values;
  Array<float> joints_values_factors;
  Array<float3> joints_positions;
  Array<float> joints_min_radii;
  Array<float> joints_min_distance;

 public:
  GradientSumFunction(const GeometrySet &geometry_set,
                      const Field<float3> position_field,
                      const Field<float3> value_field,
                      const float precision,
                      const int power_value,
                      const float offset_value)
      : power_value_(power_value), offset_value_(offset_value)
  {
    mf::SignatureBuilder builder{"Gradient Sum", signature_};
    builder.single_input<float3>("Position");
    builder.single_output<float3>("Value");
    this->set_signature(&signature_);

    const PointCloud *point_cloud = geometry_set.get_pointcloud();
    if (point_cloud == nullptr) {
      return;
    }

    const int domain_size = point_cloud->totpoint;
    const bke::PointCloudFieldContext context(*point_cloud);
    fn::FieldEvaluator evaluator{context, domain_size};
    evaluator.add(position_field);
    evaluator.add(value_field);
    evaluator.evaluate();
    const VArraySpan<float3> positions = evaluator.get_evaluated<float3>(0);
    const VArraySpan<float3> src_values = evaluator.get_evaluated<float3>(1);

    total_depth = akdbt::total_depth_from_total(positions.size());
    total_buckets = akdbt::total_buckets_for(total_depth);
    total_joints = akdbt::total_joints_for_depth(total_depth);

    start_indices.reinitialize(total_buckets + 1);
    indices.reinitialize(domain_size);
    bucket_positions.reinitialize(domain_size);
    bucket_values.reinitialize(domain_size);
    joints_values.reinitialize(total_joints);
    joints_values_factors.reinitialize(total_joints);
    joints_positions.reinitialize(total_joints);
    joints_min_radii.reinitialize(total_joints);
    joints_min_distance.reinitialize(total_joints);

    akdbt::fill_buckets_linear(domain_size, start_indices);
    const OffsetIndices<int> base_offsets(start_indices);
    akdbt::from_positions(positions, base_offsets, total_depth, indices);
    array_utils::gather(
        Span<float3>(positions), indices.as_span(), bucket_positions.as_mutable_span());
    array_utils::gather(
        Span<float3>(src_values), indices.as_span(), bucket_values.as_mutable_span());
    akdbt::mean_sums<float3>(base_offsets, total_depth, bucket_values, joints_values);
    akdbt::normalize_for_size<float3>(base_offsets, total_depth, joints_values);
    akdbt::accumulate_size<float>(base_offsets, total_depth, joints_values_factors);
    packing_spheres(
        base_offsets, total_depth, bucket_positions, joints_positions, joints_min_radii);
    cloud_radii_to_min_distance(joints_min_radii, power_value, precision, joints_min_distance);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArraySpan<float3> positions = params.readonly_single_input<float3>(0, "Position");
    MutableSpan<float3> results = params.uninitialized_single_output<float3>(1, "Value");
    results.fill(float3(0.0f));

    const FunctionRef<void(int, MutableSpan<float>)> squared_distance_invertion =
        akdbt::powered_rcp_for_squared(power_value_);

    Vector<float> buffer;
    buffer.reserve(positions.size());
    akdbt::for_each_to_bottom_skip(
        OffsetIndices<int>(start_indices),
        total_depth,
        positions.index_range(),
        [&](const int joint_index, const int value_i) -> bool {
          return (math::distance(joints_positions[joint_index], positions[value_i]) +
                  offset_value_) <= joints_min_distance[joint_index];
        },
        [&](const IndexRange buckets_range, const int joint_index, const Span<int> value_indices) {
          buffer.resize(value_indices.size());
          for (const int value_i : value_indices.index_range()) {
            const int value_index = value_indices[value_i];
            buffer[value_i] = math::square(
                math::distance(joints_positions[joint_index], positions[value_index]) +
                offset_value_);
          }

          squared_distance_invertion(power_value_, buffer.as_mutable_span());

          const float total_factor = buckets_range.size();
          for (const int value_i : value_indices.index_range()) {
            const int value_index = value_indices[value_i];
            results[value_index] += math::normalize(joints_positions[joint_index] -
                                                    positions[value_index]) *
                                    joints_values[joint_index] * buffer[value_i] * total_factor;
          }
        },
        [&](const IndexRange bucket_range, const Span<int> value_indices) {
          buffer.resize(bucket_range.size());
          for (const int value_i : value_indices) {
            const float3 position = positions[value_i];

            for (const int index : bucket_range.index_range()) {
              buffer[index] = math::square(
                  math::distance(bucket_positions[bucket_range[index]], position) + offset_value_);
            }

            squared_distance_invertion(power_value_, buffer.as_mutable_span());

            // const float3 self_value = bucket_values[value_i];
            for (const int i : bucket_range.index_range()) {
              results[value_i] += math::normalize(bucket_positions[bucket_range[i]] - position) *
                                  bucket_values[bucket_range[i]] * buffer[i];
            }
          }
        });
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<bke::GeometrySet>("Domain");
  Field<float3> position_field = params.extract_input<Field<float3>>("Position");
  GField value_field = params.extract_input<GField>("Value");

  Field<float3> sample_position_field = params.extract_input<Field<float3>>("Sample Position");

  const int power_value = params.extract_input<int>("Power");
  const float precision_value = params.extract_input<float>("Error");
  const float offset_value = params.extract_input<float>("Offset");

  std::shared_ptr<FieldOperation> sample_op = FieldOperation::Create(
      std::make_unique<GradientSumFunction>(geometry,
                                            std::move(position_field),
                                            std::move(value_field),
                                            precision_value,
                                            power_value,
                                            offset_value),
      {std::move(sample_position_field)});

  params.set_output("Sample Gradient", GField(sample_op, 0));
}

#endif

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
  // ntype.geometry_node_execute = node_geo_exec;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(&ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sample_in_space_cc
