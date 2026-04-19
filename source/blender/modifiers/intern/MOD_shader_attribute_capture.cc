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
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_rotation_types.hh"
#include "BLI_math_rotation.hh"
#include "BLI_string_ref.hh"
#include "BLI_generic_pointer.hh"

#include "BKE_lib_id.hh"
#include "BKE_object.hh"
#include "BKE_attribute.hh"
#include "BKE_geometry_set.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_compute_contexts.hh"
#include "BKE_idprop.hh"
#include "BKE_node_runtime.hh"

#include "FN_field.hh"
#include "FN_lazy_function.hh"
#include "FN_lazy_function_execute.hh"

#include "DNA_object_types.h"
#include "DNA_node_types.h"
#include "DNA_mesh_types.h"
#include "DNA_collection_types.h"
#include "DNA_mask_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_sound_types.h"
#include "DNA_text_types.h"
#include "DNA_vfont_types.h"

#include "NOD_geometry_nodes_lazy_function.hh"
#include "NOD_geometry_nodes_execute.hh"
#include "NOD_geometry_nodes_srna.hh"
#include "NOD_geometry.hh"
#include "NOD_dependencies.hh"
#include "NOD_menu_value.hh"

#include "GEO_foreach_geometry.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_nodes.hh"

#include "DEG_depsgraph_query.hh"

#include "RNA_prototypes.hh"
#include "RNA_access.hh"

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

template<typename T>
[[nodiscard]] static std::optional<bke::SocketValueVariant> load_attribute_field_input(
    PointerRNA &input_props_ptr)
{
  const std::string attribute_name = RNA_string_get(&input_props_ptr, "attribute_name");
  if (!bke::allow_procedural_attribute_access(attribute_name)) {
    return std::nullopt;
  }
  return bke::SocketValueVariant::From(bke::AttributeFieldInput::from<T>(attribute_name));
}

template<typename T>
static bke::SocketValueVariant load_data_block_input(const nodes::GeoNodesCallData *call_data,
                                                     PointerRNA &input_props_ptr)
{
  PropertyRNA &prop = *RNA_struct_find_property(&input_props_ptr, "value");
  if (RNA_property_type(&prop) == PROP_STRING) {
    if (!call_data) {
      return bke::SocketValueVariant::From(static_cast<T *>(nullptr));
    }
    BLI_assert(call_data->operator_data);
    const std::string name = RNA_string_get(&input_props_ptr, "value");
    const ID *id_orig = call_data->operator_data->input_ids->lookup_default(name, nullptr);
    if (!id_orig) {
      return bke::SocketValueVariant::From(static_cast<T *>(nullptr));
    }
    const ID *id_eval = call_data->operator_data->depsgraphs->get_evaluated_id(*id_orig);
    return bke::SocketValueVariant::From(id_cast<T *>(const_cast<ID *>(id_eval)));
  }

  BLI_assert(RNA_property_type(&prop) == PROP_POINTER);
  T *data_block = id_cast<T *>(RNA_pointer_get(&input_props_ptr, "value").owner_id);
  return bke::SocketValueVariant::From(data_block);
}

