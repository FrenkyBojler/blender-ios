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

#if (0)

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

static void transpose(const Span<float3> src, Span<MutableSpan<float>> dst)
{
  BLI_assert(dst.size() == decltype(src)::value_type::type_length);
  BLI_assert(std::all_of(dst.begin(), dst.end(), [&](const MutableSpan<float> span) { return span.size() == src.size(); }));
  
  threading::parallel_for(src.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      dst[0][i] = src[i].x;
      dst[1][i] = src[i].y;
      dst[2][i] = src[i].z;
    }
  });
}

static void transpose(const Span<Span<float>> src, MutableSpan<float3> dst)
{
  BLI_assert(src.size() == decltype(dst)::value_type::type_length);
  BLI_assert(std::all_of(src.begin(), src.end(), [&](const Span<float> span) { return span.size() == dst.size(); }));

  threading::parallel_for(dst.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      dst[i].x = src[2][i];
      dst[i].y = src[1][i];
      dst[i].z = src[0][i];
    }
  });
}

static void transpose_gather(const Span<float3> src, const Span<int> indices, Span<MutableSpan<float>> dst)
{
  BLI_assert(dst.size() == decltype(src)::value_type::type_length);
  BLI_assert(std::all_of(dst.begin(), dst.end(), [&](const MutableSpan<float> span) { return span.size() == indices.size(); }));

  threading::parallel_for(indices.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      dst[0][i] = src[indices[i]].x;
      dst[1][i] = src[indices[i]].y;
      dst[2][i] = src[indices[i]].z;
    }
  });
}

static void transpose_gather(const Span<Span<float>> src, const Span<int> indices, MutableSpan<float3> dst)
{
  BLI_assert(src.size() == decltype(dst)::value_type::type_length);
  BLI_assert(dst.size() == indices.size());

  threading::parallel_for(indices.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      dst[i].x = src[2][indices[i]];
      dst[i].y = src[1][indices[i]];
      dst[i].z = src[0][indices[i]];
    }
  });
}

static void transpose_gather(const Span<float3> src, const IndexMask mask, Span<MutableSpan<float>> dst)
{
  BLI_assert(dst.size() == decltype(src)::value_type::type_length);
  BLI_assert(std::all_of(dst.begin(), dst.end(), [&](const MutableSpan<float> span) { return span.size() == mask.size(); }));

  mask.foreach_index_optimized<int>(GrainSize(4096), [&](const int i, const int pos) {
    dst[0][pos] = src[i].x;
    dst[1][pos] = src[i].y;
    dst[2][pos] = src[i].z;
  });
}

static void transpose_gather(const Span<Span<float>> src, const IndexMask mask, MutableSpan<float3> dst)
{
  BLI_assert(src.size() == decltype(dst)::value_type::type_length);
  BLI_assert(dst.size() == mask.size());

  mask.foreach_index_optimized<int>(GrainSize(4096), [&](const int i, const int pos) {
    dst[pos].x = src[0][i];
    dst[pos].y = src[1][i];
    dst[pos].z = src[2][i];
  });
}

static void transpose_scatter(const Span<float3> src, const Span<int> indices, Span<MutableSpan<float>> dst)
{
  BLI_assert(dst.size() == decltype(src)::value_type::type_length);
  BLI_assert(src.size() == indices.size());

  threading::parallel_for(indices.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      dst[0][indices[i]] = src[i].x;
      dst[1][indices[i]] = src[i].y;
      dst[2][indices[i]] = src[i].z;
    }
  });
}

static void transpose_scatter(const Span<Span<float>> src, const Span<int> indices, MutableSpan<float3> dst)
{
  BLI_assert(src.size() == decltype(dst)::value_type::type_length);
  BLI_assert(std::all_of(src.begin(), src.end(), [&](const Span<float> span) { return span.size() == indices.size(); }));

  threading::parallel_for(indices.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      dst[indices[i]].x = src[0][i];
      dst[indices[i]].y = src[1][i];
      dst[indices[i]].z = src[2][i];
    }
  });
}

static void transpose_scatter(const Span<float3> src, const IndexMask mask, Span<MutableSpan<float>> dst)
{
  BLI_assert(dst.size() == decltype(src)::value_type::type_length);
  BLI_assert(src.size() == mask.size());

  mask.foreach_index_optimized<int>(GrainSize(4096), [&](const int i, const int pos) {
    dst[0][i] = src[pos].x;
    dst[1][i] = src[pos].y;
    dst[2][i] = src[pos].z;
  });
}

