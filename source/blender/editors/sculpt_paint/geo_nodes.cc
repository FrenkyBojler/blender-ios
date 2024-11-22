/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_execute.hh"
#include "NOD_geometry_nodes_lazy_function.hh"
#include "NOD_node_declaration.hh"
#include "NOD_socket.hh"

#include "BKE_compute_contexts.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_geometry_set.hh"
#include "BKE_idprop.hh"
#include "BKE_node_enum.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_socket_value.hh"

#include "FN_lazy_function_execute.hh"

#include "editors/sculpt_paint/sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

  void sculpting_geo_nodes_execute(StrokeCache &cache, MutableSpan<float3> translations)
  {
    const bNodeTree &tree = *cache.node_tree;

    const nodes::GeometryNodesLazyFunctionGraphInfo& lf_graph_info =
      *nodes::ensure_geometry_nodes_lazy_function_graph(tree);
    const nodes::GeometryNodesGroupFunction& function = lf_graph_info.function;
    const lf::LazyFunction& lazy_function = *function.function;
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

    nodes::GeoNodesCallData call_data;
    call_data.root_ntree = &tree;
    call_data.side_effect_nodes = {};

    bke::SculptingComputeContext compute_context;

    nodes::GeoNodesLFUserData user_data;
    user_data.call_data = &call_data;
    user_data.compute_context = &compute_context;

    LinearAllocator<> allocator;
    Vector<GMutablePointer> inputs_to_destruct;

    tree.ensure_interface_cache();

    /* Prepare main inputs. */
    for (const int i : tree.interface_inputs().index_range()) {
      const bNodeTreeInterfaceSocket& interface_socket = *tree.interface_inputs()[i];
      const bke::bNodeSocketType* typeinfo = interface_socket.socket_typeinfo();
      const eNodeSocketDatatype socket_type = typeinfo ? eNodeSocketDatatype(typeinfo->type) :
        SOCK_CUSTOM;

      const CPPType* type = typeinfo->geometry_nodes_cpp_type;
      BLI_assert(type != nullptr);
      void* value = allocator.allocate(type->size(), type->alignment());
      //initialize_group_input(btree, properties, i, value);
      param_inputs[function.inputs.main[i]] = { type, value };
      inputs_to_destruct.append({ type, value });
    }

    /* Prepare used-outputs inputs. */
    Array<bool> output_used_inputs(tree.interface_outputs().size(), true);
    for (const int i : tree.interface_outputs().index_range()) {
      param_inputs[function.inputs.output_usages[i]] = &output_used_inputs[i];
    }

    /* No anonymous attributes have to be propagated. */
    /*Array<bke::GeometryNodesReferenceSet> references_to_propagate(
      function.inputs.references_to_propagate.geometry_outputs.size());
    for (const int i : references_to_propagate.index_range()) {
      param_inputs[function.inputs.references_to_propagate.range[i]] = &references_to_propagate[i];
    }*/

    /* Prepare memory for output values. */
    for (const int i : IndexRange(num_outputs)) {
      const lf::Output& lf_output = lazy_function.outputs()[i];
      const CPPType& type = *lf_output.type;
      void* buffer = allocator.allocate(type.size(), type.alignment());
      param_outputs[i] = { type, buffer };
    }

    nodes::GeoNodesLFLocalUserData local_user_data(user_data);

    lf::Context lf_context(lazy_function.init_storage(allocator), &user_data, &local_user_data);
    lf::BasicParams lf_params{ lazy_function,
                              param_inputs,
                              param_outputs,
                              param_input_usages,
                              param_output_usages,
                              param_set_outputs };
    {
      lazy_function.execute(lf_params, lf_context);
    }
    lazy_function.destruct_storage(lf_context.storage);

    for (GMutablePointer& ptr : inputs_to_destruct) {
      ptr.destruct();
    }

    bke::SocketValueVariant output = std::move(*param_outputs[0].get<bke::SocketValueVariant>());
    float3 translation = output.get<float3>();
    printf("%f %f %f \n", translation.x, translation.y, translation.z);
    translations.fill(translation);

    //store_output_attributes(output_geometry, btree, properties, param_outputs);

    for (const int i : IndexRange(num_outputs)) {
      if (param_set_outputs[i]) {
        GMutablePointer& ptr = param_outputs[i];
        ptr.destruct();
      }
    }
  }
}
