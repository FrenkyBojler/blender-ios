/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_nodes_closure.hh"

namespace blender::bke {

ClosureSignature::ClosureSignature(Vector<Item> inputs, Vector<Item> outputs)
    : inputs_(std::move(inputs)), outputs_(std::move(outputs))
{
}

std::optional<int> ClosureSignature::get_input_index(const SocketInterfaceKey &key) const
{
  for (const int i : inputs_.index_range()) {
    const Item &item = inputs_[i];
    if (item.key.matches(key)) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<int> ClosureSignature::get_output_index(const SocketInterfaceKey &key) const
{
  for (const int i : outputs_.index_range()) {
    const Item &item = outputs_[i];
    if (item.key.matches(key)) {
      return i;
    }
  }
  return std::nullopt;
}

}  // namespace blender::bke
