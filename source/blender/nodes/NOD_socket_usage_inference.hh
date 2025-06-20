/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_array.hh"
#include "BLI_generic_pointer.hh"

#include "BKE_node.hh"

#include "NOD_geometry_nodes_execute.hh"
#include "NOD_socket_usage_inference_fwd.hh"

struct bNodeTree;
struct bNodeSocket;
struct IDProperty;

namespace blender::nodes::socket_usage_inference {

struct SocketUsageInferencer;

class InputSocketUsageParams {
 private:
  SocketUsageInferencer &inferencer_;
  const ComputeContext *compute_context_ = nullptr;

 public:
  const bNodeTree &tree;
  const bNode &node;
  const bNodeSocket &socket;

  InputSocketUsageParams(SocketUsageInferencer &inferencer,
                         const ComputeContext *compute_context,
                         const bNodeTree &tree,
                         const bNode &node,
                         const bNodeSocket &socket);

  /**
   * Get an the statically known input value for the given socket identifier. The value may be
   * unknown, in which case null is returned.
   */
  const void *get_input(StringRef identifier) const;
  template<typename T> const T *get_input(StringRef identifier) const;

  /**
   * Utility for the case when the socket depends on a specific menu input to have a certain value.
   */
  bool menu_input_may_be(StringRef identifier, int enum_value) const;
};

/**
 * Get a boolean value for each input socket in the given tree that indicates whether that input is
 * used. It is assumed that all output sockets in the tree are used.
 */
Array<SocketUsage> infer_all_input_sockets_usage(const bNodeTree &tree);

/**
 * Get a boolean value for each node group input that indicates whether that input is used by the
 * outputs. The result can be used to e.g. gray out or hide individual inputs that are unused.
 *
 * \param group: The node group that is called.
 * \param group_input_values: An optional input value for each node group input. The type is
 *   expected to be `bNodeSocketType::base_cpp_type`. If the input value for a socket is not known
 *   or can't be represented as base type, null has to be passed instead.
 * \param r_input_usages: The destination array where the inferred usages are written.
 */
void infer_group_interface_inputs_usage(const bNodeTree &group,
                                        Span<GPointer> group_input_values,
                                        MutableSpan<SocketUsage> r_input_usages);

/**
 * Same as above, but automatically retrieves the input values from the given sockets..
 * This is used for group nodes.
 */
void infer_group_interface_inputs_usage(const bNodeTree &group,
                                        Span<const bNodeSocket *> input_sockets,
                                        MutableSpan<SocketUsage> r_input_usages);

/**
 * Same as above, but automatically retrieves the input values from the given properties.
 * This is used with the geometry nodes modifier and node tools.
 */
void infer_group_interface_inputs_usage(const bNodeTree &group,
                                        const PropertiesVectorSet &properties,
                                        MutableSpan<SocketUsage> r_input_usages);

template<typename T> const T *InputSocketUsageParams::get_input(const StringRef identifier) const
{
  BLI_assert(this->node.input_by_identifier(identifier).typeinfo->base_cpp_type ==
             &CPPType::get<T>());
  const void *value = this->get_input(identifier);
  return static_cast<const T *>(value);
}

}  // namespace blender::nodes::socket_usage_inference
