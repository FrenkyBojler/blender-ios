/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_compute_context_cache.hh"
#include "BKE_lib_id.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_stack.hh"
#include "NOD_multi_function.hh"
#include "NOD_node_in_compute_context.hh"
#include "NOD_shader_nodes_inline.hh"
#include "NOD_socket_interface_key.hh"
#include <variant>

namespace blender::nodes {

struct BundleSocketValue;
using BundleSocketValuePtr = std::shared_ptr<BundleSocketValue>;

struct EmptySocketValue {};

struct PrimitiveSocketValue {
  std::variant<int, float, bool, ColorGeometry4f, float3> value;
};

struct SourceSocketValue {
  bNode *node = nullptr;
  bNodeSocket *socket = nullptr;
};

struct SocketValue {
  std::variant<EmptySocketValue, SourceSocketValue, PrimitiveSocketValue, BundleSocketValuePtr>
      value;

  bool is_primitive() const
  {
    return std::get_if<PrimitiveSocketValue>(&value) != nullptr;
  }
};

struct BundleSocketValue {
  Map<SocketInterfaceKey, SocketValue> items;
};

class ShaderNodesInliner {
 private:
  const bNodeTree &src_tree_;
  bNodeTree &dst_tree_;
  bke::ComputeContextCache compute_context_cache_;
  Map<SocketInContext, SocketValue> value_by_socket_;
  Stack<SocketInContext> scheduled_sockets_stack_;
  bool use_refcounting_ = false;

 public:
  ShaderNodesInliner(const bNodeTree &src_tree, bNodeTree &dst_tree)
      : src_tree_(src_tree), dst_tree_(dst_tree)
  {
    if (dst_tree.id.tag & ID_TAG_NO_MAIN) {
      BLI_assert(src_tree.id.tag & ID_TAG_NO_MAIN);
    }
    use_refcounting_ = !(dst_tree.id.tag & ID_TAG_NO_MAIN);
  }

  bool do_inline()
  {
    src_tree_.ensure_topology_cache();
    if (src_tree_.has_available_link_cycle()) {
      return false;
    }
    const Vector<SocketInContext> final_output_sockets = this->find_final_output_sockets();
    for (const SocketInContext &socket : final_output_sockets) {
      this->schedule_socket(socket);
    }

    while (!scheduled_sockets_stack_.is_empty()) {
      const SocketInContext socket = scheduled_sockets_stack_.peek();
      const int old_stack_size = scheduled_sockets_stack_.size();

      this->handle_socket(socket);

      if (scheduled_sockets_stack_.size() == old_stack_size) {
        /* No initial dependencies were pushed, so this socket is fully handled and can be popped
         * from the stack. */
        BLI_assert(socket == scheduled_sockets_stack_.peek());
        scheduled_sockets_stack_.pop();
      }
    }

    /* Create actual output nodes. */
    Map<NodeInContext, bNode *> final_output_nodes;
    for (const SocketInContext &socket : final_output_sockets) {
      const NodeInContext src_node = socket.owner_node();
      bNode *copied_node = final_output_nodes.lookup_or_add_cb(src_node, [&]() {
        return bke::node_copy(&dst_tree_, *src_node.node, this->node_copy_flag(), true);
      });
      bNodeSocket *copied_socket = static_cast<bNodeSocket *>(
          BLI_findlink(&copied_node->inputs, socket.socket->index()));
      this->set_socket_value(*copied_node, *copied_socket, value_by_socket_.lookup(socket));
    }

    return true;
  }

  Vector<SocketInContext> find_final_output_sockets() const
  {
    /* TODO: Handle other output nodes and outputs within node groups. */
    Vector<SocketInContext> output_sockets;
    for (const bNode *node : src_tree_.nodes_by_type("ShaderNodeOutputMaterial")) {
      for (const bNodeSocket *socket : node->input_sockets()) {
        output_sockets.append({nullptr, socket});
      }
    }
    return output_sockets;
  }

