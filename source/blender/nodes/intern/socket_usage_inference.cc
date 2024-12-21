/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_socket_usage_inference.hh"

#include "DNA_node_types.h"

#include "BKE_node_runtime.hh"

namespace blender::nodes::socket_usage_inference {

void infer_inputs_socket_usage(const bNodeTree &tree,
                               const Span<GPointer> tree_input_values,
                               const MutableSpan<bool> r_input_usages)
{
  r_input_usages.fill(false);
  for (const bNode *node : tree.group_input_nodes()) {
    for (const int i : tree.interface_inputs().index_range()) {
      const bNodeSocket &socket = node->output_socket(i);
      r_input_usages[i] |= socket.is_logically_linked();
    }
  }
}

void infer_inputs_socket_usage(const bNodeTree &tree,
                               Span<const bNodeSocket *> input_sockets,
                               MutableSpan<bool> r_input_usages)
{
  BLI_assert(tree.interface_inputs().size() == input_sockets.size());

  AlignedBuffer<1024, 8> allocator_buffer;
  LinearAllocator<> allocator;
  allocator.provide_buffer(allocator_buffer);

  Array<GPointer> input_values(input_sockets.size());
  for (const int i : input_sockets.index_range()) {
    const bNodeSocket &socket = *input_sockets[i];
    const bke::bNodeSocketType &stype = *socket.typeinfo;
    const CPPType *base_type = stype.base_cpp_type;
    if (base_type == nullptr) {
      continue;
    }
    void *value = allocator.allocate(base_type->size(), base_type->alignment());
    stype.get_base_cpp_value(socket.default_value, value);
    input_values[i] = GPointer(base_type, value);
  }

  infer_inputs_socket_usage(tree, input_values, r_input_usages);
}

}  // namespace blender::nodes::socket_usage_inference
