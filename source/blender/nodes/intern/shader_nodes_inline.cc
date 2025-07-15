/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_compute_context_cache.hh"
#include "BKE_lib_id.hh"
#include "BLI_stack.hh"
#include "NOD_node_in_compute_context.hh"
#include "NOD_shader_nodes_inline.hh"
#include "NOD_socket_interface_key.hh"
#include <variant>

namespace blender::nodes {

bool inline_shader_node_tree(const bNodeTree &src_tree, bNodeTree &dst_tree)
{
  src_tree.ensure_topology_cache();
  if (src_tree.has_available_link_cycle()) {
    return false;
  }

  struct BundleSocketValue;
  struct PrimitiveSocketValue {
    std::variant<int, float, bool, ColorGeometry4f, float3> value;
  };
  struct SocketValue {
    std::variant<bNodeSocket *, PrimitiveSocketValue, std::shared_ptr<BundleSocketValue>> value;
  };
  struct BundleSocketValue {
    Map<SocketInterfaceKey, SocketValue> items;
  };

  bke::ComputeContextCache compute_context_cache;
  const Vector<SocketInContext> final_output_sockets = {} /* TODO */;

  Map<SocketInContext, SocketValue> value_map;
  Set<SocketInContext> scheduled_sockets_set;
  Stack<SocketInContext> scheduled_sockets_stack;

  auto schedule_socket = [&](const SocketInContext &socket) {
    if (scheduled_sockets_set.add(socket)) {
      scheduled_sockets_stack.push(socket);
    }
  };

  for (const SocketInContext socket : final_output_sockets) {
    schedule_socket(socket);
  }

  while (!scheduled_sockets_stack.is_empty()) {
    const SocketInContext socket = scheduled_sockets_stack.peek();
    const int old_stack_size = scheduled_sockets_stack.size();
    if (socket->is_input()) {
      /* TODO */
    }
    else {
      /* TODO */
    }

    if (scheduled_sockets_stack.size() == old_stack_size) {
      BLI_assert(socket == scheduled_sockets_stack.peek());
      scheduled_sockets_stack.pop();
    }
  }
  return true;
}

}  // namespace blender::nodes
