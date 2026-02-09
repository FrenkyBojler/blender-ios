/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_instances.hh"

#include "DNA_curves_types.h"
#include "DNA_grease_pencil_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_transfer_attributes_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Geometry>("Target");
  b.add_output<decl::Geometry>("Target").align_with_previous();
  b.add_output<decl::Bool>("Success");
  b.add_input<decl::Geometry>("Source");

  b.add_input<decl::String>("Name").optional_label();
}

static bool name_matches_pattern(const StringRef name, const StringRef pattern)
{
  if (name == pattern) {
    return true;
  }
  // TODO: Support wildcards similar to Remove Attribute node.
  if (pattern.endswith("*")) {
    return name.startswith(pattern.drop_known_suffix("*"));
  }
  return false;
}

static bool transfer_attributes(const bke::AttributeAccessor &src_attributes,
                                const StringRef attribute_pattern,
                                bke::MutableAttributeAccessor &dst_attributes)
{
  Vector<std::string> attribute_names;
  src_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (name_matches_pattern(iter.name, attribute_pattern)) {
      attribute_names.append(iter.name);
    }
  });

  bool propagated_attribute = false;
  for (const StringRef attribute_name : attribute_names) {
    const bke::AttributeMetaData meta_data = *src_attributes.lookup_meta_data(attribute_name);
    const AttrDomain domain = meta_data.domain;
    const int src_domain_size = src_attributes.domain_size(domain);
    const int dst_domain_size = dst_attributes.domain_size(domain);
    if (src_domain_size != dst_domain_size) {
      continue;
    }
    const bke::GAttributeReader src_attribute = src_attributes.lookup(attribute_name);
    if (!src_attribute) {
      continue;
    }
    bke::GSpanAttributeWriter dst_attribute = dst_attributes.lookup_or_add_for_write_only_span(
        attribute_name, domain, meta_data.data_type);
    if (!dst_attribute) {
      continue;
    }
    array_utils::copy(src_attribute.varray, dst_attribute.span);
    dst_attribute.finish();
    propagated_attribute = true;
  }
  return propagated_attribute;
}

static bool transfer_attributes_between_grease_pencil_layers(const GreasePencil &src_grease_pencil,
                                                             GreasePencil &dst_grease_pencil,
                                                             const StringRef attribute_pattern)
{
  using namespace blender::bke::greasepencil;
  const int src_layer_num = src_grease_pencil.layers().size();
  const int dst_layer_num = dst_grease_pencil.layers().size();
  if (src_layer_num != dst_layer_num) {
    return false;
  }
  const int layer_num = src_layer_num;
  for (const int layer_i : IndexRange(layer_num)) {
    const Layer &src_layer = src_grease_pencil.layer(layer_i);
    const Drawing *src_drawing = src_grease_pencil.get_eval_drawing(src_layer);
    if (!src_drawing) {
      continue;
    }
    Layer &dst_layer = dst_grease_pencil.layer(layer_i);
    Drawing *dst_drawing = dst_grease_pencil.get_eval_drawing(dst_layer);
    if (!dst_drawing) {
      continue;
    }
    const bke::CurvesGeometry &src_curves = src_drawing->strokes();
    bke::CurvesGeometry &dst_curves = dst_drawing->strokes_for_write();
    const bke::AttributeAccessor src_attributes = src_curves.attributes();
    bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();
    transfer_attributes(src_attributes, attribute_pattern, dst_attributes);
  }
  return true;
}

static bool transfer_attributes_between_geometry_sets(GeometrySet &dst_geo,
                                                      const GeometrySet &src_geo,
                                                      const StringRef attribute_pattern)
{
  bool propagated_attribute = false;

  for (const bke::GeometryComponent::Type type : {bke::GeometryComponent::Type::Mesh,
                                                  bke::GeometryComponent::Type::PointCloud,
                                                  bke::GeometryComponent::Type::Curve,
                                                  bke::GeometryComponent::Type::Instance,
                                                  bke::GeometryComponent::Type::GreasePencil})
  {
    if (!dst_geo.has(type)) {
      continue;
    }
    if (!src_geo.has(type)) {
      continue;
    }
    const bke::GeometryComponent &src_component = *src_geo.get_component(type);
    GeometryComponent &dst_component = dst_geo.get_component_for_write(type);
    const bke::AttributeAccessor src_attributes = *src_component.attributes();
    bke::MutableAttributeAccessor dst_attributes = *dst_component.attributes_for_write();

    if (transfer_attributes(src_attributes, attribute_pattern, dst_attributes)) {
      propagated_attribute = true;
    }
  }
  /* Transfer attributes between grease pencil layers. */
  if (src_geo.has_grease_pencil() && dst_geo.has_grease_pencil()) {
    const GreasePencil &src_grease_pencil = *src_geo.get_grease_pencil();
    GreasePencil &dst_grease_pencil = *dst_geo.get_grease_pencil_for_write();
    if (transfer_attributes_between_grease_pencil_layers(
            src_grease_pencil, dst_grease_pencil, attribute_pattern))
    {
      propagated_attribute = true;
    }
  }

  return propagated_attribute;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet target_geo = params.extract_input<GeometrySet>("Target");
  GeometrySet source_geo = params.extract_input<GeometrySet>("Source");
  const std::string attribute_pattern = params.extract_input<std::string>("Name");
  bool success = false;
  if (!attribute_pattern.empty()) {
    success = transfer_attributes_between_geometry_sets(target_geo, source_geo, attribute_pattern);
  }
  params.set_output("Target", std::move(target_geo));
  params.set_output("Success", success);
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeTransferAttributes");
  ntype.ui_name = "Transfer Attributes";
  ntype.ui_description = "Copy attributes from one geometry to another";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_transfer_attributes_cc