static bke::SocketValueVariant init_socket_cpp_value(const nodes::GeoNodesCallData *call_data,
                                                     PointerRNA *input_props_ptr,
                                                     const bNodeTreeInterfaceSocket &io_socket)
{
  const bke::bNodeSocketType *stype = io_socket.socket_typeinfo();
  const eNodeSocketDatatype socket_type = stype->type;
  switch (socket_type) {
    case SOCK_FLOAT: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        const float value = RNA_float_get(input_props_ptr, "value");
        return bke::SocketValueVariant(value);
      }
      if (type == nodes::GeometryNodesInputType::Attribute) {
        if (std::optional<bke::SocketValueVariant> value = load_attribute_field_input<float>(
                *input_props_ptr))
        {
          return std::move(*value);
        }
      }
      break;
    }
    case SOCK_VECTOR: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        float3 value;
        RNA_float_get_array(input_props_ptr, "value", value);
        return bke::SocketValueVariant(value);
      }
      if (type == nodes::GeometryNodesInputType::Attribute) {
        if (std::optional<bke::SocketValueVariant> value = load_attribute_field_input<float3>(
                *input_props_ptr))
        {
          return std::move(*value);
        }
      }
      break;
    }
    case SOCK_RGBA: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        ColorGeometry4f value;
        RNA_float_get_array(input_props_ptr, "value", value);
        return bke::SocketValueVariant(value);
      }
      if (type == nodes::GeometryNodesInputType::Attribute) {
        if (std::optional<bke::SocketValueVariant> value =
                load_attribute_field_input<ColorGeometry4f>(*input_props_ptr))
        {
          return std::move(*value);
        }
      }
      break;
    }
    case SOCK_BOOLEAN: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        const bool value = RNA_boolean_get(input_props_ptr, "value");
        return bke::SocketValueVariant(value);
      }
      if (type == nodes::GeometryNodesInputType::Attribute) {
        if (std::optional<bke::SocketValueVariant> value = load_attribute_field_input<bool>(
                *input_props_ptr))
        {
          return std::move(*value);
        }
      }
      if (type == nodes::GeometryNodesInputType::Layer) {
        const std::string layer_name = RNA_string_get(input_props_ptr, "layer_name");
        return bke::SocketValueVariant::From(
            fn::GField::from_input<bke::NamedLayerSelectionFieldInput>(layer_name));
      }
      break;
    }
    case SOCK_INT: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        const int value = RNA_int_get(input_props_ptr, "value");
        return bke::SocketValueVariant(value);
      }
      if (type == nodes::GeometryNodesInputType::Attribute) {
        if (std::optional<bke::SocketValueVariant> value = load_attribute_field_input<int>(
                *input_props_ptr))
        {
          return std::move(*value);
        }
      }
      break;
    }
    case SOCK_ROTATION: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        float3 value_euler;
        RNA_float_get_array(input_props_ptr, "value", value_euler);
        math::Quaternion value_rotation = math::to_quaternion(math::EulerXYZ(value_euler));
        return bke::SocketValueVariant(value_rotation);
      }
      if (type == nodes::GeometryNodesInputType::Attribute) {
        if (std::optional<bke::SocketValueVariant> value =
                load_attribute_field_input<math::Quaternion>(*input_props_ptr))
        {
          return std::move(*value);
        }
      }
      break;
    }
    case SOCK_MENU: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        const int value = RNA_enum_get(input_props_ptr, "value");
        return bke::SocketValueVariant::From(nodes::MenuValue(value));
      }
      break;
    }
    case SOCK_STRING: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        const std::string value = RNA_string_get(input_props_ptr, "value");
        return bke::SocketValueVariant(value);
      }
      break;
    }
    case SOCK_OBJECT: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        return load_data_block_input<Object>(call_data, *input_props_ptr);
      }
      break;
    }
    case SOCK_IMAGE: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        return load_data_block_input<Image>(call_data, *input_props_ptr);
      }
      break;
    }
    case SOCK_COLLECTION: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        return load_data_block_input<Collection>(call_data, *input_props_ptr);
      }
      break;
    }
    case SOCK_TEXTURE: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        return load_data_block_input<Tex>(call_data, *input_props_ptr);
      }
      break;
    }
    case SOCK_MATERIAL: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        return load_data_block_input<Material>(call_data, *input_props_ptr);
      }
      break;
    }
    case SOCK_FONT: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        return load_data_block_input<VFont>(call_data, *input_props_ptr);
      }
      break;
    }
    case SOCK_SCENE: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        return load_data_block_input<Scene>(call_data, *input_props_ptr);
      }
      break;
    }
    case SOCK_TEXT_ID: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        return load_data_block_input<Text>(call_data, *input_props_ptr);
      }
      break;
    }
    case SOCK_MASK: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        return load_data_block_input<Mask>(call_data, *input_props_ptr);
      }
      break;
    }
    case SOCK_SOUND: {
      const auto type = nodes::GeometryNodesInputType(RNA_enum_get(input_props_ptr, "type"));
      if (type == nodes::GeometryNodesInputType::Value) {
        return load_data_block_input<bSound>(call_data, *input_props_ptr);
      }
      break;
    }
    case SOCK_GEOMETRY:
    case SOCK_MATRIX:
    case SOCK_BUNDLE:
    case SOCK_CLOSURE:
    case SOCK_SHADER:
    case SOCK_CUSTOM:
    case SOCK_INT_VECTOR:
      break;
  }

  return *stype->geometry_nodes_default_value;
}

