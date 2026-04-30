/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_exec.hh"
#include "NOD_geometry_nodes_list.hh"

#include "NOD_list_function_eval.hh"

namespace blender::nodes {

namespace list::multi_function_eval {

GVArray ListFieldContext::get_varray_for_input(const fn::FieldInput &field_input,
                                               const IndexMask &mask,
                                               ResourceScope & /*scope*/) const
{
  const auto *id_field_input = dynamic_cast<const bke::IDAttributeFieldInput *>(&field_input);
  const auto *index_field_input = dynamic_cast<const fn::IndexFieldInput *>(&field_input);
  if (id_field_input == nullptr && index_field_input == nullptr) {
    return {};
  }

  return fn::IndexFieldInput::get_index_varray(mask);
}

nodes::GListPtr evaluate_field_to_list(fn::GField field, const int64_t count)
{
  const CPPType &cpp_type = field.cpp_type();
  GArray array(cpp_type, count);

  ListFieldContext context{};
  fn::FieldEvaluator evaluator{context, count};
  evaluator.add_with_destination(std::move(field), array);
  evaluator.evaluate();

  return nodes::GList::from_garray(std::move(array));
}

static const nodes::GList &create_repeated_list(const nodes::GList *list,
                                                const int64_t dst_size,
                                                Vector<nodes::GListPtr> &repeated_lists)
{
  if (list->size() >= dst_size) {
    return *list;
  }
  if (const auto *data = std::get_if<nodes::GList::SingleData>(&list->data())) {
    const CPPType &cpp_type = list->cpp_type();
    repeated_lists.append(nodes::GList::create(cpp_type, *data, dst_size));
    return *repeated_lists.last();
  }
  const auto &data = std::get<nodes::GList::ArrayData>(list->data());
  const int64_t size = list->size();
  BLI_assert(size > 0);
  const CPPType &cpp_type = list->cpp_type();
  GArray new_data(cpp_type, dst_size, NoInitialization{});
  const int64_t chunks = dst_size / size;
  for (const int64_t i : IndexRange(chunks)) {
    cpp_type.copy_construct_n(data.data, new_data[i * size], size);
  }
  const int64_t last_chunk_size = dst_size % size;
  if (last_chunk_size > 0) {
    cpp_type.copy_construct_n(data.data, new_data[chunks * size], last_chunk_size);
  }

  repeated_lists.append(nodes::GList::from_garray(std::move(new_data)));
  return *repeated_lists.last();
}

static void add_list_to_params(mf::ParamsBuilder &params,
                               const mf::ParamType &param_type,
                               const nodes::GList &list)
{
  const CPPType &cpp_type = param_type.data_type().single_type();
  BLI_assert(cpp_type == list.cpp_type());
  if (const auto *array_data = std::get_if<nodes::GList::ArrayData>(&list.data())) {
    params.add_readonly_single_input(GSpan(cpp_type, array_data->data, list.size()));
  }
  else if (const auto *single_data = std::get_if<nodes::GList::SingleData>(&list.data())) {
    params.add_readonly_single_input(GPointer(cpp_type, single_data->value));
  }
}

EvalResult evaluate_multi_function_on_list(const mf::MultiFunction &fn,
                                           const Span<InputVariant> input_values,
                                           const Span<bool> output_usages)
{
  int inputs_num = 0;
  int outputs_num = 0;
  for (const int param_i : fn.param_indices()) {
    const mf::ParamType param_type = fn.param_type(param_i);
    if (param_type.interface_type() == mf::ParamType::Input) {
      inputs_num++;
    }
    else if (param_type.interface_type() == mf::ParamType::Output) {
      outputs_num++;
    }
    else {
      BLI_assert_unreachable();
      return {EvalResult::Failure()};
    }
  }

  BLI_assert(input_values.size() == inputs_num);
  BLI_assert(output_usages.size() == outputs_num);

  bool any_list = false;
  int64_t max_size = 0;
  for (const int i : input_values.index_range()) {
    if (const auto *const list = std::get_if<const nodes::GList *>(&input_values[i])) {
      if (*list) {
        max_size = std::max(max_size, (*list)->size());
        any_list = true;
      }
    }
  }

  if (!any_list) {
    BLI_assert_unreachable();
    return {EvalResult::Failure()};
  }

  const IndexMask mask(max_size);
  mf::ParamsBuilder params{fn, &mask};
  mf::ContextBuilder context;

  Vector<nodes::GListPtr> created_lists(input_values.size());
  for (const int i : IndexRange(inputs_num)) {
    const mf::ParamType param_type = fn.param_type(params.next_param_index());
    const CPPType &cpp_type = param_type.data_type().single_type();
    if (const auto *single = std::get_if<GPointer>(&input_values[i])) {
      params.add_readonly_single_input(*single);
    }
    else if (const auto *const list = std::get_if<const nodes::GList *>(&input_values[i])) {
      if (!*list || (*list)->size() == 0) {
        params.add_readonly_single_input(GPointer(cpp_type, cpp_type.default_value()));
        continue;
      }
      add_list_to_params(params, param_type, create_repeated_list(*list, max_size, created_lists));
    }
    else if (const auto *const *field = std::get_if<const fn::GField *>(&input_values[i])) {
      created_lists.append(evaluate_field_to_list(**field, max_size));
      add_list_to_params(params, param_type, *created_lists.last());
    }
  }

  Array<nodes::GListPtr> ouput_lists(outputs_num);
  for (const int i : output_usages.index_range()) {
    if (!output_usages[i]) {
      params.add_ignored_single_output("");
      ouput_lists[i] = {};
      continue;
    }
    const mf::ParamType param_type = fn.param_type(params.next_param_index());
    const CPPType &cpp_type = param_type.data_type().single_type();
    GArray array(cpp_type, max_size, NoInitialization{});
    params.add_uninitialized_single_output(GMutableSpan(cpp_type, array.data(), max_size));
    ouput_lists[i] = nodes::GList::from_garray(std::move(array));
  }

  fn.call(mask, params, context);

  return {EvalResult::Success{std::move(ouput_lists)}};
}

}  // namespace list::multi_function_eval

bool execute_multi_function_on_value_variant__list(const mf::MultiFunction &fn,
                                                   const Span<SocketValueVariant *> input_values,
                                                   const Span<SocketValueVariant *> output_values)
{
  using namespace list::multi_function_eval;

  const int inputs_num = input_values.size();

  Array<InputVariant> inputs(inputs_num);
  Array<nodes::GListPtr> input_lists(inputs_num);
  Array<std::optional<GField>> input_fields(inputs_num);

  for (const int i : input_values.index_range()) {
    bke::SocketValueVariant &input_value = *input_values[i];
    if (input_value.is_list()) {
      input_lists[i] = input_value.extract<nodes::GListPtr>();
      inputs[i] = input_lists[i].get();
    }
    else if (input_value.is_context_dependent_field()) {
      input_fields[i] = input_value.extract<GField>();
      inputs[i] = &*input_fields[i];
    }
    else {
      input_value.convert_to_single();
      inputs[i] = input_value.get_single_ptr();
    }
  }

  Array<bool> output_usages(output_values.size());
  for (const int i : output_values.index_range()) {
    output_usages[i] = output_values[i] != nullptr;
  }

  EvalResult result = evaluate_multi_function_on_list(fn, inputs, output_usages);

  if (const auto *_ = std::get_if<EvalResult::Failure>(&result.result)) {
    return false;
  }
  auto &success = std::get<EvalResult::Success>(result.result);
  for (const int i : output_values.index_range()) {
    if (output_usages[i]) {
      output_values[i]->set(std::move(success.output_lists[i]));
    }
  }

  return true;
}

}  // namespace blender::nodes
