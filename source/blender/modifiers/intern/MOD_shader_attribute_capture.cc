/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include "BLI_array.hh"
#include "BLI_vector.hh"
#include "BLI_stack.hh"
#include "BLI_multi_value_map.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_generic_pointer.hh"

#include "BKE_lib_id.hh"
#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_compute_contexts.hh"
#include "BKE_node_runtime.hh"

#include "FN_field.hh"
#include "FN_lazy_function.hh"
#include "FN_lazy_function_execute.hh"

#include "DNA_object_types.h"
#include "DNA_node_types.h"
#include "DNA_mesh_types.h"
#include "DNA_material_types.h"

#include "NOD_geometry_nodes_lazy_function.hh"
#include "NOD_geometry_nodes_execute.hh"
#include "NOD_geometry.hh"

#include "GEO_foreach_geometry.hh"

#include "MOD_modifiertypes.hh"

#include "DEG_depsgraph_query.hh"

#include "RNA_prototypes.hh"

#include "UI_resources.hh"

#include "BLT_translation.hh"

namespace blender::mod_shader_attribute_capture {

struct OutputAttributeInfo {
  fn::GField field;
  std::string name;
};

struct OutputAttributeToStore {
  bke::GeometryComponent::Type component_type;
  bke::AttrDomain domain;
  std::string name;
  GMutableSpan data;
};

/**
 * The output attributes are organized based on their domain, because attributes on the same domain
 * can be evaluated together.
 */
static MultiValueMap<bke::AttrDomain, OutputAttributeInfo> find_output_attributes_to_store(
    const bNodeTree &tree, const Span<std::string> capture_named, Span<GMutablePointer> output_values)
{
  const bNode &output_node = *tree.group_output_node();
  MultiValueMap<bke::AttrDomain, OutputAttributeInfo> outputs_by_domain;
  const Span<const bNodeSocket *> outputs = output_node.input_sockets().drop_back(1);
  for (const int socket_i : outputs.index_range()) {
    const bNodeSocket &socket = *outputs[socket_i];
    BLI_assert(nodes::socket_type_has_attribute_toggle(eNodeSocketDatatype(socket.type)));

    const std::string &attribute_name = capture_named[socket_i];
    BLI_assert(!attribute_name.empty());
    BLI_assert(bke::allow_procedural_attribute_access(attribute_name));

    const bke::SocketValueVariant &value_variant = *output_values[socket_i].get<bke::SocketValueVariant>();
    const fn::GField field = value_variant.get<fn::GField>();

    const bNodeTreeInterfaceSocket &interface_socket = *tree.interface_outputs()[socket_i];
    const bke::AttrDomain domain = bke::AttrDomain(interface_socket.attribute_domain);
    OutputAttributeInfo output_info{std::move(field), attribute_name};
    outputs_by_domain.add(domain, std::move(output_info));
  }
  return outputs_by_domain;
}

/**
 * The computed values are stored in newly allocated arrays. They still have to be moved to the
 * actual geometry.
 */
static Vector<OutputAttributeToStore> compute_attributes_to_store(
    const bke::GeometrySet &geometry,
    const MultiValueMap<bke::AttrDomain, OutputAttributeInfo> &outputs_by_domain,
    const Span<const bke::GeometryComponent::Type> component_types)
{
  Vector<OutputAttributeToStore> attributes_to_store;
  for (const auto component_type : component_types) {
    if (!geometry.has(component_type)) {
      continue;
    }
    const bke::GeometryComponent &component = *geometry.get_component(component_type);
    const bke::AttributeAccessor attributes = *component.attributes();
    for (const auto item : outputs_by_domain.items()) {
      const bke::AttrDomain domain = item.key;
      const Span<OutputAttributeInfo> outputs_info = item.value;
      if (!attributes.domain_supported(domain)) {
        continue;
      }
      const int domain_size = attributes.domain_size(domain);
      bke::GeometryFieldContext field_context{component, domain};
      fn::FieldEvaluator field_evaluator{field_context, domain_size};
      for (const OutputAttributeInfo &output_info : outputs_info) {
        const CPPType &type = output_info.field.cpp_type();
        const bke::AttributeValidator validator = attributes.lookup_validator(output_info.name);

        OutputAttributeToStore store{
            component_type,
            domain,
            output_info.name,
            GMutableSpan{
                type,
                MEM_new_uninitialized_aligned(type.size * domain_size, type.alignment, __func__),
                domain_size}};
        fn::GField field = validator.validate_field_if_necessary(output_info.field);
        field_evaluator.add_with_destination(std::move(field), store.data);
        attributes_to_store.append(store);
      }
      field_evaluator.evaluate();
    }
  }
  return attributes_to_store;
}

static void store_computed_output_attributes(
    bke::GeometrySet &geometry, const Span<OutputAttributeToStore> attributes_to_store)
{
  for (const OutputAttributeToStore &store : attributes_to_store) {
    bke::GeometryComponent &component = geometry.get_component_for_write(store.component_type);
    bke::MutableAttributeAccessor attributes = *component.attributes_for_write();

    const bke::AttrType data_type = bke::cpp_type_to_attribute_type(store.data.type());
    const std::optional<bke::AttributeMetaData> meta_data = attributes.lookup_meta_data(
        store.name);

    /* Attempt to remove the attribute if it already exists but the domain and type don't match.
     * Removing the attribute won't succeed if it is built in and non-removable. */
    if (meta_data.has_value() &&
        (meta_data->domain != store.domain || meta_data->data_type != data_type))
    {
      attributes.remove(store.name);
    }

    /* Try to create the attribute reusing the stored buffer. This will only succeed if the
     * attribute didn't exist before, or if it existed but was removed above. */
    if (attributes.add(store.name,
                       store.domain,
                       bke::cpp_type_to_attribute_type(store.data.type()),
                       bke::AttributeInitMoveArray(store.data.data())))
    {
      continue;
    }

    bke::GAttributeWriter attribute = attributes.lookup_or_add_for_write(
        store.name, store.domain, data_type);
    if (attribute) {
      attribute.varray.set_all(store.data.data());
      attribute.finish();
    }

    /* We were unable to reuse the data, so it must be destructed and freed. */
    store.data.type().destruct_n(store.data.data(), store.data.size());
    MEM_delete_void(store.data.data());
  }
}

static void store_output_attributes(bke::GeometrySet &geometry,
                                    const bNodeTree &tree,
                                    const Span<std::string> capture_named,
                                    Span<GMutablePointer> output_values)
{
  /* All new attribute values have to be computed before the geometry is actually changed. This is
   * necessary because some fields might depend on attributes that are overwritten. */
  MultiValueMap<bke::AttrDomain, OutputAttributeInfo> outputs_by_domain =
      find_output_attributes_to_store(tree, capture_named, output_values);
  if (outputs_by_domain.size() == 0) {
    return;
  }

  geometry::foreach_real_geometry(geometry, [&](bke::GeometrySet &instance_geometry) {
    /* Instance attributes should only be created for the top-level geometry. */
    Vector<OutputAttributeToStore> attributes_to_store = compute_attributes_to_store(
        instance_geometry,
        outputs_by_domain,
        {bke::GeometryComponent::Type::Mesh,
         bke::GeometryComponent::Type::PointCloud,
         bke::GeometryComponent::Type::Curve});
    store_computed_output_attributes(instance_geometry, attributes_to_store);
  });
}


static void capture_named(const bNodeTree &tree,
                          const ComputeContext &base_compute_context,
                          const Span<std::string> output_names,
                          bke::GeometrySet &r_geometry)
{
  BLI_assert(output_names.size() == tree.interface_outputs().size());
  const nodes::GeometryNodesLazyFunctionGraphInfo &lf_graph_info = *nodes::ensure_geometry_nodes_lazy_function_graph(tree);
  const nodes::GeometryNodesGroupFunction &function = lf_graph_info.function;
  const lf::LazyFunction &lazy_function = *function.function;
  const int num_inputs = lazy_function.inputs().size();
  const int num_outputs = lazy_function.outputs().size();

  Array<GMutablePointer> param_inputs(num_inputs);
  Array<GMutablePointer> param_outputs(num_outputs);
  Array<std::optional<lf::ValueUsage>> param_input_usages(num_inputs);
  Array<lf::ValueUsage> param_output_usages(num_outputs);
  Array<bool> param_set_outputs(num_outputs, false);

  /* We want to evaluate the main outputs, but don't care about which inputs are used for now. */
  param_output_usages.as_mutable_span().slice(function.outputs.main).fill(lf::ValueUsage::Used);
  param_output_usages.as_mutable_span()
      .slice(function.outputs.input_usages)
      .fill(lf::ValueUsage::Unused);

  tree.ensure_interface_cache();

  BLI_assert(tree.interface_inputs().size() == 0);

  /* Prepare used-outputs inputs. */
  Array<bool> output_used_inputs(tree.interface_outputs().size(), true);
  for (const int i : tree.interface_outputs().index_range()) {
    param_inputs[function.inputs.output_usages[i]] = &output_used_inputs[i];
  }

  BLI_assert(function.inputs.references_to_propagate.geometry_outputs.size() == 0);

  /* Prepare memory for output values. */
  ResourceScope scope;
  LinearAllocator<> &allocator = scope.allocator();
  for (const int i : IndexRange(num_outputs)) {
    const lf::Output &lf_output = lazy_function.outputs()[i];
    const CPPType &type = *lf_output.type;
    void *buffer = allocator.allocate(type);
    param_outputs[i] = {type, buffer};
  }

  nodes::GeoNodesCallData call_data;
  call_data.modifier_data = nullptr;
  call_data.simulation_params = nullptr;
  call_data.bake_params = nullptr;

  nodes::GeoNodesSideEffectNodes side_effect_nodes;
  call_data.side_effect_nodes = &side_effect_nodes;

  call_data.call_depth_limit = U.geometry_nodes_stack_limit;

  nodes::GeoNodesUserData user_data;
  user_data.call_data = &call_data;
  call_data.root_ntree = &tree;
  user_data.compute_context = &base_compute_context;

  nodes::GeoNodesLocalUserData local_user_data(user_data);

  lf::Context lf_context(lazy_function.init_storage(allocator), &user_data, &local_user_data);
  lf::BasicParams lf_params{lazy_function,
                            param_inputs,
                            param_outputs,
                            param_input_usages,
                            param_output_usages,
                            param_set_outputs};
  lazy_function.execute(lf_params, lf_context);
  lazy_function.destruct_storage(lf_context.storage);

  BLI_assert(param_outputs.size() == output_names.size());

  store_output_attributes(r_geometry, tree, output_names, param_outputs);

  for (const int i : IndexRange(num_outputs)) {
    if (param_set_outputs[i]) {
      GMutablePointer &ptr = param_outputs[i];
      ptr.destruct();
    }
  }
}

static void modify_geometry_set(ModifierData *md,
                                const ModifierEvalContext *ctx,
                                bke::GeometrySet *geometry_set)
{
  BLI_assert(geometry_set != nullptr);
  
  Set<const bNodeTree *> materials_trees;
  if (const Mesh *mesh = geometry_set->get_mesh()) {
    Stack<const bNodeTree *> stack;
    for (const Material *material : Span{mesh->mat, mesh->totcol}) {
      if (material == nullptr) {
        continue;
      }
      
      materials_trees.add(material->nodetree);
      
      stack.push(material->nodetree);
      while (!stack.is_empty()) {
        const bNodeTree *tree = stack.pop();
        tree->ensure_topology_cache();
        for (const bNode *group_node : tree->group_nodes()) {
          const bNodeTree *other_tree = id_cast<const bNodeTree *>(group_node->id);
          if (other_tree == nullptr) {
            continue;
          }

          if (ID_IS_LINKED(other_tree)) {
            if (ID_MISSING(other_tree)) {
              continue;
            }
            /* Currently the missing flag is only set on original data. */
            if (const ID *orig_group = DEG_get_original_id(&other_tree->id)) {
              if (ID_MISSING(orig_group)) {
                continue;
              }
            }
          }
          
          if (!materials_trees.add(other_tree)) {
            continue;
          }
          
          stack.push(other_tree);
        }
      }
    }
  }

  Set<const bNodeTree *> geometry_nodes;
  for (const bNodeTree *material_tree : materials_trees) {
    material_tree->ensure_topology_cache();
    for (const bNode *node : material_tree->nodes_by_type("ShaderNodeGeometryAttribute"_ustr)) {
      const bNodeTree *geometry_tree = id_cast<const bNodeTree *>(node->id);
      if (geometry_tree == nullptr) {
        continue;
      }
      
      if (ID_IS_LINKED(geometry_tree)) {
        if (ID_MISSING(geometry_tree)) {
          continue;
        }
        /* Currently the missing flag is only set on original data. */
        if (const ID *orig_group = DEG_get_original_id(&geometry_tree->id)) {
          if (ID_MISSING(orig_group)) {
            continue;
          }
        }
      }
      
      geometry_nodes.add(geometry_tree);
    }
  }

  if (geometry_nodes.is_empty()) {
    return;
  }

  bke::DataBlockComputeContext data_block_compute_context{nullptr, ctx->object->id};
  for (const bNodeTree *tree : geometry_nodes) {
    tree->ensure_interface_cache();
    Array<std::string> names(tree->interface_outputs().size(), "AAA");

    const std::string capture_prefix = std::string(".capture[") + BKE_id_name(tree->id) + "]";
    const Span<const bNodeTreeInterfaceSocket *> outputs = tree->interface_outputs();
    for (const int index : outputs.index_range()) {
      names[index] = capture_prefix + "." + outputs[index]->identifier;
    }

    const bke::ImplicitCatureModifierComputeContext base_compute_context(&data_block_compute_context, md->persistent_uid);
    capture_named(*tree, base_compute_context, names, *geometry_set);
  }
}

}

namespace blender {

ModifierTypeInfo modifierType_CaptureShaderAttribute = {
    /*idname*/ "AttributeCapture",
    /*name*/ N_("AttributeCapture"),
    /*struct_name*/ "AttributeCaptureModifierData",
    /*struct_size*/ sizeof(AttributeCaptureModifierData),
    /*srna*/ &RNA_Modifier,
    /*type*/ ModifierTypeType::NonGeometrical,
    /*flags*/ eModifierTypeFlag_AcceptsCVs | eModifierTypeFlag_AcceptsVertexCosOnly |
        eModifierTypeFlag_SupportsEditmode,
    /*icon*/ ICON_DOT,

    /*copy_data*/ nullptr,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ nullptr,
    /*modify_geometry_set*/ mod_shader_attribute_capture::modify_geometry_set,

    /*init_data*/ nullptr,
    /*required_data_mask*/ nullptr,
    /*free_data*/ nullptr,
    /*is_disabled*/ nullptr,
    /*update_depsgraph*/ nullptr,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ nullptr,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ nullptr,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender::mod_shader_attribute_capture