static void capture_named(const bNodeTree &tree,
                          const PointerRNA &properties_ptr,
                          const ModifierEvalContext *ctx,
                          const ComputeContext &base_compute_context,
                          const Span<std::string> output_names,
                          nodes::geo_eval_log::GeoNodesLog &logger,
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

  PointerRNA inputs_ptr = RNA_pointer_get(const_cast<PointerRNA *>(&properties_ptr), "inputs");

  ResourceScope scope;

  nodes::GeoNodesCallData call_data;

  call_data.eval_log = &logger;

  nodes::GeoNodesModifierData modifier_eval_data{};
  modifier_eval_data.depsgraph = ctx->depsgraph;
  modifier_eval_data.self_object = ctx->object;
  call_data.modifier_data = &modifier_eval_data;

  call_data.simulation_params = nullptr;
  call_data.bake_params = nullptr;

  nodes::GeoNodesSideEffectNodes side_effect_nodes;
  call_data.side_effect_nodes = &side_effect_nodes;

  call_data.call_depth_limit = U.geometry_nodes_stack_limit;

  /* Prepare main inputs. */
  for (const int i : tree.interface_inputs().index_range()) {
    const bNodeTreeInterfaceSocket &interface_socket = *tree.interface_inputs()[i];
    const bke::bNodeSocketType *typeinfo = interface_socket.socket_typeinfo();
    const eNodeSocketDatatype socket_type = typeinfo ? typeinfo->type : SOCK_CUSTOM;

    if (socket_type == SOCK_GEOMETRY && i == 0) {
      bke::SocketValueVariant &value = scope.construct<bke::SocketValueVariant>();
      value.set(r_geometry);
      param_inputs[function.inputs.main[0]] = &value;
      continue;
    }

    PointerRNA input_props_ptr = RNA_pointer_get(&inputs_ptr, interface_socket.identifier);
    bke::SocketValueVariant value = init_socket_cpp_value(
        &call_data, &input_props_ptr, interface_socket);
    param_inputs[function.inputs.main[i]] = &scope.construct<bke::SocketValueVariant>(
        std::move(value));
  }

  /* Prepare used-outputs inputs. */
  Array<bool> output_used_inputs(tree.interface_outputs().size(), true);
  for (const int i : tree.interface_outputs().index_range()) {
    param_inputs[function.inputs.output_usages[i]] = &output_used_inputs[i];
  }

  BLI_assert(function.inputs.references_to_propagate.geometry_outputs.size() == 0);

  /* Prepare memory for output values. */
  LinearAllocator<> &allocator = scope.allocator();
  for (const int i : IndexRange(num_outputs)) {
    const lf::Output &lf_output = lazy_function.outputs()[i];
    const CPPType &type = *lf_output.type;
    void *buffer = allocator.allocate(type);
    param_outputs[i] = {type, buffer};
  }

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
  {
    nodes::ScopedComputeContextTimer timer{lf_context};
    lazy_function.execute(lf_params, lf_context);
    
    lazy_function.destruct_storage(lf_context.storage);

    store_output_attributes(r_geometry, tree, output_names, param_outputs);
  }

  for (const int i : IndexRange(num_outputs)) {
    if (param_set_outputs[i]) {
      GMutablePointer &ptr = param_outputs[i];
      ptr.destruct();
    }
  }
}

