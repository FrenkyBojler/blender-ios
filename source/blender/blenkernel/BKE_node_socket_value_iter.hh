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

struct EditVisitors {
  FunctionRef<bool(const GeometrySet &)> needs_edit_GeometrySet;
  FunctionRef<void(GeometrySet &)> edit_GeometrySet;

  FunctionRef<bool(const AttributeAccessor &)> needs_edit_AttributeAccessor;
  FunctionRef<void(MutableAttributeAccessor &)> edit_AttributeAccessor;

  FunctionRef<bool(const fn::GField &)> needs_edit_GField;
  FunctionRef<void(fn::GField &)> edit_GField;

  FunctionRef<bool(const SocketValueVariant &)> needs_edit_SocketValueVariant;
  FunctionRef<void(SocketValueVariant &)> edit_SocketValueVariant;
};

void edit_recursive(SocketValueVariant &value, const EditVisitors &visitors);
void edit_recursive(GeometrySet &value, const EditVisitors &visitors);

}  // namespace blender::bke::socket_value_visitor
