/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_compute_context_cache.hh"
#include "BKE_lib_id.hh"
#include "BKE_type_conversions.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_stack.hh"
#include "NOD_multi_function.hh"
#include "NOD_node_declaration.hh"
#include "NOD_node_in_compute_context.hh"
#include "NOD_shader_nodes_inline.hh"
#include "NOD_socket_interface_key.hh"
#include <variant>

namespace blender::nodes {
namespace {

struct BundleSocketValue;
using BundleSocketValuePtr = std::shared_ptr<BundleSocketValue>;

struct FallbackValue {};

struct PrimitiveSocketValue {
  std::variant<int, float, bool, ColorGeometry4f, float3> value;

  const void *buffer() const
  {
    return std::visit([](auto &&value) -> const void * { return &value; }, value);
  }

  void *buffer()
  {
    return const_cast<void *>(const_cast<const PrimitiveSocketValue *>(this)->buffer());
  }

  static PrimitiveSocketValue from_value(const GPointer value)
  {
    const CPPType &type = *value.type();
    if (type.is<int>()) {
      return {*static_cast<const int *>(value.get())};
    }
    if (type.is<float>()) {
      return {*static_cast<const float *>(value.get())};
    }
    if (type.is<bool>()) {
      return {*static_cast<const bool *>(value.get())};
    }
    if (type.is<ColorGeometry4f>()) {
      return {*static_cast<const ColorGeometry4f *>(value.get())};
    }
    if (type.is<float3>()) {
      return {*static_cast<const float3 *>(value.get())};
    }
    BLI_assert_unreachable();
    return {};
  }
};

/** References an output socket in the generated node tree. */
struct LinkedSocketValue {
  bNode *node = nullptr;
  bNodeSocket *socket = nullptr;
};

/** References an input socket in the source node tree. */
struct InputSocketValue {
  const bNodeSocket *socket = nullptr;
};

static bool is_supported_primitive_type(const eNodeSocketDatatype type)
{
  return ELEM(type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN, SOCK_VECTOR, SOCK_RGBA);
}

struct SocketValue {
  std::variant<FallbackValue,
               LinkedSocketValue,
               InputSocketValue,
               PrimitiveSocketValue,
               BundleSocketValuePtr>
      value;

  std::optional<PrimitiveSocketValue> to_primitive(const eNodeSocketDatatype type) const
  {
    if (const auto *primitive_value = std::get_if<PrimitiveSocketValue>(&this->value)) {
      return *primitive_value;
    }
    if (const auto *input_socket_value = std::get_if<InputSocketValue>(&this->value)) {
      const bNodeSocket &socket = *input_socket_value->socket;
      BLI_assert(socket.type == type);
      if (!socket.runtime->declaration) {
        return std::nullopt;
      }
      if (socket.runtime->declaration->default_input_type != NODE_DEFAULT_INPUT_VALUE) {
        return std::nullopt;
      }
      switch (socket.typeinfo->type) {
        case SOCK_FLOAT:
          return PrimitiveSocketValue{socket.default_value_typed<bNodeSocketValueFloat>()->value};
        case SOCK_INT:
          return PrimitiveSocketValue{socket.default_value_typed<bNodeSocketValueInt>()->value};
        case SOCK_BOOLEAN:
          return PrimitiveSocketValue{
              socket.default_value_typed<bNodeSocketValueBoolean>()->value};
        case SOCK_VECTOR:
          return PrimitiveSocketValue{
              float3(socket.default_value_typed<bNodeSocketValueVector>()->value)};
        case SOCK_RGBA:
          return PrimitiveSocketValue{
              ColorGeometry4f(socket.default_value_typed<bNodeSocketValueRGBA>()->value)};
        default:
          return std::nullopt;
      }
    }
    if (std::get_if<FallbackValue>(&this->value)) {
      switch (type) {
        case SOCK_FLOAT:
          return PrimitiveSocketValue{0.0f};
        case SOCK_INT:
          return PrimitiveSocketValue{0};
        case SOCK_BOOLEAN:
          return PrimitiveSocketValue{false};
        case SOCK_VECTOR:
          return PrimitiveSocketValue{float3(0, 0, 0)};
        case SOCK_RGBA:
          return PrimitiveSocketValue{ColorGeometry4f(0, 0, 0, 1)};
        default:
          return std::nullopt;
      }
    }
    return std::nullopt;
  }
};

struct BundleSocketValue {
  Map<SocketInterfaceKey, SocketValue> items;
};

class ShaderNodesInliner {
 private:
  ResourceScope scope_;
  const bNodeTree &src_tree_;
  bNodeTree &dst_tree_;
  bke::ComputeContextCache compute_context_cache_;
  Map<SocketInContext, SocketValue> value_by_socket_;
  Stack<SocketInContext> scheduled_sockets_stack_;
  bool use_refcounting_ = false;
  const bke::DataTypeConversions &data_type_conversions_;