static MultiValueMap<const bNodeTree *, const bNodeTree *> material_geometry_trees(const Span<const Material *> materials)
{
  Set<const bNodeTree *> materials_trees;
  Stack<const bNodeTree *> stack;
  for (const Material *material : materials) {
    if (material == nullptr) {
      continue;
    }
    
    const Material *original_material = id_cast<const Material *>(DEG_get_original_id(&material->id));

    materials_trees.add(original_material->nodetree);

    stack.push(original_material->nodetree);
    while (!stack.is_empty()) {
      const bNodeTree *tree = stack.pop();
      tree->ensure_topology_cache();
      for (const bNode *group_node : tree->group_nodes()) {
        if (group_node->is_muted()) {
          continue;
        }
        
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


  MultiValueMap<const bNodeTree *, const bNodeTree *> geometry_nodes;
  for (const bNodeTree *material_tree : materials_trees) {
    for (const bNode *node : material_tree->nodes_by_type("ShaderNodeGeometryAttribute"_ustr)) {

      if (node->is_muted()) {
        continue;
      }

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

      geometry_nodes.add(material_tree, geometry_tree);
    }
  }

  return geometry_nodes;
}

static void modify_geometry_set(ModifierData *md,
                                const ModifierEvalContext *ctx,
                                bke::GeometrySet *geometry_set)
{
  BLI_assert(geometry_set != nullptr);

  AttributeCaptureModifierData &modifier_data = *reinterpret_cast<AttributeCaptureModifierData *>(md);

  MultiValueMap<const bNodeTree *, const bNodeTree *> geometry_nodes;
  if (const Mesh *mesh = geometry_set->get_mesh()) {
    geometry_nodes = material_geometry_trees(Span{mesh->mat, mesh->totcol});
  }

  if (geometry_nodes.is_empty()) {
    return;
  }
        
  if (modifier_data.runtime == nullptr) {
    modifier_data.runtime = new ShaderGeometryNodeModifierRuntime();
  } else {
    modifier_data.runtime->eval_logs.clear();
  }

  bke::DataBlockComputeContext data_block_compute_context{nullptr, ctx->object->id};
  for (const auto [material_tree, geometry_trees] : geometry_nodes.items()) {
    for (const bNodeTree *geometry_tree : geometry_trees) {
      geometry_tree->ensure_interface_cache();

      const bNode *output_node = geometry_tree->group_output_node();
      const std::string error_prefix = BKE_id_name(geometry_tree->id);
      if (output_node == nullptr) {
        BKE_modifier_set_error(ctx->object, md, (error_prefix + "Node group must have a group output node").c_str());
        continue;
      }

      const nodes::GeometryNodesLazyFunctionGraphInfo *lf_graph_info =
          nodes::ensure_geometry_nodes_lazy_function_graph(*geometry_tree).get();
      if (lf_graph_info == nullptr) {
        BKE_modifier_set_error(ctx->object, md, (error_prefix + "Cannot evaluate node group").c_str());
        continue;
      }

      Array<std::string> names(geometry_tree->interface_outputs().size());

      const std::string capture_prefix = std::string("._a_capture[") + BKE_id_name(geometry_tree->id) + "]";
      const Span<const bNodeTreeInterfaceSocket *> outputs = geometry_tree->interface_outputs();
      for (const int index : outputs.index_range()) {
        names[index] = capture_prefix + "[" + outputs[index]->identifier + "]";
      }

      const auto node = [&]() -> const bNode * {
        for (const bNode *node : material_tree->nodes_by_type("ShaderNodeGeometryAttribute"_ustr)) {
          if (node->id == &geometry_tree->id) {
            return node;
          }
        }
        BLI_assert(false);
        return nullptr;
      }();

      PointerRNA node_ptr = RNA_pointer_create_discrete(const_cast<ID *>(&material_tree->id), RNA_ShaderNodeGeometryAttribute, const_cast<bNode *>(node));
      PointerRNA properties_ptr = RNA_pointer_get(&node_ptr, "properties");

      auto eval_log = std::make_unique<nodes::geo_eval_log::GeoNodesLog>();

      const bke::ImplicitCatureModifierComputeContext base_compute_context(&data_block_compute_context, md->persistent_uid);
      capture_named(*geometry_tree, properties_ptr, ctx, base_compute_context, names, *eval_log.get(), *geometry_set);
      
      modifier_data.runtime->eval_logs.add(std::make_pair(material_tree, node->identifier), std::move(eval_log));
    }
  }
}

static void find_dependencies_from_settings(const bNode &node,
                                            nodes::EvalDependencies &deps)
{
  IDP_foreach_property(node.prop, IDP_TYPE_FILTER_ID, [&](IDProperty *property) {
    if (ID *id = IDP_ID_get(property)) {
      deps.add_generic_id_full(id);
    }
  });
}

static const CustomData_MeshMasks dependency_data_mask{CD_MASK_PROP_ALL | CD_MASK_MDEFORMVERT,
                                                       CD_MASK_PROP_ALL,
                                                       CD_MASK_PROP_ALL,
                                                       CD_MASK_PROP_ALL,
                                                       CD_MASK_PROP_ALL};

static void add_collection_relation(const ModifierUpdateDepsgraphContext *ctx,
                                    Collection &collection)
{
  DEG_add_collection_geometry_relation(ctx->node, &collection, "Nodes Modifier");
  DEG_add_collection_geometry_customdata_mask(ctx->node, &collection, &dependency_data_mask);
}

static void add_object_relation(const ModifierUpdateDepsgraphContext *ctx,
                                Object &object,
                                const nodes::EvalDependencies::ObjectDependencyInfo &info)
{
  if (info.transform) {
    DEG_add_object_relation(ctx->node, &object, DEG_OB_COMP_TRANSFORM, "Nodes Modifier");
  }
  if (&object == ctx->object) {
    return;
  }
  if (info.geometry) {
    if (object.type == OB_EMPTY && object.instance_collection != nullptr) {
      add_collection_relation(ctx, *object.instance_collection);
    }
    else if (DEG_object_has_geometry_component(&object)) {
      DEG_add_object_relation(ctx->node, &object, DEG_OB_COMP_GEOMETRY, "Nodes Modifier");
      DEG_add_customdata_mask(ctx->node, &object, &dependency_data_mask);
    }
  }
  if (object.type == OB_CAMERA) {
    if (info.camera_parameters) {
      DEG_add_object_relation(ctx->node, &object, DEG_OB_COMP_PARAMETERS, "Nodes Modifier");
    }
  }
  if (object.type == OB_ARMATURE) {
    if (info.pose) {
      DEG_add_object_relation(ctx->node, &object, DEG_OB_COMP_EVAL_POSE, "Nodes Modifier");
    }
  }
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  MultiValueMap<const bNodeTree *, const bNodeTree *> geometry_nodes;
  if (ctx->object != nullptr) {
    const Mesh *mesh = BKE_object_get_original_mesh(ctx->object);
    if (mesh != nullptr) {
      geometry_nodes = material_geometry_trees(Span{mesh->mat, mesh->totcol});
    }
  }

  Set<const bNodeTree *> linked_geometry_nodes;
  for (const auto [material_tree, trees] : geometry_nodes.items()) {
    for (const bNodeTree *tree : trees) {
      if (linked_geometry_nodes.add(tree)) {        
        DEG_add_node_tree_output_relation(ctx->node, tree, "Implicit Attribute Capture");

        nodes::EvalDependencies eval_deps = nodes::gather_eval_dependencies_recursive(*tree);

        const auto node = [&]() -> const bNode * {
          for (const bNode *node : material_tree->nodes_by_type("ShaderNodeGeometryAttribute"_ustr)) {
            if (node->id == &tree->id) {
              return node;
            }
          }
          BLI_assert(false);
          return nullptr;
        }();

        /* Create dependencies to data-blocks referenced by the settings in the modifier. */
        find_dependencies_from_settings(*node, eval_deps);

        for (ID *id : eval_deps.ids.values()) {
          switch (ID_Type(GS(id->name))) {
            case ID_OB: {
              Object *object = reinterpret_cast<Object *>(id);
              add_object_relation(
                  ctx, *object, eval_deps.objects_info.lookup_default(object->id.session_uid, {}));
              break;
            }
            case ID_GR: {
              Collection *collection = reinterpret_cast<Collection *>(id);
              add_collection_relation(ctx, *collection);
              break;
            }
            case ID_IM:
            case ID_TE: {
              DEG_add_generic_id_relation(ctx->node, id, "Nodes Modifier");
              break;
            }
            case ID_VF: {
              DEG_add_vfont_relation(ctx->node, reinterpret_cast<VFont *>(id), "Nodes Modifier");
              break;
            }
            case ID_MA: {
              /* Purposefully don't add relations for materials. While there are material sockets,
               * the pointers are only passed around as handles rather than dereferenced. */
              break;
            }
            default: {
              /* Other types don't need depsgraph dependencies currently. */
              break;
            }
          }
        }

        if (eval_deps.needs_own_transform) {
          DEG_add_depends_on_transform_relation(ctx->node, "Nodes Modifier");
        }
        if (eval_deps.needs_active_camera) {
          DEG_add_scene_camera_relation(ctx->node, ctx->scene, DEG_OB_COMP_TRANSFORM, "Nodes Modifier");
        }
        /* Active camera is a scene parameter that can change, so we need a relation for that, too. */
        if (eval_deps.needs_active_camera || eval_deps.needs_scene_render_params) {
          DEG_add_scene_relation(ctx->node, ctx->scene, DEG_SCENE_COMP_PARAMETERS, "Nodes Modifier");
        }
      }
    }
  }

  // TODO: Also gather all materials of all objects in all modifiers props and in modifier node tree props.
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
    /*update_depsgraph*/ mod_shader_attribute_capture::update_depsgraph,
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
