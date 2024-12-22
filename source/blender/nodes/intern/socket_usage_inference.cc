/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <functional>

#include "NOD_geometry_nodes_execute.hh"
#include "NOD_multi_function.hh"
#include "NOD_node_in_compute_context.hh"
#include "NOD_socket_usage_inference.hh"

#include "DNA_node_types.h"

#include "BKE_compute_contexts.hh"
#include "BKE_node_runtime.hh"
#include "BKE_type_conversions.hh"

#include "BLI_stack.hh"

namespace blender::nodes::socket_usage_inference {

enum class TaskType {
  Value,
  Usage,
};

struct Task {
  TaskType type;
  SocketInContext socket;
};

static void handle_unlinked_input_value(const SocketInContext &socket,
                                        ResourceScope &scope,
                                        Map<SocketInContext, const void *> &all_socket_values)
{
  const CPPType &base_type = *socket->typeinfo->base_cpp_type;
  void *value_buffer = scope.linear_allocator().allocate(base_type.size(), base_type.alignment());
  socket->typeinfo->get_base_cpp_value(socket->default_value, value_buffer);
  all_socket_values.add_new(socket, value_buffer);
  if (!base_type.is_trivially_destructible()) {
    scope.add_destruct_call([type = &base_type, value_buffer]() { type->destruct(value_buffer); });
  }
}

static const void *convert_type_if_necessary(const void *src,
                                             ResourceScope &scope,
                                             const bNodeSocket &from_socket,
                                             const bNodeSocket &to_socket)
{
  if (!src) {
    return nullptr;
  }
  const CPPType *from_type = from_socket.typeinfo->base_cpp_type;
  const CPPType *to_type = to_socket.typeinfo->base_cpp_type;
  if (from_type == to_type) {
    return src;
  }
  if (!to_type) {
    return nullptr;
  }
  const bke::DataTypeConversions &conversions = bke::get_implicit_type_conversions();
  if (!conversions.is_convertible(*from_type, *to_type)) {
    return nullptr;
  }
  void *dst = scope.linear_allocator().allocate(to_type->size(), to_type->alignment());
  conversions.convert_to_uninitialized(*from_type, *to_type, src, dst);
  if (!to_type->is_trivially_destructible()) {
    scope.add_destruct_call([to_type, dst]() { to_type->destruct(dst); });
  }
  return dst;
}

static void handle_linked_input_value(const SocketInContext &from_socket,
                                      const SocketInContext &to_socket,
                                      Stack<Task> &tasks,
                                      ResourceScope &scope,
                                      Map<SocketInContext, const void *> &all_socket_values)
{
  const std::optional<const void *> from_value = all_socket_values.lookup_try(from_socket);
  if (!from_value.has_value()) {
    tasks.push({TaskType::Value, from_socket});
    return;
  }
  const void *converted_value = convert_type_if_necessary(
      *from_value, scope, *from_socket.socket, *to_socket.socket);
  all_socket_values.add_new(to_socket, converted_value);
}

static void handle_input_value_task(const SocketInContext &socket,
                                    Stack<Task> &tasks,
                                    ResourceScope &scope,
                                    Map<SocketInContext, const void *> &all_socket_values)
{
  if (socket->is_multi_input()) {
    /* Can't know the single value of a multi-input. */
    all_socket_values.add_new(socket, nullptr);
    return;
  }
  const bNodeLink *source_link = nullptr;
  const Span<const bNodeLink *> connected_links = socket->directly_linked_links();
  for (const bNodeLink *link : connected_links) {
    if (!link->is_used()) {
      continue;
    }
    if (link->fromnode->is_dangling_reroute()) {
      continue;
    }
    source_link = link;
    break;
  }
  if (!source_link) {
    handle_unlinked_input_value(socket, scope, all_socket_values);
    return;
  }
  handle_linked_input_value(
      {socket.context, source_link->fromsock}, socket, tasks, scope, all_socket_values);
}

static void handle_muted_node_output_value(const SocketInContext &socket,
                                           Stack<Task> &tasks,
                                           ResourceScope &scope,
                                           Map<SocketInContext, const void *> &all_socket_values)
{
  const NodeInContext node = socket.owner_node();

  SocketInContext input_socket;
  for (const bNodeLink &internal_link : node->internal_links()) {
    if (internal_link.tosock == socket.socket) {
      input_socket = SocketInContext{socket.context, internal_link.fromsock};
      break;
    }
  }
  if (!input_socket) {
    all_socket_values.add_new(socket, nullptr);
    return;
  }
  const std::optional<const void *> input_value = all_socket_values.lookup_try(input_socket);
  if (!input_value.has_value()) {
    tasks.push({TaskType::Value, input_socket});
    return;
  }
  const void *converted_value = convert_type_if_necessary(
      *input_value, scope, *input_socket.socket, *socket.socket);
  all_socket_values.add_new(socket, converted_value);
}

static void handle_multi_function_node_output_value(
    const SocketInContext &socket,
    Stack<Task> &tasks,
    ResourceScope &scope,
    Map<SocketInContext, const void *> &all_socket_values)
{
  const NodeInContext node = socket.owner_node();
  const int inputs_num = node->input_sockets().size();
  Vector<const void *> input_values(inputs_num);
  for (const int input_i : IndexRange(inputs_num)) {
    const SocketInContext input_socket = node.input_socket(input_i);
    const std::optional<const void *> input_value = all_socket_values.lookup_try(input_socket);
    if (!input_value.has_value()) {
      tasks.push({TaskType::Value, input_socket});
      return;
    }
    if (*input_value == nullptr) {
      all_socket_values.add_new(socket, nullptr);
      return;
    }
    input_values[input_i] = *input_value;
  }

  NodeMultiFunctionBuilder builder{*node.node, node->owner_tree()};
  node->typeinfo->build_multi_function(builder);
  const mf::MultiFunction &fn = builder.function();
  const IndexMask mask(1);
  mf::ParamsBuilder params{fn, &mask};
  for (const int input_i : IndexRange(inputs_num)) {
    const SocketInContext input_socket = node.input_socket(input_i);
    if (!input_socket->is_available()) {
      continue;
    }
    params.add_readonly_single_input(
        GPointer(input_socket->typeinfo->base_cpp_type, input_values[input_i]));
  }
  for (const int output_i : node->output_sockets().index_range()) {
    const SocketInContext output_socket = node.output_socket(output_i);
    if (!output_socket->is_available()) {
      continue;
    }
    const CPPType &base_type = *output_socket->typeinfo->base_cpp_type;
    void *value = scope.linear_allocator().allocate(base_type.size(), base_type.alignment());
    params.add_uninitialized_single_output(GMutableSpan(base_type, value, 1));
    all_socket_values.add_new(output_socket, value);
    if (!base_type.is_trivially_destructible()) {
      scope.add_destruct_call(
          [type = &base_type, value]() { type->destruct(const_cast<void *>(value)); });
    }
  }
  mf::ContextBuilder context;
  fn.call(mask, params, context);
}

static void handle_group_node_output_value(const SocketInContext &socket,
                                           Stack<Task> &tasks,
                                           ResourceScope &scope,
                                           Map<SocketInContext, const void *> &all_socket_values)
{
  const NodeInContext node = socket.owner_node();
  const bNodeTree *group = reinterpret_cast<const bNodeTree *>(node->id);
  if (!group || ID_MISSING(&group->id)) {
    all_socket_values.add_new(socket, nullptr);
    return;
  }
  const bNode *group_output_node = group->group_output_node();
  if (!group_output_node) {
    all_socket_values.add_new(socket, nullptr);
    return;
  }
  const ComputeContext &group_context = scope.construct<bke::GroupNodeComputeContext>(
      socket.context, *node, node->owner_tree());
  const SocketInContext socket_in_group{&group_context,
                                        &group_output_node->input_socket(socket->index())};
  const std::optional<const void *> value = all_socket_values.lookup_try(socket_in_group);
  if (!value.has_value()) {
    tasks.push({TaskType::Value, socket_in_group});
    return;
  }
  all_socket_values.add_new(socket, *value);
}

static void handle_group_input_node_value(const SocketInContext &socket,
                                          Stack<Task> &tasks,
                                          Map<SocketInContext, const void *> &all_socket_values)
{
  /* Group inputs for the root context should be initialized already. */
  BLI_assert(socket.context != nullptr);

  const bke::GroupNodeComputeContext &group_context =
      *static_cast<const bke::GroupNodeComputeContext *>(socket.context);
  const SocketInContext group_node_input{
      group_context.parent(), &group_context.caller_group_node()->input_socket(socket->index())};
  const std::optional<const void *> value = all_socket_values.lookup_try(group_node_input);
  if (!value.has_value()) {
    tasks.push({TaskType::Value, group_node_input});
    return;
  }
  all_socket_values.add_new(socket, *value);
}

static void handle_output_value_task(const SocketInContext &socket,
                                     Stack<Task> &tasks,
                                     ResourceScope &scope,
                                     Map<SocketInContext, const void *> &all_socket_values)
{
  const NodeInContext node = socket.owner_node();
  if (node->is_muted()) {
    handle_muted_node_output_value(socket, tasks, scope, all_socket_values);
    return;
  }
  switch (node->type) {
    case NODE_GROUP:
    case NODE_CUSTOM_GROUP: {
      handle_group_node_output_value(socket, tasks, scope, all_socket_values);
      return;
    }
    case NODE_GROUP_INPUT: {
      handle_group_input_node_value(socket, tasks, all_socket_values);
      return;
    }
    default: {
      if (node->typeinfo->build_multi_function) {
        handle_multi_function_node_output_value(socket, tasks, scope, all_socket_values);
        return;
      }
      break;
    }
  }
  all_socket_values.add_new(socket, nullptr);
}

static void handle_value_task(const SocketInContext &socket,
                              Stack<Task> &tasks,
                              ResourceScope &scope,
                              Map<SocketInContext, const void *> &all_socket_values)
{
  if (all_socket_values.contains(socket)) {
    return;
  }
  const CPPType *base_type = socket->typeinfo->base_cpp_type;
  if (!base_type) {
    all_socket_values.add_new(socket, nullptr);
    return;
  }
  if (socket->is_input()) {
    handle_input_value_task(socket, tasks, scope, all_socket_values);
  }
  else {
    handle_output_value_task(socket, tasks, scope, all_socket_values);
  }
}

static void handle_switch_node_input_usage(const SocketInContext &socket,
                                           Stack<Task> &tasks,
                                           Map<SocketInContext, bool> &all_socket_usages,
                                           Map<SocketInContext, const void *> &all_socket_values)
{
  const NodeInContext node = socket.owner_node();

  const SocketInContext output_socket = node.output_socket(0);
  const std::optional<bool> output_is_used = all_socket_usages.lookup_try(output_socket);
  if (!output_is_used.has_value()) {
    tasks.push({TaskType::Usage, output_socket});
    return;
  }
  if (!*output_is_used) {
    all_socket_usages.add_new(socket, false);
    return;
  }
  const SocketInContext condition_socket = node.input_socket(0);
  if (socket == condition_socket) {
    all_socket_usages.add_new(socket, true);
    return;
  }
  const std::optional<const void *> condition_value = all_socket_values.lookup_try(
      condition_socket);
  if (!condition_value.has_value()) {
    tasks.push({TaskType::Value, condition_socket});
    return;
  }
  if (*condition_value == nullptr) {
    /* Can't know the condition value, so assume it can be anything. */
    all_socket_usages.add_new(socket, true);
    return;
  }
  const bool switch_condition = *static_cast<const bool *>(*condition_value);
  const SocketInContext true_socket = node.input_socket(2);
  const bool is_used = (socket == true_socket) == switch_condition;
  all_socket_usages.add_new(socket, is_used);
}

static void handle_index_switch_node_input_usage(
    const SocketInContext &socket,
    Stack<Task> &tasks,
    Map<SocketInContext, bool> &all_socket_usages,
    Map<SocketInContext, const void *> &all_socket_values)
{
  const NodeInContext node = socket.owner_node();
  const SocketInContext output_socket = node.output_socket(0);
  const std::optional<bool> output_is_used = all_socket_usages.lookup_try(output_socket);
  if (!output_is_used.has_value()) {
    tasks.push({TaskType::Usage, output_socket});
    return;
  }
  if (!*output_is_used) {
    all_socket_usages.add_new(socket, false);
    return;
  }
  const SocketInContext index_socket = node.input_socket(0);
  if (socket == index_socket) {
    all_socket_usages.add_new(socket, true);
    return;
  }
  const std::optional<const void *> index_ptr = all_socket_values.lookup_try(index_socket);
  if (!index_ptr.has_value()) {
    tasks.push({TaskType::Value, index_socket});
    return;
  }
  if (*index_ptr == nullptr) {
    /* The index is unknown, so any input may be used. */
    all_socket_usages.add_new(socket, true);
    return;
  }
  const int index = *static_cast<const int *>(*index_ptr);
  const int item_i = socket->index() - 1;
  const bool is_used = index == item_i;
  all_socket_usages.add_new(socket, is_used);
}

static void handle_menu_switch_node_input_usage(
    const SocketInContext &socket,
    Stack<Task> &tasks,
    Map<SocketInContext, bool> &all_socket_usages,
    Map<SocketInContext, const void *> &all_socket_values)
{
  const NodeInContext node = socket.owner_node();

  const SocketInContext output_socket = node.output_socket(0);
  const std::optional<bool> output_is_used = all_socket_usages.lookup_try(output_socket);
  if (!output_is_used.has_value()) {
    tasks.push({TaskType::Usage, output_socket});
    return;
  }
  if (!*output_is_used) {
    all_socket_usages.add_new(socket, false);
    return;
  }
  const SocketInContext condition_socket = node.input_socket(0);
  if (socket == condition_socket) {
    all_socket_usages.add_new(socket, true);
    return;
  }
  const std::optional<const void *> condition_value = all_socket_values.lookup_try(
      condition_socket);
  if (!condition_value.has_value()) {
    tasks.push({TaskType::Value, condition_socket});
    return;
  }
  if (*condition_value == nullptr) {
    /* Can't know the condition value, so assume it can be anything. */
    all_socket_usages.add_new(socket, true);
    return;
  }
  const int menu_value = *static_cast<const int *>(*condition_value);

  const NodeMenuSwitch &storage = *static_cast<const NodeMenuSwitch *>(node->storage);
  /* Subtract one because the first input is the menu socket. */
  const int item_i = socket->index() - 1;
  const NodeEnumItem &item = storage.enum_definition.items_array[item_i];
  const bool is_used = menu_value == item.identifier;
  all_socket_usages.add_new(socket, is_used);
}

static void handle_group_node_input_usage(const SocketInContext &socket,
                                          Stack<Task> &tasks,
                                          ResourceScope &scope,
                                          Map<SocketInContext, bool> &all_socket_usages)
{
  const NodeInContext node = socket.owner_node();
  const bNodeTree *group = reinterpret_cast<const bNodeTree *>(node->id);
  if (!group || ID_MISSING(&group->id)) {
    all_socket_usages.add_new(socket, false);
    return;
  }
  group->ensure_topology_cache();
  const int input_i = socket->index();
  const ComputeContext &group_context = scope.construct<bke::GroupNodeComputeContext>(
      socket.context, *node, node->owner_tree());

  /* Check if we know that the socket is used.*/
  for (const bNode *group_input_node : group->group_input_nodes()) {
    const bNodeSocket &group_input_socket = group_input_node->output_socket(input_i);
    if (all_socket_usages.lookup_default({&group_context, &group_input_socket}, false)) {
      all_socket_usages.add_new(socket, true);
      return;
    }
  }

  /* Schedule next socket. */
  for (const bNode *group_input_node : group->group_input_nodes()) {
    const bNodeSocket &group_input_socket = group_input_node->output_socket(input_i);
    if (all_socket_usages.contains({&group_context, &group_input_socket})) {
      continue;
    }
    tasks.push({TaskType::Usage, {&group_context, &group_input_socket}});
    return;
  }

  all_socket_usages.add_new(socket, false);
}

static void handle_group_output_node_input_usage(const SocketInContext &socket,
                                                 Stack<Task> &tasks,
                                                 Map<SocketInContext, bool> &all_socket_usages)
{
  const int output_i = socket->index();
  if (socket.context == nullptr) {
    /* This is a final output which is always used. */
    all_socket_usages.add_new(socket, true);
    return;
  }
  const bke::GroupNodeComputeContext &group_context =
      *static_cast<const bke::GroupNodeComputeContext *>(socket.context);
  const SocketInContext group_node_output{
      socket.context->parent(), &group_context.caller_group_node()->output_socket(output_i)};
  const std::optional<bool> is_used = all_socket_usages.lookup_try(group_node_output);
  if (!is_used.has_value()) {
    tasks.push({TaskType::Usage, group_node_output});
    return;
  }
  all_socket_usages.add_new(socket, *is_used);
}

static void handle_fallback_node_input_usage(const SocketInContext &socket,
                                             Stack<Task> &tasks,
                                             Map<SocketInContext, bool> &all_socket_usages)
{
  const NodeInContext node = socket.owner_node();
  const int outputs_num = node->output_sockets().size();

  /* Check if any output of the node is used already.*/
  for (const int output_i : IndexRange(outputs_num)) {
    const SocketInContext output_socket = node.output_socket(output_i);
    if (all_socket_usages.lookup_default(output_socket, false)) {
      all_socket_usages.add_new(socket, true);
      return;
    }
  }
  /* Create a task that checks if the next output is used. */
  for (const int output_i : node->output_sockets().index_range()) {
    const SocketInContext output_socket = node.output_socket(output_i);
    if (!all_socket_usages.contains(output_socket)) {
      tasks.push({TaskType::Usage, output_socket});
      return;
    }
  }
  /* No task was added, so all of the outputs are already known to be unused. */
  all_socket_usages.add_new(socket, false);
}

static void handle_input_usage_task(const SocketInContext &socket,
                                    Stack<Task> &tasks,
                                    ResourceScope &scope,
                                    Map<SocketInContext, bool> &all_socket_usages,
                                    Map<SocketInContext, const void *> &all_socket_values)
{
  const NodeInContext node = socket.owner_node();
  switch (node->type) {
    case NODE_GROUP:
    case NODE_CUSTOM_GROUP: {
      handle_group_node_input_usage(socket, tasks, scope, all_socket_usages);
      break;
    }
    case NODE_GROUP_OUTPUT: {
      handle_group_output_node_input_usage(socket, tasks, all_socket_usages);
      break;
    }
    case GEO_NODE_SWITCH: {
      handle_switch_node_input_usage(socket, tasks, all_socket_usages, all_socket_values);
      break;
    }
    case GEO_NODE_INDEX_SWITCH: {
      handle_index_switch_node_input_usage(socket, tasks, all_socket_usages, all_socket_values);
      break;
    }
    case GEO_NODE_MENU_SWITCH: {
      handle_menu_switch_node_input_usage(socket, tasks, all_socket_usages, all_socket_values);
      break;
    }
    default: {
      handle_fallback_node_input_usage(socket, tasks, all_socket_usages);
      break;
    }
  }
}

static void handle_output_usage_task(const SocketInContext &socket,
                                     Stack<Task> &tasks,
                                     Map<SocketInContext, bool> &all_socket_usages)
{
  for (const bNodeLink *link : socket->directly_linked_links()) {
    if (!link->is_used()) {
      continue;
    }
    const SocketInContext target_socket = {socket.context, link->tosock};
    if (all_socket_usages.lookup_default(target_socket, false)) {
      all_socket_usages.add_new(socket, true);
      return;
    }
  }
  /* Create task that checks if the next target is used. */
  for (const bNodeLink *link : socket->directly_linked_links()) {
    if (!link->is_used()) {
      continue;
    }
    const SocketInContext target_socket = {socket.context, link->tosock};
    if (!all_socket_usages.contains(target_socket)) {
      tasks.push({TaskType::Usage, target_socket});
      return;
    }
  }
  /* No task was added, so all of the targets are already known to be unused. */
  all_socket_usages.add_new(socket, false);
}

static void handle_usage_task(const SocketInContext &socket,
                              Stack<Task> &tasks,
                              ResourceScope &scope,
                              Map<SocketInContext, bool> &all_socket_usages,
                              Map<SocketInContext, const void *> &all_socket_values)
{
  if (all_socket_usages.contains(socket)) {
    return;
  }
  if (socket->is_input()) {
    handle_input_usage_task(socket, tasks, scope, all_socket_usages, all_socket_values);
  }
  else {
    handle_output_usage_task(socket, tasks, all_socket_usages);
  }
}

void infer_inputs_socket_usage(const bNodeTree &tree,
                               const Span<GPointer> tree_input_values,
                               const MutableSpan<bool> r_input_usages)
{
  tree.ensure_topology_cache();

  AlignedBuffer<1024, 8> scope_buffer;
  ResourceScope scope;
  scope.linear_allocator().provide_buffer(scope_buffer);

  Map<SocketInContext, bool> all_socket_usages;
  Map<SocketInContext, const void *> all_socket_values;

  Stack<Task> tasks;

  for (const bNode *node : tree.group_input_nodes()) {
    for (const int i : tree.interface_inputs().index_range()) {
      const bNodeSocket &socket = node->output_socket(i);
      tasks.push({TaskType::Usage, {nullptr, &socket}});
      all_socket_values.add_new({nullptr, &socket}, tree_input_values[i].get());
    }
  }

  while (!tasks.is_empty()) {
    const Task &task = tasks.peek();
    const int prev_tasks_num = tasks.size();

    switch (task.type) {
      case TaskType::Value: {
        handle_value_task(task.socket, tasks, scope, all_socket_values);
        break;
      }
      case TaskType::Usage: {
        handle_usage_task(task.socket, tasks, scope, all_socket_usages, all_socket_values);
        break;
      }
    }

    if (tasks.size() == prev_tasks_num) {
      tasks.pop();
    }
  }

  r_input_usages.fill(false);
  for (const bNode *node : tree.group_input_nodes()) {
    for (const int i : tree.interface_inputs().index_range()) {
      const bNodeSocket &socket = node->output_socket(i);
      r_input_usages[i] |= all_socket_usages.lookup({nullptr, &socket});
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
    if (socket.is_directly_linked()) {
      continue;
    }

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

  for (GPointer &value : input_values) {
    if (const void *data = value.get()) {
      value.type()->destruct(const_cast<void *>(data));
    }
  }
}

void infer_inputs_socket_usage(const bNodeTree &tree,
                               const IDProperty *properties,
                               MutableSpan<bool> r_input_usages)
{
  const int inputs_num = tree.interface_inputs().size();
  Array<GPointer> input_values(inputs_num);
  ResourceScope scope;
  nodes::get_geometry_nodes_input_base_values(tree, properties, scope, input_values);
  nodes::socket_usage_inference::infer_inputs_socket_usage(tree, input_values, r_input_usages);
}

}  // namespace blender::nodes::socket_usage_inference
