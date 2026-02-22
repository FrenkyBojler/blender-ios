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
  b.add_input<decl::Int>("Target ID")
      .implicit_field(NODE_DEFAULT_INPUT_ID_INDEX_FIELD)
      .structure_type(StructureType::Field);
  b.add_input<decl::Geometry>("Source");
  b.add_input<decl::Int>("Source ID")
      .implicit_field(NODE_DEFAULT_INPUT_ID_INDEX_FIELD)
      .structure_type(StructureType::Field);

  b.add_input<decl::String>("Names")
      .optional_label()
      .structure_type(StructureType::List)
      .description("List of attribute names to transfer. A wildcard (*) at the end is allowed");

  b.get_anonymous_attribute_relations().propagate_relations.append({0, 0});
}

class AttributeTransferer {
 private:
  ResourceScope scope_;
  IndexMaskMemory mask_memory_;
  GeometrySet &dst_geo_;
  GeometrySet &src_geo_;
  const VectorSet<std::string> &attribute_patterns_;
  const Field<int> &dst_id_field_;
  const Field<int> &src_id_field_;
  bool any_transferred_ = false;

 public:
  AttributeTransferer(GeometrySet &dst_geo,
                      GeometrySet &src_geo,
                      const VectorSet<std::string> &attribute_patterns,
                      const Field<int> &dst_id_field,
                      const Field<int> &src_id_field)
      : dst_geo_(dst_geo),
        src_geo_(src_geo),
        attribute_patterns_(attribute_patterns),
        dst_id_field_(dst_id_field),
        src_id_field_(src_id_field)
  {
  }

  bool do_transfer()
  {
    for (const GeometryComponent::Type type : {bke::GeometryComponent::Type::Mesh,
                                               bke::GeometryComponent::Type::PointCloud,
                                               bke::GeometryComponent::Type::Curve,
                                               bke::GeometryComponent::Type::Instance,
                                               bke::GeometryComponent::Type::GreasePencil})
    {
      if (!dst_geo_.has(type)) {
        continue;
      }
      if (!src_geo_.has(type)) {
        continue;
      }
      const GeometryComponent &src_component = *src_geo_.get_component(type);
      GeometryComponent &dst_component = dst_geo_.get_component_for_write(type);
      const bke::AttributeAccessor src_attributes = *src_component.attributes();
      bke::MutableAttributeAccessor dst_attributes = *dst_component.attributes_for_write();
      this->transfer_attributes(
          src_attributes,
          dst_attributes,
          [&](const AttrDomain domain) -> fn::FieldContext & {
            return scope_.construct<bke::GeometryFieldContext>(src_component, domain);
          },
          [&](const AttrDomain domain) -> fn::FieldContext & {
            return scope_.construct<bke::GeometryFieldContext>(dst_component, domain);
          });
    }

    if (src_geo_.has_grease_pencil() && dst_geo_.has_grease_pencil()) {
      const GreasePencil &src_grease_pencil = *src_geo_.get_grease_pencil();
      GreasePencil &dst_grease_pencil = *dst_geo_.get_grease_pencil_for_write();
      this->transfer_attributes_between_grease_pencil_layers(src_grease_pencil, dst_grease_pencil);
    }

    return any_transferred_;
  }

  void transfer_attributes_between_grease_pencil_layers(const GreasePencil &src_grease_pencil,
                                                        GreasePencil &dst_grease_pencil)
  {
    using namespace blender::bke::greasepencil;
    const int src_layer_num = src_grease_pencil.layers().size();
    const int dst_layer_num = dst_grease_pencil.layers().size();
    /* Could also support custom mapping of src to dst layers. */
    const int common_layer_num = std::min(src_layer_num, dst_layer_num);
    for (const int layer_i : IndexRange(common_layer_num)) {
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
      this->transfer_attributes(
          src_attributes,
          dst_attributes,
          [&](const AttrDomain domain) -> fn::FieldContext & {
            return scope_.construct<bke::GreasePencilLayerFieldContext>(
                src_grease_pencil, domain, layer_i);
          },
          [&](const AttrDomain domain) -> fn::FieldContext & {
            return scope_.construct<bke::GreasePencilLayerFieldContext>(
                dst_grease_pencil, domain, layer_i);
          });
    }
  }

