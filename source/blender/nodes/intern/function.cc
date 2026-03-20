/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
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

static std::optional<LineFunc> as_integer_line_function_imp(const fn::GField &entry_field)
{
  Map<fn::GFieldRef, LineFunc> known_fields;

  Stack<fn::GFieldRef> fields_to_check;
  fields_to_check.push(entry_field);

  static const mf::MultiFunction &add_func = int_math_op(NODE_INTEGER_MATH_ADD);
  static const mf::MultiFunction &sub_func = int_math_op(NODE_INTEGER_MATH_SUBTRACT);
  static const mf::MultiFunction &minus_func = int_math_op(NODE_INTEGER_MATH_NEGATE);
  static const mf::MultiFunction &mul_func = int_math_op(NODE_INTEGER_MATH_MULTIPLY);
  static const mf::MultiFunction &mul_add_func = int_math_op(NODE_INTEGER_MATH_MULTIPLY_ADD);

  while (!fields_to_check.is_empty()) {
    const fn::GFieldRef field = fields_to_check.pop();
    if (known_fields.contains(field)) {
      continue;
    }

    const fn::FieldNode &field_node = field.node();
    const fn::FieldNodeType node_type = field_node.node_type();
    if (node_type == fn::FieldNodeType::Input) {
      BLI_assert(dynamic_cast<const fn::IndexFieldInput *>(&field_node) != nullptr);
      known_fields.add(field, {1, 0});
      continue;
    }

    if (node_type == fn::FieldNodeType::Constant) {
      /* Currently any constant values are evaluated in place.
       * So any conversion was made before field construction.
       * See #execute_multi_function_on_value_variant for more info. */
      const int offset = *dynamic_cast<const fn::FieldConstant &>(field_node).value().get<int>();
      known_fields.add(field, {0, offset});
      continue;
    }

    BLI_assert(node_type == fn::FieldNodeType::Operation);
    const fn::FieldOperation &operation = static_cast<const fn::FieldOperation &>(field_node);
    const mf::MultiFunction &node_function = operation.multi_function();
    if (!ELEM(&node_function, &minus_func, &add_func, &sub_func, &mul_func, &mul_add_func)) {
      return std::nullopt;
    }

    const Span<fn::GField> inputs = operation.inputs();
    const bool all_known = std::all_of(
        inputs.begin(), inputs.end(), [&](const fn::GFieldRef input_field) {
          return known_fields.contains(input_field);
        });

    if (!all_known) {
      fields_to_check.push(field);

      for (const fn::GFieldRef input_field : inputs) {
        if (!known_fields.contains(input_field)) {
          fields_to_check.push(input_field);
        }
      }

      continue;
    }

    const LineFunc &first_arg = known_fields.lookup(inputs[0]);
    if (&node_function == &minus_func) {
      known_fields.add(field, {-first_arg.factor, -first_arg.offset});
      continue;
    }

    const LineFunc &second_arg = known_fields.lookup(inputs[1]);
    if (&node_function == &add_func) {
      known_fields.add(
          field, {first_arg.factor + second_arg.factor, first_arg.offset + second_arg.offset});
      continue;
    }

    if (&node_function == &sub_func) {
      known_fields.add(
          field, {first_arg.factor - second_arg.factor, first_arg.offset - second_arg.offset});
      continue;
    }

    if (&node_function == &mul_func) {

      if (second_arg.factor == 0) {
        known_fields.add(
            field, {first_arg.factor * second_arg.offset, first_arg.offset * second_arg.offset});
        continue;
      }

      if (first_arg.factor == 0) {
        known_fields.add(
            field, {second_arg.factor * first_arg.offset, second_arg.offset * first_arg.offset});
        continue;
      }

      /* This simple implementation does not support non-linear polynomial even as temporarily
       * values`. */
      return std::nullopt;
    }

    BLI_assert(&node_function == &mul_add_func);

    const LineFunc &third_arg = known_fields.lookup(inputs[2]);
    if (second_arg.factor == 0) {
      known_fields.add(field,
                       {first_arg.factor * second_arg.offset + third_arg.factor,
                        first_arg.offset * second_arg.offset + third_arg.offset});
      continue;
    }

    if (first_arg.factor == 0) {
      known_fields.add(field,
                       {second_arg.factor * first_arg.offset + third_arg.factor,
                        second_arg.offset * first_arg.offset + third_arg.offset});
      continue;
    }

    /* This simple implementation does not support non-linear polynomial even as temporarily
     * values`. */
    return std::nullopt;
  }

  return known_fields.lookup(entry_field);
}

class IndexFieldContext : public fn::FieldContext {
  Span<int> indices_;

 public:
  IndexFieldContext(const Span<int> indices) : indices_(indices) {}

  GVArray get_varray_for_input(const fn::FieldInput &field_input,
                               const IndexMask &mask,
                               ResourceScope & /*scope*/) const final
  {
    BLI_assert(mask.size() <= indices_.size());

    if (dynamic_cast<const fn::IndexFieldInput *>(&field_input) == nullptr) {
      return {};
    }

    return VArray<int>::from_span(indices_);
  }
};

static void evaluate_on(const Span<int> src_indices,
                        const fn::GField &field,
                        MutableSpan<int> dst_indices)
{
  BLI_assert(src_indices.size() == dst_indices.size());
  const IndexFieldContext context(src_indices);
  fn::FieldEvaluator evaluator(context, src_indices.size());

  evaluator.add_with_destination(field, dst_indices);
  evaluator.evaluate();
}

static std::optional<LineFunc> as_integer_line_function(const fn::GField &field)
{
  const std::optional<LineFunc> as_func = as_integer_line_function_imp(field);
  if (!as_func.has_value()) {
    return std::nullopt;
  }

#ifndef NDEBUG
  std::array<int, 10> src_indices;
  array_utils::fill_index_range<int>(src_indices, -5);
  std::array<int, 10> dst_indices;
  evaluate_on(src_indices, field, dst_indices);

  for (const int i : IndexRange(10)) {
    BLI_assert(src_indices[i] * as_func->factor + as_func->offset == dst_indices[i]);
  }
#endif

  return as_func;
}

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

  const std::optional<LineFunc> as_func = as_integer_line_function(index_field);
  if (!as_func.has_value()) {
    return std::monostate{};
  }

  if (as_func->factor == 0) {
    return as_func->offset;
  }

  if (!ELEM(as_func->factor, -1, 1)) {
    return std::monostate{};
  }

  return IndexTransform{as_func->offset, as_func->factor < 0};
}

}  // namespace blender::nodes
