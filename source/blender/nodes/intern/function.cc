/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_set.hh"
#include "BLI_stack.hh"

#include "FN_field.hh"
#include "FN_multi_function.hh"

#include "NOD_function.hh"

#include "DNA_node_types.h"

namespace blender::nodes {

static bool is_only_linear_int_math(const fn::GField &entry_fields)
{
  Stack<fn::GFieldRef> fields_to_check;
  Set<fn::GFieldRef> handled_fields;

  handled_fields.add(entry_fields);
  fields_to_check.push(entry_fields);

  static const mf::MultiFunction &add_func = int_math_op(NODE_INTEGER_MATH_ADD);
  static const mf::MultiFunction &sub_func = int_math_op(NODE_INTEGER_MATH_SUBTRACT);
  static const mf::MultiFunction &minus_func = int_math_op(NODE_INTEGER_MATH_NEGATE);

  while (!fields_to_check.is_empty()) {
    const fn::GFieldRef field = fields_to_check.pop();
    const fn::FieldNode &field_node = field.node();
    switch (field_node.node_type()) {
      case fn::FieldNodeType::Input:
      case fn::FieldNodeType::Constant:
        break;
      case fn::FieldNodeType::Operation: {
        const fn::FieldOperation &operation = static_cast<const fn::FieldOperation &>(field_node);
        if (!ELEM(&operation.multi_function(), &add_func, &sub_func, &minus_func)) {
          return false;
        }
        for (const fn::GFieldRef operation_input : operation.inputs()) {
          if (handled_fields.add(operation_input)) {
            fields_to_check.push(operation_input);
          }
        }
        break;
      }
    }
  }

  return true;
}

class IndexFieldContext : public fn::FieldContext {
  Span<int> indices_;

 public:
  IndexFieldContext(const Span<int> indices) : indices_(indices) {}

  GVArray get_varray_for_input(const fn::FieldInput &field_input,
                               const IndexMask &mask,
                               ResourceScope &scope) const final
  {
    BLI_assert(mask.size() <= indices_.size());

    if (dynamic_cast<const fn::IndexFieldInput *>(&field_input) == nullptr) {
      return {};
    }

    return VArray<int>::from_span(indices_);
  }
};

std::optional<IndexTransform> field_as_range(const fn::Field<int> &index_field)
{
  const std::shared_ptr<const fn::FieldInputs> &dependencys = index_field.node().field_inputs();
  if (!dependencys) {
    return std::nullopt;
  }
  if (dependencys->deduplicated_nodes.size() != 1) {
    return std::nullopt;
  }

  const fn::FieldInput &source_index_field =
      dependencys->deduplicated_nodes.as_span().first().get();
  if (dynamic_cast<const fn::IndexFieldInput *>(&source_index_field) == nullptr) {
    return std::nullopt;
  }

  if (!is_only_linear_int_math(index_field)) {
    return std::nullopt;
  }

  const std::array<int, 2> src_indices({0, 1});
  const IndexFieldContext context(src_indices);
  fn::FieldEvaluator evaluator(context, 2);

  std::array<int, 2> dst_indices;
  evaluator.add_with_destination(index_field, GMutableSpan(MutableSpan(dst_indices)));
  evaluator.evaluate();
  return IndexTransform{dst_indices[0], dst_indices[0] > dst_indices[1]};
}

}  // namespace blender::nodes