  void handle_socket(const SocketInContext &socket)
  {
    if (!socket->is_available()) {
      return;
    }
    if (value_by_socket_.contains(socket)) {
      return;
    }
    if (socket->is_input()) {
      this->handle_socket_input(socket);
    }
    else {
      this->handle_socket_output(socket);
    }
  }

  void handle_socket_input(const SocketInContext &socket)
  {
    /* Multi-inputs are not supported in shader nodes currently. */
    BLI_assert(!socket->is_multi_input());

    const bNodeLink *used_link = nullptr;
    for (const bNodeLink *link : socket->directly_linked_links()) {
      if (!link->is_used()) {
        continue;
      }
      used_link = link;
    }
    if (!used_link) {
      this->handle_socket_input_unlinked(socket);
      return;
    }
    const SocketInContext origin_socket = {socket.context, used_link->fromsock};
    if (const auto *value = value_by_socket_.lookup_ptr(origin_socket)) {
      SocketValue converted_value = this->handle_implicit_conversion(
          *value, *used_link->fromsock, *used_link->tosock);
      value_by_socket_.add_new(socket, std::move(converted_value));
      return;
    }
    this->schedule_socket(origin_socket);
  }

  void handle_socket_input_unlinked(const SocketInContext &socket)
  {
    SocketValue value;
    switch (eNodeSocketDatatype(socket->type)) {
      case SOCK_FLOAT: {
        value.value = PrimitiveSocketValue{
            socket->default_value_typed<bNodeSocketValueFloat>()->value};
        break;
      }
      case SOCK_INT: {
        value.value = PrimitiveSocketValue{
            socket->default_value_typed<bNodeSocketValueInt>()->value};
        break;
      }
      case SOCK_BOOLEAN: {
        value.value = PrimitiveSocketValue{
            socket->default_value_typed<bNodeSocketValueBoolean>()->value};
        break;
      }
      case SOCK_VECTOR: {
        value.value = PrimitiveSocketValue{
            float3(socket->default_value_typed<bNodeSocketValueVector>()->value)};
        break;
      }
      case SOCK_RGBA: {
        value.value = PrimitiveSocketValue{
            ColorGeometry4f(socket->default_value_typed<bNodeSocketValueRGBA>()->value)};
        break;
      }
      default: {
        value.value = EmptySocketValue{};
        break;
      }
    }
    value_by_socket_.add_new(socket, std::move(value));
  }

  void handle_socket_output(const SocketInContext &socket)
  {
    const NodeInContext node = socket.owner_node();
    if (node->is_muted()) {
      for (const bNodeLink &internal_link : node->internal_links()) {
        if (internal_link.tosock == socket.socket) {
          const SocketInContext src_socket = {socket.context, internal_link.fromsock};
          if (const SocketValue *value = value_by_socket_.lookup_ptr(src_socket)) {
            value_by_socket_.add_new(socket,
                                     this->handle_implicit_conversion(
                                         *value, *internal_link.fromsock, *internal_link.tosock));
            return;
          }
          this->schedule_socket(src_socket);
          return;
        }
      }
      value_by_socket_.add_new(socket, {EmptySocketValue{}});
      return;
    }
    if (node->is_group()) {
      /* TODO */
      return;
    }
    if (node->is_group_input()) {
      /* TODO */
      return;
    }

    bool has_missing_inputs = false;
    bool all_inputs_primitive = true;
    for (const bNodeSocket *input_socket : node->input_sockets()) {
      if (!input_socket->is_available()) {
        continue;
      }
      const SocketInContext input_socket_ctx = {socket.context, input_socket};
      const SocketValue *value = value_by_socket_.lookup_ptr(input_socket_ctx);
      if (!value) {
        this->schedule_socket(input_socket_ctx);
        has_missing_inputs = true;
        continue;
      }
      if (!value->is_primitive()) {
        all_inputs_primitive = false;
      }
    }
    if (has_missing_inputs) {
      return;
    }
    const bke::bNodeType &node_type = *node->typeinfo;
    if (node_type.build_multi_function && all_inputs_primitive) {
      NodeMultiFunctionBuilder builder{*node.node, node->owner_tree()};
      node->typeinfo->build_multi_function(builder);
      const mf::MultiFunction &fn = builder.function();
      /* TODO */
      return;
    }
    Map<const bNodeSocket *, bNodeSocket *> socket_map;
    bNode &copied_node = *bke::node_copy_with_mapping(
        &dst_tree_, *node.node, this->node_copy_flag(), true, socket_map);
    for (const bNodeSocket *src_input_socket : node->input_sockets()) {
      if (!src_input_socket->is_available()) {
        continue;
      }
      bNodeSocket &dst_input_socket = *socket_map.lookup(src_input_socket);
      const SocketInContext input_socket_ctx = {socket.context, src_input_socket};
      const SocketValue &value = value_by_socket_.lookup(input_socket_ctx);
      this->set_socket_value(copied_node, dst_input_socket, value);
    }
    for (const bNodeSocket *src_output_socket : node->output_sockets()) {
      if (!src_output_socket->is_available()) {
        continue;
      }
      bNodeSocket &dst_output_socket = *socket_map.lookup(src_output_socket);
      const SocketInContext output_socket_ctx = {socket.context, src_output_socket};
      value_by_socket_.add_new(output_socket_ctx,
                               {SourceSocketValue{&copied_node, &dst_output_socket}});
    }
  }

