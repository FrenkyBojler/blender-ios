/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_node.hh"

#include "NOD_socket_interface_key.hh"

namespace blender::nodes {

/** Describes the names and types of the inputs and outputs of a closure. */
class ClosureSignature {
 public:
  struct Item {
    SocketInterfaceKey key;
    const bke::bNodeSocketType *type = nullptr;
    std::optional<StructureType> structure_type = std::nullopt;
  };

  Vector<Item> inputs;
  Vector<Item> outputs;

  std::optional<int> find_input_index(const SocketInterfaceKey &key) const;
  std::optional<int> find_output_index(const SocketInterfaceKey &key) const;

  bool matches_exactly(const ClosureSignature &other) const
  {
    if (inputs.size() != other.inputs.size()) {
      return false;
    }
    if (outputs.size() != other.outputs.size()) {
      return false;
    }
    for (const Item &item : inputs) {
      if (std::none_of(other.inputs.begin(), other.inputs.end(), [&](const Item &other_item) {
            return item.key.matches(other_item.key) && item.type == other_item.type &&
                   item.structure_type == other_item.structure_type;
          }))
      {
        return false;
      }
    }
    for (const Item &item : outputs) {
      if (std::none_of(other.outputs.begin(), other.outputs.end(), [&](const Item &other_item) {
            return item.key.matches(other_item.key) && item.type == other_item.type &&
                   item.structure_type == other_item.structure_type;
          }))
      {
        return false;
      }
    }
    return true;
  }
};

}  // namespace blender::nodes
