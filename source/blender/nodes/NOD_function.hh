/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "FN_field.hh"

#include "DNA_node_types.h"

namespace blender::nodes {

const mf::MultiFunction &int_math_op(NodeIntegerMathOperation operation);

struct IndexTransform {
  int shift;
  bool index_is_reversed;
};

std::optional<IndexTransform> field_as_range(const fn::Field<int> &index_field);

}  // namespace blender::nodes