  SocketValue handle_implicit_conversion(const SocketValue &src_value,
                                         const bNodeSocket &from_socket,
                                         const bNodeSocket &to_socket) const
  {
    const eNodeSocketDatatype from_type = eNodeSocketDatatype(from_socket.type);
    const eNodeSocketDatatype to_type = eNodeSocketDatatype(to_socket.type);
    if (from_type == to_type) {
      return src_value;
    }
    /* TODO */
    return SocketValue{EmptySocketValue{}};
  }

  void set_socket_value(bNode &dst_node, bNodeSocket &dst_socket, const SocketValue &value)
  {
    if (const auto *primitive_value = std::get_if<PrimitiveSocketValue>(&value.value)) {
      switch (dst_socket.type) {
        case SOCK_FLOAT: {
          dst_socket.default_value_typed<bNodeSocketValueFloat>()->value = std::get<float>(
              primitive_value->value);
          break;
        }
        case SOCK_INT: {
          dst_socket.default_value_typed<bNodeSocketValueInt>()->value = std::get<int>(
              primitive_value->value);
          break;
        }
        case SOCK_BOOLEAN: {
          dst_socket.default_value_typed<bNodeSocketValueBoolean>()->value = std::get<bool>(
              primitive_value->value);
          break;
        }
        case SOCK_VECTOR: {
          copy_v3_v3(dst_socket.default_value_typed<bNodeSocketValueVector>()->value,
                     std::get<float3>(primitive_value->value));
          break;
        }
        case SOCK_RGBA: {
          copy_v4_v4(dst_socket.default_value_typed<bNodeSocketValueRGBA>()->value,
                     std::get<ColorGeometry4f>(primitive_value->value));
          break;
        }
        default: {
          BLI_assert_unreachable();
          break;
        }
      }
      return;
    }
    if (std::get_if<EmptySocketValue>(&value.value)) {
      /* TODO */
      return;
    }
    if (std::get_if<BundleSocketValuePtr>(&value.value)) {
      /* This type can't be assigned to a socket. The bundle has to be separated first. */
      BLI_assert_unreachable();
      return;
    }
    if (const auto *src_socket_value = std::get_if<SourceSocketValue>(&value.value)) {
      bke::node_add_link(
          dst_tree_, *src_socket_value->node, *src_socket_value->socket, dst_node, dst_socket);
      return;
    }
    BLI_assert_unreachable();
  }

  void schedule_socket(const SocketInContext &socket)
  {
    scheduled_sockets_stack_.push(socket);
  }

  int node_copy_flag() const
  {
    return use_refcounting_ ? 0 : LIB_ID_CREATE_NO_USER_REFCOUNT;
  }
};

bool inline_shader_node_tree(const bNodeTree &src_tree, bNodeTree &dst_tree)
{
  ShaderNodesInliner inliner(src_tree, dst_tree);
  return inliner.do_inline();
}

}  // namespace blender::nodes
