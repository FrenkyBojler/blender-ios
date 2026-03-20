/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <variant>

#include "FN_field.hh"

#include "DNA_node_types.h"

namespace blender::nodes {

const mf::MultiFunction &int_math_op(NodeIntegerMathOperation operation);

struct IndexTransform {
  int shift;
  bool index_is_reversed;
};

std::variant<std::monostate, int, IndexTransform> field_as_index_transform(const fn::Field<int> &index_field);

}  // namespace blender::nodes
