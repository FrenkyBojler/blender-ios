/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_generic_array.hh"
#include "BLI_generic_span.hh"

#include "NOD_geometry_nodes_list_fwd.hh"

namespace blender::nodes {

/**
 * A bundle is a map containing keys and their corresponding values. Values are stored as the type
 * they have in Geometry Nodes (#bNodeSocketType::geometry_nodes_cpp_type).
 */
class List : public ImplicitSharingMixin {
 public:
  List();
  List(const List &other);
  List(List &&other) noexcept;
  List &operator=(const List &other);
  List &operator=(List &&other) noexcept;
  ~List();

  static ListPtr for_garray(GArray<> array);

  virtual GSpan values() const = 0;
  virtual GMutableSpan values_for_write() = 0;

  void delete_self() override;
};

}  // namespace blender::nodes
