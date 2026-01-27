/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_bake_values.hh"

#include "BKE_node.hh"

namespace blender::bke::bake {

BakeValues BakeValues::from_runtime_values(Vector<InputValue> runtime_values,
                                           const BakeDataBlockMap * /*data_block_map*/)
{
  BakeValues bake_values;
  // TODO: ensure owns all data, material pointers, clear fields/closures, ...
  for (InputValue &input_value : runtime_values) {
    input_value.value.ensure_owns_direct_data();
  }
  for (InputValue &input_value : runtime_values) {
    bake_values.values_by_id_.add(input_value.id,
                                  Item{std::move(input_value.value), std::move(input_value.name)});
  }
  return bake_values;
}

Vector<SocketValueVariant> BakeValues::to_runtime_values(
    const Span<OutputKey> keys,
    const ComputeContext & /*compute_context*/,
    const BakeDataBlockMap * /*data_block_map*/) const
{
  Vector<SocketValueVariant> output_values(keys.size());
  for (const int i : keys.index_range()) {
    const OutputKey &key = keys[i];
    SocketValueVariant &output_value = output_values[i];
    const Item *item = values_by_id_.lookup_ptr(key.id);
    if (item == nullptr) {
      bke::bNodeSocketType *stype = node_socket_type_find_static(key.type);
      if (!stype) {
        continue;
      }
      if (!stype->geometry_nodes_default_value) {
        continue;
      }
      output_value = *stype->geometry_nodes_default_value;
      continue;
    }
    // TODO: potentially implicit conversion
    output_value = item->value;
  }
  return output_values;
}

}  // namespace blender::bke::bake
