/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_generic_pointer.hh"
#include "BLI_resource_scope.hh"

#include "BKE_compute_context_cache_fwd.hh"

#include "DNA_material_types.h"
#include "NOD_node_in_compute_context.hh"

struct bNodeTree;

namespace blender::nodes {

/**
 * During socket usage inferencing, some socket values are computed. This class represents such a
 * computed value. Not all possible values can be presented here, only "basic" once (like int, but
 * not int-field). A value can also be unknown if it can't be determined statically.
 */
class InferenceValue {
 private:
  /**
   * Non-owning pointer to a value of type #bNodeSocketType.base_cpp_type of the corresponding
   * socket. If this is null, the value is assumed to be unknown (aka, it can't be determined
   * statically).
   */
  const void *value_ = nullptr;

 public:
  explicit InferenceValue(const void *value) : value_(value) {}

  static InferenceValue Unknown()
  {
    return InferenceValue(nullptr);
  }

  bool is_unknown() const
  {
    return value_ == nullptr;
  }

  const void *data() const
  {
    return value_;
  }

  template<typename T> T get_known() const
  {
    BLI_assert(!this->is_unknown());
    return *static_cast<const T *>(this->value_);
  }

  template<typename T> std::optional<T> get() const
  {
    if (this->is_unknown()) {
      return std::nullopt;
    }
    return this->get_known<T>();
  }
};

class SocketValueInferencerImpl;

class SocketValueInferencer {
 private:
  SocketValueInferencerImpl &impl_;

 public:
  SocketValueInferencer(const bNodeTree &tree,
                        ResourceScope &scope,
                        bke::ComputeContextCache &compute_context_cache,
                        const std::optional<Span<GPointer>> tree_input_values,
                        const std::optional<Span<bool>> top_level_ignored_inputs);

  InferenceValue get_socket_value(const SocketInContext &socket);
};

inline bool switch__is_socket_selected(const SocketInContext &socket,
                                       const InferenceValue &condition)
{
  const bool is_true = condition.get_known<bool>();
  const int selected_index = is_true ? 2 : 1;
  return socket->index() == selected_index;
}

inline bool index_switch__is_socket_selected(const SocketInContext &socket,
                                             const InferenceValue &condition)
{
  const int index = condition.get_known<int>();
  return socket->index() == index + 1;
}

inline bool menu_switch__is_socket_selected(const SocketInContext &socket,
                                            const InferenceValue &condition)
{
  const NodeMenuSwitch &storage = *static_cast<const NodeMenuSwitch *>(
      socket->owner_node().storage);
  const int menu_value = condition.get_known<int>();
  const NodeEnumItem &item = storage.enum_definition.items_array[socket->index() - 1];
  return menu_value == item.identifier;
}

inline bool mix_node__is_socket_selected(const SocketInContext &socket,
                                         const InferenceValue &condition)
{
  const NodeShaderMix &storage = *static_cast<const NodeShaderMix *>(socket.owner_node()->storage);
  if (storage.data_type == SOCK_RGBA && storage.blend_type != MA_RAMP_BLEND) {
    return true;
  }

  const bool clamp_factor = storage.clamp_factor != 0;
  bool only_a = false;
  bool only_b = false;
  if (storage.data_type == SOCK_VECTOR && storage.factor_mode == NODE_MIX_MODE_NON_UNIFORM) {
    const float3 mix_factor = condition.get_known<float3>();
    if (clamp_factor) {
      only_a = mix_factor.x <= 0.0f && mix_factor.y <= 0.0f && mix_factor.z <= 0.0f;
      only_b = mix_factor.x >= 1.0f && mix_factor.y >= 1.0f && mix_factor.z >= 1.0f;
    }
    else {
      only_a = float3{0.0f, 0.0f, 0.0f} == mix_factor;
      only_b = float3{1.0f, 1.0f, 1.0f} == mix_factor;
    }
  }
  else {
    const float mix_factor = condition.get_known<float>();
    if (clamp_factor) {
      only_a = mix_factor <= 0.0f;
      only_b = mix_factor >= 1.0f;
    }
    else {
      only_a = mix_factor == 0.0f;
      only_b = mix_factor == 1.0f;
    }
  }
  if (only_a) {
    if (STREQ(socket->name, "B")) {
      return false;
    }
  }
  if (only_b) {
    if (STREQ(socket->name, "A")) {
      return false;
    }
  }
  return true;
}

inline bool shader_mix_node__is_socket_selected(const SocketInContext &socket,
                                                const InferenceValue &condition)
{
  const float mix_factor = condition.get_known<float>();
  if (mix_factor == 0.0f) {
    if (STREQ(socket->identifier, "Shader_001")) {
      return false;
    }
  }
  else if (mix_factor == 1.0f) {
    if (STREQ(socket->identifier, "Shader")) {
      return false;
    }
  }
  return true;
}

inline const bNodeSocket *get_first_available_bsocket(const Span<const bNodeSocket *> sockets)
{
  for (const bNodeSocket *socket : sockets) {
    if (socket->is_available()) {
      return socket;
    }
  }
  return nullptr;
}

}  // namespace blender::nodes