 private:
  void transfer_attributes(
      const bke::AttributeAccessor &src_attributes,
      bke::MutableAttributeAccessor &dst_attributes,
      FunctionRef<fn::FieldContext &(const AttrDomain domain)> create_src_field_context,
      FunctionRef<fn::FieldContext &(const AttrDomain domain)> create_dst_field_context)
  {
    struct AttrItem {
      std::string name;
      AttrDomain domain;
      bke::AttrType type;
    };
    Vector<AttrItem> items;
    src_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
      if (this->name_matches_any_pattern(iter.name)) {
        items.append({iter.name, iter.domain, iter.data_type});
      }
    });

    struct IDs {
      bool transfer_by_index = false;
      Array<int> src_by_dst_index;
      IndexMask dst_mask;
    };
    Array<std::optional<IDs>> ids_by_domain(ATTR_DOMAIN_NUM);
    for (const AttrItem &item : items) {
      std::optional<IDs> &ids = ids_by_domain[int(item.domain)];
      if (ids.has_value()) {
        continue;
      }
      ids.emplace();

      if (dynamic_cast<const fn::IndexFieldInput *>(&src_id_field_.node()) &&
          dynamic_cast<const fn::IndexFieldInput *>(&dst_id_field_.node()))
      {
        ids->transfer_by_index = true;
        continue;
      }

      const int src_size = src_attributes.domain_size(item.domain);
      const int dst_size = dst_attributes.domain_size(item.domain);

      fn::FieldContext &src_field_context = create_src_field_context(item.domain);
      fn::FieldEvaluator &src_evaluator = scope_.construct<fn::FieldEvaluator>(src_field_context,
                                                                               src_size);
      src_evaluator.add(src_id_field_);
      src_evaluator.evaluate();
      const VArraySpan<int> src_ids = src_evaluator.get_evaluated<int>(0);

      fn::FieldContext &dst_field_context = create_dst_field_context(item.domain);
      fn::FieldEvaluator &dst_evaluator = scope_.construct<fn::FieldEvaluator>(dst_field_context,
                                                                               dst_size);
      dst_evaluator.add(dst_id_field_);
      dst_evaluator.evaluate();
      const VArraySpan<int> dst_ids = dst_evaluator.get_evaluated<int>(0);

      Map<int, int> src_index_by_id;
      for (const int i : IndexRange(src_size)) {
        const int id = src_ids[i];
        src_index_by_id.add(id, i);
      }
      ids->src_by_dst_index.reinitialize(dst_size);
      Array<bool> dst_mask_bools(dst_size);
      threading::parallel_for(IndexRange(dst_size), 2048, [&](const IndexRange range) {
        for (const int dst_i : range) {
          const int dst_id = dst_ids[dst_i];
          const int src_i = src_index_by_id.lookup_default(dst_id, -1);
          ids->src_by_dst_index[dst_i] = src_i;
          dst_mask_bools[dst_i] = src_i != -1;
        }
      });
      ids->dst_mask = IndexMask::from_bools(dst_mask_bools, mask_memory_);
    }

    for (const AttrItem &item : items) {
      const GVArraySpan src_attr = *src_attributes.lookup(item.name);
      bke::GSpanAttributeWriter dst_attr = dst_attributes.lookup_or_add_for_write_span(
          item.name, item.domain, item.type);
      if (!dst_attr) {
        continue;
      }
      const int src_size = src_attr.size();
      const int dst_size = dst_attr.span.size();

      const IDs &ids = *ids_by_domain[int(item.domain)];
      const CPPType &cpp_type = src_attr.type();
      if (ids.transfer_by_index) {
        const int copy_num = std::min(src_size, dst_size);
        const IndexRange slice(copy_num);
        cpp_type.copy_assign_n(src_attr.data(), dst_attr.span.data(), copy_num);
      }
      else {
        bke::attribute_math::to_static_type(cpp_type, [&]<typename T>() {
          const Span<T> src_span = src_attr.typed<T>();
          const MutableSpan<T> dst_span = dst_attr.span.typed<T>();
          /* Should probably use some array_utils::gather overload, but the right one doesn't seem
           * to exist yet. */
          ids.dst_mask.foreach_index([&](const int dst_i) {
            const int src_i = ids.src_by_dst_index[dst_i];
            dst_span[dst_i] = src_span[src_i];
          });
        });
      }
      dst_attr.finish();
      any_transferred_ = true;
    }
  }

  bool name_matches_any_pattern(const StringRef name) const
  {
    if (attribute_patterns_.contains_as(name)) {
      return true;
    }
    for (const StringRef pattern : attribute_patterns_) {
      // TODO: Support wildcards similar to Remove Attribute node.
      if (pattern.endswith("*")) {
        if (name.startswith(pattern.drop_known_suffix("*"))) {
          return true;
        }
      }
    }
    return false;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet target_geo = params.extract_input<GeometrySet>("Target");
  GeometrySet source_geo = params.extract_input<GeometrySet>("Source");
  const ListPtr attribute_patterns_list = params.extract_input<ListPtr>("Names");
  const Field<int> target_id = params.extract_input<Field<int>>("Target ID");
  const Field<int> source_id = params.extract_input<Field<int>>("Source ID");

  VectorSet<std::string> attribute_patterns;
  if (attribute_patterns_list) {
    const CPPType &cpp_type = attribute_patterns_list->cpp_type();
    if (cpp_type.is<std::string>()) {
      attribute_patterns_list->foreach<std::string>(
          [&](const StringRef pattern) { attribute_patterns.add_as(pattern); });
    }
  }

  bool success = false;
  if (!attribute_patterns.is_empty()) {
    AttributeTransferer transferer(
        target_geo, source_geo, attribute_patterns, target_id, source_id);
    success = transferer.do_transfer();
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
