/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_set.hh"
#include "BLI_stack.hh"

#include "FN_field.hh"
#include "FN_field_recognize.hh"
#include "FN_multi_function.hh"
#include "FN_multi_function_registry.hh"

#include "DNA_node_types.h"

namespace blender::fn {

std::optional<Polynom<int>> field_as_polynom_try(const Field<int> &entry_field)
{
  const std::shared_ptr<const fn::FieldInputs> &dependencys = entry_field.node().field_inputs();
  if (!dependencys) {
    /* TODO: Extract constant. */
    return std::nullopt;
  }
  if (dependencys->deduplicated_nodes.size() != 1) {
    return std::nullopt;
  }

  static const mf::MultiFunction &minus_func = multi_function::registry::lookup("-int"_ustr);
  static const mf::MultiFunction &add_func = multi_function::registry::lookup("int + int"_ustr);
  static const mf::MultiFunction &sub_func = multi_function::registry::lookup("int - int"_ustr);
  static const mf::MultiFunction &mul_func = multi_function::registry::lookup("int * int"_ustr);
  /* TODO: Support integer division. */
  static const mf::MultiFunction &mul_add_func = multi_function::registry::lookup(
      "int * int + int"_ustr);

  Map<GFieldRef, Polynom<int>> known_fields;

  Stack<GFieldRef> fields_to_check;
  fields_to_check.push(entry_field);

  while (!fields_to_check.is_empty()) {
    const GFieldRef field = fields_to_check.pop();
    if (known_fields.contains(field)) {
      continue;
    }

    const FieldNode &field_node = field.node();
    const FieldNodeType node_type = field_node.node_type();
    if (node_type == FieldNodeType::Input) {
      known_fields.add(field, Polynom<int>::from_degree_variable(1, 1));
      continue;
    }

    if (node_type == FieldNodeType::Constant) {
      /* Currently any constant values are evaluated in place.
       * So any conversion was made before field construction.
       * See #execute_multi_function_on_value_variant for more info. */
      const int offset = *dynamic_cast<const FieldConstant &>(field_node).value().get<int>();
      known_fields.add(field, Polynom<int>::from_const(offset));
      continue;
    }

    BLI_assert(node_type == FieldNodeType::Operation);
    const FieldOperation &operation = dynamic_cast<const FieldOperation &>(field_node);
    const mf::MultiFunction &node_function = operation.multi_function();
    if (!ELEM(&node_function, &minus_func, &add_func, &sub_func, &mul_func, &mul_add_func)) {
      return std::nullopt;
    }

    const Span<GField> inputs = operation.inputs();
    const bool all_known = std::all_of(
        inputs.begin(), inputs.end(), [&](const GFieldRef input_field) {
          return known_fields.contains(input_field);
        });

    if (!all_known) {
      fields_to_check.push(field);

      for (const GFieldRef input_field : inputs) {
        if (!known_fields.contains(input_field)) {
          fields_to_check.push(input_field);
        }
      }

      continue;
    }

    const Polynom<int> &first_arg = known_fields.lookup(inputs[0]);
    if (&node_function == &minus_func) {
      known_fields.add(field, -first_arg);
      continue;
    }

    const Polynom<int> &second_arg = known_fields.lookup(inputs[1]);
    if (&node_function == &add_func) {
      known_fields.add(field, first_arg + second_arg);
      continue;
    }

    if (&node_function == &sub_func) {
      known_fields.add(field, first_arg - second_arg);
      continue;
    }

    if (&node_function == &mul_func) {
      known_fields.add(field, first_arg * second_arg);
      continue;
    }

    BLI_assert(&node_function == &mul_add_func);
    const Polynom<int> &third_arg = known_fields.lookup(inputs[2]);
    known_fields.add(field, first_arg * second_arg + third_arg);

    /* This simple implementation does not support non-linear polynomial even as temporarily
     * values`. */
    return std::nullopt;
  }

  return known_fields.lookup(entry_field);
}

}  // namespace blender::fn
