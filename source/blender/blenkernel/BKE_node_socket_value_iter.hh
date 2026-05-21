/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_function_ref.hh"

namespace blender::fn {
class GField;
}

namespace blender::bke {

struct GeometrySet;
class SocketValueVariant;
class AttributeAccessor;
class MutableAttributeAccessor;

}  // namespace blender::bke

namespace blender::bke::socket_value_visitor {

struct VisitParams {
  static bool needs_edit(const bool value)
  {
    return value;
  }
  static bool continue_check(const bool value)
  {
    return !value;
  }

  bool check_non_editable = false;
  bool check_non_geometry_instance_references = false;

  FunctionRef<bool(const GeometrySet &)> check_GeometrySet;
  FunctionRef<void(GeometrySet &)> edit_GeometrySet;

  FunctionRef<bool(const AttributeAccessor &)> check_AttributeAccessor;
  FunctionRef<void(MutableAttributeAccessor &)> edit_AttributeAccessor;

  FunctionRef<bool(const fn::GField &)> check_GField;
  FunctionRef<void(fn::GField &)> edit_GField;

  FunctionRef<bool(const SocketValueVariant &)> check_SocketValueVariant;
  FunctionRef<void(SocketValueVariant &)> edit_SocketValueVariant;
};

void edit_recursive(SocketValueVariant &value, const VisitParams &params);
void edit_recursive(GeometrySet &value, const VisitParams &params);

void check_recursive(const SocketValueVariant &value, const VisitParams &params);

}  // namespace blender::bke::socket_value_visitor
