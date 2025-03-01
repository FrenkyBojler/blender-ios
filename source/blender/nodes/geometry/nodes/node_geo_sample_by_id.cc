/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sample_by_id_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry")
      .supported_type({GeometryComponent::Type::Mesh,
                       GeometryComponent::Type::PointCloud,
                       GeometryComponent::Type::Curve,
                       GeometryComponent::Type::Instance});

  b.add_input<decl::Int>("ID").implicit_field_on(implicit_field_inputs::id_or_index, {0});
  b.add_input<decl::Int>("Sample ID").supports_field();

  b.add_output<decl::Int>("Index").dependent_field({2}).description(
      "First index of sample ID in sampled geometry");
  b.add_output<decl::Bool>("Is Valid")
      .dependent_field({2})
      .description("Sample ID is exists in sempled geometry at least once");
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
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

static Map<int, int> id_to_index_map(const VArray<int> &id_varray)
{
  BLI_assert(!id_varray.is_empty());
  Map<int, int> map;

  if (const std::optional<int> id = id_varray.get_if_single()) {
    map.add_new(*id, 0);
    return map;
  }

  const IndexRange range = id_varray.index_range();
  devirtualize_varray(id_varray, [&](auto id_varray) {
    for (const int index : range) {
      map.add(id_varray[index], index);
    }
  });

  return map;
}

class SampleIDFunction : public mf::MultiFunction {
  mf::Signature signature_;
  Map<int, int> id_map_;

 public:
  SampleIDFunction(GeometrySet geometry, Field<int> id_field, const AttrDomain domain)
  {
    geometry.ensure_owns_direct_data();

    mf::SignatureBuilder builder{"Sample ID", signature_};
    builder.single_input<int>("Sample ID");
    builder.single_output<int>("Index", mf::ParamFlag::SupportsUnusedOutput);
    builder.single_output<bool>("Is Valid", mf::ParamFlag::SupportsUnusedOutput);
    this->set_signature(&signature_);

    const GeometryComponent *component = find_source_component(geometry, domain);
    if (component == nullptr) {
      throw std::runtime_error("no component to sample");
    }

    bke::GeometryFieldContext context(*component, domain);
    FieldEvaluator evaluator(context, component->attribute_domain_size(domain));
    evaluator.add(std::move(id_field));
    evaluator.evaluate();

    const VArray<int> id_varray = evaluator.get_evaluated<int>(0);
    id_map_ = id_to_index_map(id_varray);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<int> &ids = params.readonly_single_input<int>(0, "Sample ID");
    MutableSpan<int> indices = params.uninitialized_single_output_if_required<int>(1, "Index");
    MutableSpan<bool> is_valid = params.uninitialized_single_output_if_required<bool>(2,
                                                                                      "Is Valid");

    if (!indices.is_empty()) {
      devirtualize_varray(ids, [&](auto ids) {
        mask.foreach_index_optimized<int>(
            [&](const int i) { indices[i] = id_map_.lookup_default(ids[i], 0); });
      });
    }

    if (!is_valid.is_empty()) {
      devirtualize_varray(ids, [&](auto ids) {
        mask.foreach_index_optimized<int>(
            [&](const int i) { is_valid[i] = id_map_.contains(ids[i]); });
      });
    }
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Geometry");
  const AttrDomain domain = AttrDomain(params.node().custom1);

  Field<int> id_field = params.extract_input<Field<int>>("ID");
  Field<int> sample_id_field = params.extract_input<Field<int>>("Sample ID");

  std::unique_ptr<SampleIDFunction> sample_id_fn;
  try {
    sample_id_fn = std::make_unique<SampleIDFunction>(
        std::move(geometry), std::move(id_field), domain);
  }
  catch (const std::runtime_error &) {
    params.set_default_remaining_outputs();
    return;
  }

  auto sample_id_op = FieldOperation::Create(std::move(sample_id_fn),
                                             {std::move(sample_id_field)});
  params.set_output("Index", Field<int>(sample_id_op, 0));
  params.set_output("Is Valid", Field<bool>(std::move(sample_id_op), 1));
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

  geo_node_type_base(&ntype, "GeometryNodeSampleByID");
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
