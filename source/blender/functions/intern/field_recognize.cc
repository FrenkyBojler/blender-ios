/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <variant>

#include "BLI_map.hh"
#include "BLI_stack.hh"

#include "FN_field.hh"
#include "FN_field_recognize.hh"
#include "FN_multi_function_registry.hh"

namespace blender::fn {

std::optional<Polynom<int>> field_as_polynom_try(const Field<int> &entry_field)
{
  const fn::FieldInputsPtr &dependencys = GField(entry_field).field_inputs();
  if (!dependencys) {
    /* TODO: Extract constant. */
    return std::nullopt;
  }
  if (dependencys->inputs.size() != 1) {
    return std::nullopt;
  }

  /* Multiplication basiacally double degree, so it is easy to make exponential bomb from repeat
   * zone and multiply. */
  constexpr int max_degree = 120;

  static const mf::MultiFunction &minus_func = multi_function::registry::lookup("-int"_ustr);
  static const mf::MultiFunction &add_func = multi_function::registry::lookup("int + int"_ustr);
  static const mf::MultiFunction &sub_func = multi_function::registry::lookup("int - int"_ustr);
  /* TODO: Support integer division. */
  static const mf::MultiFunction &mul_func = multi_function::registry::lookup("int * int"_ustr);
  static const mf::MultiFunction &mul_add_func = multi_function::registry::lookup(
      "int * int + int"_ustr);

  Map<GFieldRef, Polynom<int>> known_fields;

  Stack<GFieldRef> fields_to_check;
  fields_to_check.push(entry_field);

  while (!fields_to_check.is_empty()) {
    const GFieldRef field = fields_to_check.pop();
    if (const Polynom<int> *value = known_fields.lookup_ptr(field)) {
      if (value->size() > max_degree) {
        return std::nullopt;
      }
      continue;
    }

    enum class LoopState {
      Continue,
      Terminate,
    };

    const auto state = std::visit(
        [&]<typename T>(const T field_data) -> LoopState {
          if constexpr (std::is_same_v<T, GFieldRef::Value>) {
            BLI_assert(field_data.type->template is<int>());
            /* Currently any constant values are evaluated in place.
             * So any conversion was made before field construction.
             * See #execute_multi_function_on_value_variant for more info. */
            /* Have to use implicit cast to void ptr in case value is buffer object. */
            const int constant = *static_cast<const int *>(field_data.value);
            known_fields.add(field, Polynom<int>::from_const(constant));
            return LoopState::Continue;
          }
          else

              if constexpr (std::is_same_v<T, GFieldRef::Input>)
          {
            known_fields.add(field, Polynom<int>::from_degree_variable(1, 1));
            return LoopState::Continue;
          }
          else {

            static_assert(std::is_same_v<T, GFieldRef::MultiFn>);
            const fn::FieldOperation &operation = *field_data.node;
            BLI_assert(field_data.output_i == 0);
            const mf::MultiFunction &node_function = operation.multi_function();
            if (!ELEM(&node_function, &minus_func, &add_func, &sub_func, &mul_func, &mul_add_func))
            {
              return LoopState::Terminate;
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

              return LoopState::Continue;
            }

            const Polynom<int> &first_arg = known_fields.lookup(inputs[0]);
            if (&node_function == &minus_func) {
              known_fields.add(field, -first_arg);
              return LoopState::Continue;
            }

            const Polynom<int> &second_arg = known_fields.lookup(inputs[1]);
            if (&node_function == &add_func) {
              known_fields.add(field, first_arg + second_arg);
              return LoopState::Continue;
            }

            if (&node_function == &sub_func) {
              known_fields.add(field, first_arg - second_arg);
              return LoopState::Continue;
            }

            if (first_arg.size() + second_arg.size() > max_degree) {
              return LoopState::Terminate;
            }

            if (&node_function == &mul_func) {
              known_fields.add(field, first_arg * second_arg);
              return LoopState::Continue;
            }

            BLI_assert(&node_function == &mul_add_func);
            const Polynom<int> &third_arg = known_fields.lookup(inputs[2]);
            known_fields.add(field, first_arg * second_arg + third_arg);

            return LoopState::Continue;
          }
        },
        field.variant());

    switch (state) {
      using enum LoopState;
      case Continue:
        break;
      case Terminate:
        return std::nullopt;
    }
  }

  return known_fields.lookup(entry_field);
}

}  // namespace blender::fn