static void transpose_scatter(const Span<Span<float>> src, const IndexMask mask, MutableSpan<float3> dst)
{
  BLI_assert(src.size() == decltype(dst)::value_type::type_length);
  BLI_assert(std::all_of(src.begin(), src.end(), [&](const Span<float> span) { return span.size() == mask.size(); }));

  mask.foreach_index_optimized<int>(GrainSize(4096), [&](const int i, const int pos) {
    dst[i].x = src[0][pos];
    dst[i].y = src[1][pos];
    dst[i].z = src[2][pos];
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
  {
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
    transpose_gather(Span(positions), indices.as_span(), {bucket_positions_[0].as_mutable_span(),
                                                          bucket_positions_[1].as_mutable_span(),
                                                          bucket_positions_[2].as_mutable_span()});

    BLI_assert(data_type.is<float>() || data_type.is<float3>());

    const int data_axes_count = data_type.is<float3>() ? 3 : 1;
    bucket_values_.reinitialize(data_axes_count);
    for (const int axis_i : IndexRange(data_axes_count)) {
      bucket_values_[axis_i].reinitialize(domain_size);
    }

    if (data_type.is<float>()) {
      array_utils::gather<float>(src_values.as_span().typed<float>(), indices.as_span(), bucket_values_.first().as_mutable_span());
    } else {
      transpose_gather(src_values.as_span().typed<float3>(), indices.as_span(), {bucket_values_[0].as_mutable_span(),
                                                                                 bucket_values_[1].as_mutable_span(),
                                                                                 bucket_values_[2].as_mutable_span()});
    }

    joints_values_.reinitialize(data_axes_count);
    for (const int axis_i : IndexRange(data_axes_count)) {
      joints_values_[axis_i].reinitialize(domain_size);
      akdbh::mean_sums(base_offsets, total_depth_, bucket_values_[axis_i].as_span(), joints_values_[axis_i].as_mutable_span());
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
                                joints_min_distance_.as_mutable_span());
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArraySpan<float3> positions = params.readonly_single_input<float3>(0, "Position");
    GMutableSpan results = params.uninitialized_single_output(1, "Value");

    const CPPType &data_type = results.type();
    BLI_assert(data_type.is<float>() || data_type.is<float3>());
    const int data_axes_count = data_type.is<float3>() ? 3 : 1;
    

    std::array<Array<float, 0>, 3> position_components;

    transpose_gather(positions.as_span(), mask, {position_components[0].as_mutable_span(),
                                    position_components[1].as_mutable_span(),
                                    position_components[2].as_mutable_span()});

    BLI_assert(joints_values_.size() == data_axes_count);
    Array<Array<float, 0>, 3> masked_results(data_axes_count);

    for (const int axis_i : IndexRange(data_axes_count)) {
      masked_results[axis_i].reinitialize();
      masked_results[axis_i].as_mutable_span().fill(0.0f);
    }

    using namespace blender::geometry;
    fmm::akdbh_accumulate_in(OffsetIndices<int>(offset_indices_),
                             total_depth_,
                             joints_min_distance_,
                             joints_positions_,
                             joints_values_,
                             bucket_positions_,
                             bucket_values_,
                             power_value_,
                             offset_value_,
                             task_positions,
                             masked_results,
                             std::nullopt);

    if (data_type.is<float>()) {
      array_utils::scatter<float>(masked_results.first().as_span(), mask, results.typed<float>());
    } else {
      transpose_scatter({masked_results[0].as_span(),
                         masked_results[1].as_span(),
                         masked_results[2].as_span()},
                         mask,
                         results.typed<float3>());
    }
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

#endif

static void node_register()
{
  // static blender::bke::bNodeType ntype;
  // 
  // geo_node_type_base(&ntype, "GeometryNodeSampleInSpace");
  // ntype.nclass = NODE_CLASS_CONVERTER;
  // ntype.ui_name = "Sample in Space";
  // ntype.geometry_node_execute = node_geo_exec;
  // ntype.initfunc = node_init;
  // ntype.declare = node_declare;
  // ntype.draw_buttons = node_layout;
  // blender::bke::node_register_type(ntype);
  // 
  // node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sample_in_space_cc
