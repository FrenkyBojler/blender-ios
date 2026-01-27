/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_bake_data_block_map.hh"
#include "BKE_node_socket_value.hh"

#include "BLI_compute_context.hh"
#include "BLI_map.hh"

namespace blender::bke::bake {

class BakeValues {
 public:
  struct Item {
    SocketValueVariant value;
    std::optional<std::string> name;
  };

 private:
  Map<int, Item> values_by_id_;

 public:
  struct InputValue {
    int id;
    std::string name;
    SocketValueVariant value;
  };
  struct OutputKey {
    int id;
    eNodeSocketDatatype type;
  };

  BakeValues() = default;
  explicit BakeValues(Map<int, Item> values_by_id) : values_by_id_(std::move(values_by_id)) {}

  static BakeValues from_runtime_values(Vector<InputValue> runtime_values,
                                        const BakeDataBlockMap *data_block_map);
  Vector<SocketValueVariant> to_runtime_values(const Span<OutputKey> keys,
                                               const ComputeContext &compute_context,
                                               const BakeDataBlockMap *data_block_map) const;

  bool is_empty() const
  {
    return values_by_id_.is_empty();
  }

  void clear()
  {
    values_by_id_.clear();
  }

  const Map<int, Item> &values_by_id() const
  {
    return values_by_id_;
  }
};

}  // namespace blender::bke::bake
