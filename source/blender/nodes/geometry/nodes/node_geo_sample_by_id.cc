/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_fields.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sample_by_id_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry"_ustr)
      .supported_type({GeometryComponent::Type::Mesh,
                       GeometryComponent::Type::PointCloud,
                       GeometryComponent::Type::Curve,
                       GeometryComponent::Type::Instance});

  b.add_input<decl::Int>("ID"_ustr)
      .default_input_type(NODE_DEFAULT_INPUT_ID_INDEX_FIELD)
      .evaluated_geometry_field();
  const int id_input_index =
      b.add_input<decl::Int>("Sample ID"_ustr).structure_type(StructureType::Dynamic).index();

  b.add_output<decl::Int>("Index"_ustr)
      .inferred_structure_type({id_input_index})
      .propagate_references({id_input_index})
      .description("First index of sample ID in sampled geometry");
  b.add_output<decl::Bool>("Is Valid"_ustr)
      .inferred_structure_type({id_input_index})
      .propagate_references({id_input_index})
      .description("Sample ID is exists in sempled geometry at least once");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int(AttrDomain::Point);
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

static Map<int, int> id_to_index_map(const VArray<int> &ids)
{
  BLI_assert(!ids.is_empty());
  Map<int, int> map;

  if (const std::optional<int> id = ids.get_if_single()) {
    map.add_new(*id, 0);
    return map;
  }

  const IndexRange range = ids.index_range();
  devirtualize_varray(ids, [&](auto ids) {
    for (const int index : range) {
      map.add(ids[index], index);
    }
  });

  return map;
}

class SampleIDFunction : public mf::MultiFunction {
  Map<int, int> id_map_;

 public:
  SampleIDFunction(const VArray<int> &source_ids)
  {
    static auto signature = []() -> mf::Signature {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Sample by ID", signature};
      builder.single_input<int>("Sample ID");
      builder.single_output<int>("Index", mf::ParamFlag::SupportsUnusedOutput);
      builder.single_output<bool>("Is Valid", mf::ParamFlag::SupportsUnusedOutput);
      return signature;
    }();
    this->set_signature(&signature);

    id_map_ = id_to_index_map(source_ids);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const final
  {
    const VArray<int> &ids = params.readonly_single_input<int>(0, "Sample ID");
    MutableSpan<int> indices = params.uninitialized_single_output_if_required<int>(1, "Index");
    MutableSpan<bool> is_valid = params.uninitialized_single_output_if_required<bool>(2,
                                                                                      "Is Valid");

    if (!indices.is_empty() && !is_valid.is_empty()) {
      devirtualize_varray(ids, [&](auto ids) {
        mask.foreach_index_optimized<int>([&](const int i) {
          const int *index = id_map_.lookup_ptr(ids[i]);
          is_valid[i] = index != nullptr;
          indices[i] = index ? *index : 0;
        });
      });
    }
    else if (!indices.is_empty()) {
      devirtualize_varray(ids, [&](auto ids) {
        mask.foreach_index_optimized<int>(
            [&](const int i) { indices[i] = id_map_.lookup_default(ids[i], 0); });
      });
    }
    else if (!is_valid.is_empty()) {
      devirtualize_varray(ids, [&](auto ids) {
        mask.foreach_index_optimized<int>(
            [&](const int i) { is_valid[i] = id_map_.contains(ids[i]); });
      });
    }
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const GeometrySet geometry = params.extract_input<GeometrySet>("Geometry"_ustr);
  const AttrDomain domain = AttrDomain(params.node().custom1);

  const GeometryComponent *component = find_source_component(geometry, domain);
  if (component == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  bke::GeometryFieldContext context(*component, domain);
  FieldEvaluator evaluator(context, component->attribute_domain_size(domain));
  evaluator.add(params.extract_input<Field<int>>("ID"_ustr));
  evaluator.evaluate();

  const VArray<int> ids_varray = evaluator.get_evaluated<int>(0);

  SocketValueVariant sample_id_variant = params.extract_input<SocketValueVariant>(
      "Sample ID"_ustr);
  if (sample_id_variant.is_single()) {
    const int sample_id = sample_id_variant.get<int>();
    if (ids_varray.is_single()) {
      params.set_output("Index"_ustr, 0);
      params.set_output("Valid"_ustr, ids_varray.get_internal_single() == sample_id);
      return;
    }

    VArraySpan<int> ids_span(ids_varray);
    const int index = ids_span.first_index_try(sample_id);
    params.set_output("Index"_ustr, std::max(0, index));
    params.set_output("Valid"_ustr, index != -1);
    return;
  }

  auto sampling_fn = std::make_shared<SampleIDFunction>(ids_varray);

  bke::SocketValueVariant index_output_value;
  bke::SocketValueVariant valid_output_value;
  std::string error_message;
  if (!execute_multi_function_on_value_variant(sampling_fn,
                                               {&sample_id_variant},
                                               {&index_output_value, &valid_output_value},
                                               params.user_data(),
                                               error_message))
  {
    params.set_default_remaining_outputs();
    params.error_message_add(NodeWarningType::Error, std::move(error_message));
    return;
  }

  params.set_output("Index"_ustr, std::move(index_output_value));
  params.set_output("Valid"_ustr, std::move(valid_output_value));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "domain",
                    "Domain",
                    "",
                    rna_enum_attribute_domain_items,
                    NOD_inline_enum_accessors(custom1),
                    int(AttrDomain::Point));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSampleByID"_ustr);
  ntype.ui_name = "Sample by ID";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sample_by_id_cc