 public:
  ShaderNodesInliner(const bNodeTree &src_tree, bNodeTree &dst_tree)
      : src_tree_(src_tree),
        dst_tree_(dst_tree),
        data_type_conversions_(bke::get_implicit_type_conversions())
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
      value_by_socket_.add_new(socket, {InputSocketValue{socket.socket}});
      return;
    }
    const SocketInContext origin_socket = {socket.context, used_link->fromsock};
    if (const auto *value = value_by_socket_.lookup_ptr(origin_socket)) {
      SocketValue converted_value = this->handle_implicit_conversion(
          *value, *used_link->fromsock, *used_link->tosock);
      value_by_socket_.add_new(socket, converted_value);
      return;
    }
    this->schedule_socket(origin_socket);
  }

  void handle_socket_output(const SocketInContext &socket)
  {
    const NodeInContext node = socket.owner_node();
    if (node->is_reroute()) {
      const SocketInContext input_socket = {socket.context, &node->input_socket(0)};
      if (const SocketValue *value = value_by_socket_.lookup_ptr(input_socket)) {
        value_by_socket_.add_new(socket, *value);
        return;
      }
      this->schedule_socket(input_socket);
      return;
    }
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
      value_by_socket_.add_new(socket, {FallbackValue{}});
      return;
    }
    if (node->is_group()) {
      const bNodeTree *group = reinterpret_cast<const bNodeTree *>(node->id);
      if (!group || ID_MISSING(&group->id)) {
        value_by_socket_.add_new(socket, {FallbackValue{}});
        return;
      }
      group->ensure_interface_cache();
      const bNode *group_output_node = group->group_output_node();
      if (!group_output_node) {
        value_by_socket_.add_new(socket, {FallbackValue{}});
        return;
      }
      const ComputeContext &group_compute_context = compute_context_cache_.for_group_node(
          socket.context, node->identifier, &node->owner_tree());
      const SocketInContext group_output_socket_ctx = {
          &group_compute_context, &group_output_node->input_socket(socket->index())};
      if (const SocketValue *value = value_by_socket_.lookup_ptr(group_output_socket_ctx)) {
        value_by_socket_.add_new(socket, *value);
        return;
      }
      this->schedule_socket(group_output_socket_ctx);
      return;
    }
    if (node->is_group_input()) {
      if (const auto *group_node_compute_context =
              dynamic_cast<const bke::GroupNodeComputeContext *>(socket.context))
      {
        const ComputeContext *parent_compute_context = group_node_compute_context->parent();
        const bNode *group_node = group_node_compute_context->node();
        BLI_assert(group_node);
        const bNodeSocket &group_node_input = group_node->input_socket(socket->index());
        const SocketInContext group_input_socket_ctx = {parent_compute_context, &group_node_input};
        if (const SocketValue *value = value_by_socket_.lookup_ptr(group_input_socket_ctx)) {
          value_by_socket_.add_new(socket, *value);
          return;
        }
        this->schedule_socket(group_input_socket_ctx);
        return;
      }
      value_by_socket_.add_new(socket, {FallbackValue{}});
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
      if (!value->to_primitive(input_socket->typeinfo->type)) {
        all_inputs_primitive = false;
      }
    }
    if (has_missing_inputs) {
      return;
    }
    const bke::bNodeType &node_type = *node->typeinfo;
    if (node_type.build_multi_function && all_inputs_primitive) {
      bool all_outputs_can_be_primitive = true;
      for (const bNodeSocket *output_socket : node->output_sockets()) {
        if (!output_socket->is_available()) {
          continue;
        }
        if (!ELEM(output_socket->type, SOCK_FLOAT, SOCK_INT, SOCK_BOOLEAN, SOCK_VECTOR, SOCK_RGBA))
        {
          all_outputs_can_be_primitive = false;
          break;
        }
      }
      if (all_outputs_can_be_primitive) {
        NodeMultiFunctionBuilder builder{*node.node, node->owner_tree()};
        node->typeinfo->build_multi_function(builder);
        const mf::MultiFunction &fn = builder.function();
        mf::ContextBuilder context;
        IndexMask mask(1);
        mf::ParamsBuilder params{fn, &mask};

        for (const bNodeSocket *input_socket : node->input_sockets()) {
          if (!input_socket->is_available()) {
            continue;
          }
          const SocketInContext input_socket_ctx = {node.context, input_socket};
          const PrimitiveSocketValue value = *value_by_socket_.lookup(input_socket_ctx)
                                                  .to_primitive(input_socket->typeinfo->type);
          switch (input_socket->type) {
            case SOCK_FLOAT: {
              params.add_readonly_single_input_value(std::get<float>(value.value));
              break;
            }
            case SOCK_INT: {
              params.add_readonly_single_input_value(std::get<int>(value.value));
              break;
            }
            case SOCK_BOOLEAN: {
              params.add_readonly_single_input_value(std::get<bool>(value.value));
              break;
            }
            case SOCK_VECTOR: {
              params.add_readonly_single_input_value(std::get<float3>(value.value));
              break;
            }
            case SOCK_RGBA: {
              params.add_readonly_single_input_value(
                  ColorGeometry4f(std::get<ColorGeometry4f>(value.value)));
              break;
            }
            default: {
              BLI_assert_unreachable();
              break;
            }
          }
        }

        Vector<void *> output_values;
        for (const bNodeSocket *output_socket : node->output_sockets()) {
          if (!output_socket->is_available()) {
            continue;
          }
          void *value = scope_.allocate_owned(*output_socket->typeinfo->base_cpp_type);
          output_values.append(value);
          params.add_uninitialized_single_output(
              GMutableSpan(output_socket->typeinfo->base_cpp_type, value, 1));
        }

        fn.call(mask, params, context);

        int current_output_i = 0;
        for (const bNodeSocket *output_socket : node->output_sockets()) {
          if (!output_socket->is_available()) {
            continue;
          }
          const void *value = output_values[current_output_i++];
          PrimitiveSocketValue computed_value;
          switch (output_socket->type) {
            case SOCK_FLOAT: {
              computed_value = {*static_cast<const float *>(value)};
              break;
            }
            case SOCK_INT: {
              computed_value = {*static_cast<const int *>(value)};
              break;
            }
            case SOCK_BOOLEAN: {
              computed_value = {*static_cast<const bool *>(value)};
              break;
            }
            case SOCK_VECTOR: {
              computed_value = {*static_cast<const float3 *>(value)};
              break;
            }
            case SOCK_RGBA: {
              computed_value = {*static_cast<const ColorGeometry4f *>(value)};
              break;
            }
            default: {
              BLI_assert_unreachable();
              break;
            }
          }
          value_by_socket_.add_new({socket.context, output_socket}, {computed_value});
        }
        return;
      }
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
                               {LinkedSocketValue{&copied_node, &dst_output_socket}});
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
    const std::optional<PrimitiveSocketValue> src_primitive_value = src_value.to_primitive(
        from_socket.typeinfo->type);
    if (from_socket.typeinfo->base_cpp_type && to_socket.typeinfo->base_cpp_type) {
      if (data_type_conversions_.is_convertible(*from_socket.typeinfo->base_cpp_type,
                                                *to_socket.typeinfo->base_cpp_type))
      {
        const void *src_buffer = src_primitive_value->buffer();
        BUFFER_FOR_CPP_TYPE_VALUE(*to_socket.typeinfo->base_cpp_type, dst_buffer);
        data_type_conversions_.convert_to_uninitialized(*from_socket.typeinfo->base_cpp_type,
                                                        *to_socket.typeinfo->base_cpp_type,
                                                        src_buffer,
                                                        dst_buffer);
        return {PrimitiveSocketValue::from_value(
            GPointer{to_socket.typeinfo->base_cpp_type, dst_buffer})};
      }
    }

    /* TODO */
    return SocketValue{FallbackValue{}};
  }

  void set_socket_value(bNode &dst_node, bNodeSocket &dst_socket, const SocketValue &value)
  {
    if (const std::optional<PrimitiveSocketValue> primitive_value = value.to_primitive(
            dst_socket.typeinfo->type))
    {
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
    if (std::get_if<InputSocketValue>(&value.value)) {
      if (dst_socket.type == SOCK_SHADER) {
        return;
      }
      /* TODO*/
      return;
    }
    if (std::get_if<FallbackValue>(&value.value)) {
      if (dst_socket.type == SOCK_SHADER) {
        return;
      }
      /* TODO */
      return;
    }
    if (std::get_if<BundleSocketValuePtr>(&value.value)) {
      /* This type can't be assigned to a socket. The bundle has to be separated first. */
      BLI_assert_unreachable();
      return;
    }
    if (const auto *src_socket_value = std::get_if<LinkedSocketValue>(&value.value)) {
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

}  // namespace

bool inline_shader_node_tree(const bNodeTree &src_tree, bNodeTree &dst_tree)
{
  ShaderNodesInliner inliner(src_tree, dst_tree);
  return inliner.do_inline();
}

}  // namespace blender::nodes
