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

/* Desciption of index * factor + offset */
struct LineFunc {
  int factor;
  int offset;
};

static std::optional<LineFunc> as_integer_line_function(const fn::GField &entry_fields)
{
  Map<fn::GFieldRef, LineFunc> known_fields;

  Stack<std::pair<fn::GFieldRef, std::optiona<LineFunc>>> fields_to_check;
  fields_to_check.push(std::make_pair(entry_fields, std::nulopt));

  static const mf::MultiFunction &add_func = int_math_op(NODE_INTEGER_MATH_ADD);
  static const mf::MultiFunction &sub_func = int_math_op(NODE_INTEGER_MATH_SUBTRACT);
  static const mf::MultiFunction &minus_func = int_math_op(NODE_INTEGER_MATH_NEGATE);
  static const mf::MultiFunction &mul_func = int_math_op(NODE_INTEGER_MATH_MULTIPLY);
  static const mf::MultiFunction &mul_add_func = int_math_op(NODE_INTEGER_MATH_MULTIPLY_ADD);

  while (!fields_to_check.is_empty()) {
    const auto [field, factor] = fields_to_check.pop();

    if (factor.has_value()) {
      BLI_assert(known_fields.contains(field));
      continue;
    }

    const fn::FieldNode &field_node = field.node();
    switch (field_node.node_type()) {
      case fn::FieldNodeType::Input: {
        BLI_assert(dynamic_cast<const fn::IndexFieldInput *>(&field_node) != nullptr);
        known_fields.add(field, {1, 0});
        continue;
      }
      case fn::FieldNodeType::Constant: {
        /* Currently any constant values are evaluated in place.
         * So any conversion was made before field construction.
         * See #execute_multi_function_on_value_variant for more info. */
        const int offset = field_node.value().get<int>();
        known_fields.add(field, {0, offset});
        continue;
      }
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

std::variant<std::monostate, int, IndexTransform> field_as_index_transform(
    const fn::Field<int> &index_field)
{
  const std::shared_ptr<const fn::FieldInputs> &dependencys = index_field.node().field_inputs();
  if (!dependencys) {
    return std::monostate{};
  }
  if (dependencys->deduplicated_nodes.size() != 1) {
    return std::monostate{};
  }

  const fn::FieldInput &source_index_field =
      dependencys->deduplicated_nodes.as_span().first().get();
  if (dynamic_cast<const fn::IndexFieldInput *>(&source_index_field) == nullptr) {
    return std::monostate{};
  }

  if (!is_only_linear_int_math(index_field)) {
    return std::monostate{};
  }

  const std::array<int, 2> src_indices({0, 1});
  const IndexFieldContext context(src_indices);
  fn::FieldEvaluator evaluator(context, 2);

  std::array<int, 2> dst_indices;
  evaluator.add_with_destination(index_field, GMutableSpan(MutableSpan(dst_indices)));
  evaluator.evaluate();

  if (dst_indices[0] == dst_indices[1]) {
    return dst_indices[0];
  }

  return IndexTransform{dst_indices[0], dst_indices[0] > dst_indices[1]};
}

}  // namespace blender::nodes
