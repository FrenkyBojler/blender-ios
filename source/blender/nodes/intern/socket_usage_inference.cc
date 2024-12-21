/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <functional>

#include "NOD_multi_function.hh"
#include "NOD_socket_usage_inference.hh"

#include "DNA_node_types.h"

#include "BKE_node_runtime.hh"

#include "BLI_stack.hh"

namespace blender::nodes::socket_usage_inference {

enum class TaskType {
  Value,
  Usage,
};

struct Task {
  TaskType type;
  const bNodeSocket *socket = nullptr;
};

static void handle_unlinked_input_value(const bNodeSocket &socket,
                                        ResourceScope &scope,
                                        MutableSpan<std::optional<const void *>> all_socket_values)
{
  const CPPType &base_type = *socket.typeinfo->base_cpp_type;
  void *value_buffer = scope.linear_allocator().allocate(base_type.size(), base_type.alignment());
  socket.typeinfo->get_base_cpp_value(socket.default_value, value_buffer);
  all_socket_values[socket.index_in_tree()] = value_buffer;
  if (!base_type.is_trivially_destructible()) {
    scope.add_destruct_call([type = &base_type, value_buffer]() { type->destruct(value_buffer); });
  }
}

static void handle_linked_input_value(const bNodeLink &link,
                                      Stack<Task> &tasks,
                                      MutableSpan<std::optional<const void *>> all_socket_values)
{
  const bNodeSocket &socket = *link.tosock;
  const bNodeSocket &origin_socket = *link.fromsock;
  /* TODO: type conversion */
  BLI_assert(origin_socket.type == socket.type);
  const std::optional<const void *> &origin_value =
      all_socket_values[origin_socket.index_in_tree()];
  if (!origin_value.has_value()) {
    tasks.push({TaskType::Value, &origin_socket});
    return;
  }
  all_socket_values[socket.index_in_tree()] = origin_value;
}

static void handle_input_value_task(const bNodeSocket &socket,
                                    Stack<Task> &tasks,
                                    ResourceScope &scope,
                                    MutableSpan<std::optional<const void *>> all_socket_values)
{
  const int socket_tree_index = socket.index_in_tree();

  if (socket.is_multi_input()) {
    /* Can't know the single value of a multi-input. */
    all_socket_values[socket_tree_index] = nullptr;
    return;
  }
  const bNodeLink *source_link = nullptr;
  const Span<const bNodeLink *> connected_links = socket.directly_linked_links();
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
  handle_linked_input_value(*source_link, tasks, all_socket_values);
}

static void handle_muted_node_output_value(
    const bNodeSocket &socket,
    Stack<Task> &tasks,
    MutableSpan<std::optional<const void *>> all_socket_values)
{
  const bNode &node = socket.owner_node();
  const int socket_tree_index = socket.index_in_tree();

  const bNodeSocket *input_socket = nullptr;
  for (const bNodeLink &internal_link : node.internal_links()) {
    if (internal_link.tosock == &socket) {
      input_socket = internal_link.fromsock;
      break;
    }
  }
  if (!input_socket) {
    all_socket_values[socket_tree_index] = nullptr;
    return;
  }
  const std::optional<const void *> &input_value =
      all_socket_values[input_socket->index_in_tree()];
  if (!input_value.has_value()) {
    tasks.push({TaskType::Value, input_socket});
    return;
  }
  /* TODO: Handle type conversion. */
  all_socket_values[socket_tree_index] = input_value;
}

static void handle_multi_function_node_output_value(
    const bNodeTree &tree,
    const bNodeSocket &socket,
    Stack<Task> &tasks,
    ResourceScope &scope,
    MutableSpan<std::optional<const void *>> all_socket_values)
{
  const bNode &node = socket.owner_node();
  const int socket_tree_index = socket.index_in_tree();
  const int prev_tasks_num = tasks.size();

  for (const bNodeSocket *input_socket : node.input_sockets()) {
    const std::optional<const void *> &input_value =
        all_socket_values[input_socket->index_in_tree()];
    if (!input_value.has_value()) {
      tasks.push({TaskType::Value, input_socket});
      break;
    }
    if (*input_value == nullptr) {
      all_socket_values[socket_tree_index] = nullptr;
      break;
    }
  }
  if (tasks.size() > prev_tasks_num) {
    /* Waiting for input value. */
    return;
  }
  if (all_socket_values[socket_tree_index].has_value()) {
    /* Done already. */
    return;
  }

  NodeMultiFunctionBuilder builder{node, tree};
  node.typeinfo->build_multi_function(builder);
  const mf::MultiFunction &fn = builder.function();
  const IndexMask mask(1);
  mf::ParamsBuilder params{fn, &mask};
  for (const bNodeSocket *input_socket : node.input_sockets()) {
    if (!input_socket->is_available()) {
      continue;
    }
    const void *value = *all_socket_values[input_socket->index_in_tree()];
    BLI_assert(value);
    params.add_readonly_single_input(GPointer(input_socket->typeinfo->base_cpp_type, value));
  }
  for (const bNodeSocket *output_socket : node.output_sockets()) {
    if (!output_socket->is_available()) {
      continue;
    }
    const CPPType &base_type = *output_socket->typeinfo->base_cpp_type;
    void *value = scope.linear_allocator().allocate(base_type.size(), base_type.alignment());
    params.add_uninitialized_single_output(GMutableSpan(base_type, value, 1));
    all_socket_values[output_socket->index_in_tree()] = value;
    if (!base_type.is_trivially_destructible()) {
      scope.add_destruct_call(
          [type = &base_type, value]() { type->destruct(const_cast<void *>(value)); });
    }
  }
  mf::ContextBuilder context;
  fn.call(mask, params, context);
}

static void handle_output_value_task(const bNodeTree &tree,
                                     const bNodeSocket &socket,
                                     Stack<Task> &tasks,
                                     ResourceScope &scope,
                                     MutableSpan<std::optional<const void *>> all_socket_values)
{
  const bNode &node = socket.owner_node();
  const int socket_tree_index = socket.index_in_tree();

  if (node.is_muted()) {
    handle_muted_node_output_value(socket, tasks, all_socket_values);
    return;
  }
  if (node.typeinfo->build_multi_function) {
    handle_multi_function_node_output_value(tree, socket, tasks, scope, all_socket_values);
    return;
  }
  all_socket_values[socket_tree_index] = nullptr;
}

static void handle_value_task(const bNodeTree &tree,
                              const bNodeSocket &socket,
                              Stack<Task> &tasks,
                              ResourceScope &scope,
                              MutableSpan<std::optional<const void *>> all_socket_values)
{
  const int socket_tree_index = socket.index_in_tree();

  if (all_socket_values[socket_tree_index].has_value()) {
    return;
  }
  const CPPType *base_type = socket.typeinfo->base_cpp_type;
  if (!base_type) {
    all_socket_values[socket_tree_index] = nullptr;
    return;
  }
  if (socket.is_input()) {
    handle_input_value_task(socket, tasks, scope, all_socket_values);
  }
  else {
    handle_output_value_task(tree, socket, tasks, scope, all_socket_values);
  }
}

static void handle_switch_node_input_usage(
    const bNodeSocket &socket,
    Stack<Task> &tasks,
    MutableSpan<std::optional<bool>> all_socket_usages,
    MutableSpan<std::optional<const void *>> all_socket_values)
{
  const bNode &node = socket.owner_node();
  const int socket_tree_index = socket.index_in_tree();

  const bNodeSocket &output_socket = node.output_socket(0);
  const std::optional<bool> &output_usage = all_socket_usages[output_socket.index_in_tree()];
  if (!output_usage.has_value()) {
    tasks.push({TaskType::Usage, &output_socket});
    return;
  }
  if (!*output_usage) {
    all_socket_usages[socket_tree_index] = false;
    return;
  }
  const bNodeSocket &condition_socket = node.input_socket(0);
  if (&socket == &condition_socket) {
    all_socket_usages[socket_tree_index] = true;
    return;
  }
  const std::optional<const void *> &switch_condition_ptr =
      all_socket_values[condition_socket.index_in_tree()];
  if (!switch_condition_ptr.has_value()) {
    tasks.push({TaskType::Value, &condition_socket});
    return;
  }
  if (*switch_condition_ptr == nullptr) {
    /* Can't know the condition value, so assume it can be anything. */
    all_socket_usages[socket_tree_index] = true;
    return;
  }
  const bool switch_condition = *static_cast<const bool *>(*switch_condition_ptr);
  const bNodeSocket &true_socket = node.input_socket(2);
  const bool is_used = (&socket == &true_socket) == switch_condition;
  all_socket_usages[socket_tree_index] = is_used;
}

static void handle_fallback_node_input_usage(const bNodeSocket &socket,
                                             Stack<Task> &tasks,
                                             MutableSpan<std::optional<bool>> all_socket_usages)
{
  const bNode &node = socket.owner_node();
  const int socket_tree_index = socket.index_in_tree();
  const int prev_tasks_num = tasks.size();

  /* Check if any output of the node is used already.*/
  bool is_used = false;
  for (const bNodeSocket *output_socket : node.output_sockets()) {
    const std::optional<bool> &output_usage = all_socket_usages[output_socket->index_in_tree()];
    if (output_usage.has_value()) {
      if (*output_usage) {
        is_used = true;
        break;
      }
    }
  }
  if (is_used) {
    all_socket_usages[socket_tree_index] = true;
    return;
  }
  /* Create a task that checks if the next output is used. */
  for (const bNodeSocket *output_socket : node.output_sockets()) {
    const std::optional<bool> &output_usage = all_socket_usages[output_socket->index_in_tree()];
    if (!output_usage.has_value()) {
      tasks.push({TaskType::Usage, output_socket});
      return;
    }
  }
  if (tasks.size() == prev_tasks_num) {
    /* No task was added, so all of the outputs are already known to be unused. */
    all_socket_usages[socket_tree_index] = false;
  }
}

static void handle_input_usage_task(const bNodeSocket &socket,
                                    Stack<Task> &tasks,
                                    MutableSpan<std::optional<bool>> all_socket_usages,
                                    MutableSpan<std::optional<const void *>> all_socket_values)
{
  const bNode &node = socket.owner_node();
  const int socket_tree_index = socket.index_in_tree();

  if (node.output_sockets().is_empty()) {
    all_socket_usages[socket_tree_index] = true;
    return;
  }
  switch (node.type) {
    case GEO_NODE_SWITCH: {
      handle_switch_node_input_usage(socket, tasks, all_socket_usages, all_socket_values);
      break;
    }
    default: {
      handle_fallback_node_input_usage(socket, tasks, all_socket_usages);
      break;
    }
  }
}

static void handle_output_usage_task(const bNodeSocket &socket,
                                     Stack<Task> &tasks,
                                     MutableSpan<std::optional<bool>> all_socket_usages)
{
  const int socket_tree_index = socket.index_in_tree();
  const int prev_tasks_num = tasks.size();

  bool is_used = false;
  for (const bNodeLink *link : socket.directly_linked_links()) {
    if (!link->is_used()) {
      continue;
    }
    const bNodeSocket &target_socket = *link->tosock;
    const std::optional<bool> &target_usage = all_socket_usages[target_socket.index_in_tree()];
    if (target_usage.has_value()) {
      if (*target_usage) {
        is_used = true;
        break;
      }
    }
  }
  if (is_used) {
    all_socket_usages[socket_tree_index] = true;
    return;
  }
  /* Create task that checks if the next target is used. */
  for (const bNodeLink *link : socket.directly_linked_links()) {
    if (!link->is_used()) {
      continue;
    }
    const bNodeSocket &target_socket = *link->tosock;
    const std::optional<bool> &target_usage = all_socket_usages[target_socket.index_in_tree()];
    if (!target_usage.has_value()) {
      tasks.push({TaskType::Usage, &target_socket});
      return;
    }
  }
  if (tasks.size() == prev_tasks_num) {
    /* No task was added, so all of the targets are already known to be unused. */
    all_socket_usages[socket_tree_index] = false;
  }
}

static void handle_usage_task(const bNodeSocket &socket,
                              Stack<Task> &tasks,
                              MutableSpan<std::optional<bool>> all_socket_usages,
                              MutableSpan<std::optional<const void *>> all_socket_values)
{
  const int socket_tree_index = socket.index_in_tree();

  if (all_socket_usages[socket_tree_index].has_value()) {
    return;
  }
  if (socket.is_input()) {
    handle_input_usage_task(socket, tasks, all_socket_usages, all_socket_values);
  }
  else {
    handle_output_usage_task(socket, tasks, all_socket_usages);
  }
}

void infer_inputs_socket_usage(const bNodeTree &tree,
                               const Span<GPointer> tree_input_values,
                               const MutableSpan<bool> r_input_usages)
{
  AlignedBuffer<1024, 8> scope_buffer;
  ResourceScope scope;
  scope.linear_allocator().provide_buffer(scope_buffer);

  Array<std::optional<bool>> all_socket_usages(tree.all_sockets().size());
  Array<std::optional<const void *>> all_socket_values(tree.all_sockets().size());

  Stack<Task> tasks;

  for (const bNode *node : tree.group_input_nodes()) {
    for (const int i : tree.interface_inputs().index_range()) {
      const bNodeSocket &socket = node->output_socket(i);
      tasks.push({TaskType::Usage, &socket});
      all_socket_values[socket.index_in_tree()] = tree_input_values[i].get();
    }
  }

  while (!tasks.is_empty()) {
    const Task &task = tasks.peek();
    const int prev_tasks_num = tasks.size();
    const bNodeSocket &socket = *task.socket;

    switch (task.type) {
      case TaskType::Value: {
        handle_value_task(tree, socket, tasks, scope, all_socket_values);
        break;
      }
      case TaskType::Usage: {
        handle_usage_task(socket, tasks, all_socket_usages, all_socket_values);
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
      const std::optional<bool> &socket_usage = all_socket_usages[socket.index_in_tree()];
      BLI_assert(socket_usage.has_value());
      r_input_usages[i] |= *socket_usage;
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

}  // namespace blender::nodes::socket_usage_inference
