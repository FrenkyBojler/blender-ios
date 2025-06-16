/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_exec.hh"
#include "NOD_geometry_nodes_lazy_function.hh"
#include "NOD_geometry_nodes_list.hh"

#include "list_function_eval.hh"

namespace blender::nodes {

static ListPtr create_repeated_list(ListPtr list, const int64_t dst_size)
{
  if (list->size() >= dst_size) {
    return list;
  }
  if (const auto *data = std::get_if<nodes::ArrayData>(&list->data())) {
    const int64_t size = list->size();
    const CPPType &cpp_type = list->cpp_type();
    ArrayData new_data = ArrayData::ForUninitialized(cpp_type, dst_size);
    const int64_t chunks = dst_size / size;
    for (const int64_t i : IndexRange(chunks)) {
      const int64_t offset = cpp_type.size * i * size;
      cpp_type.copy_construct_n(data->data, POINTER_OFFSET(new_data.data, offset), size);
    }
    const int64_t last_chunk_size = dst_size % size;
    if (last_chunk_size > 0) {
      const int64_t offset = cpp_type.size * chunks * size;
      cpp_type.copy_construct_n(
          data->data, POINTER_OFFSET(new_data.data, offset), last_chunk_size);
    }

    return List::create(cpp_type, std::move(new_data), dst_size);
  }
  if (const auto *data = std::get_if<nodes::SingleData>(&list->data())) {
    const CPPType &cpp_type = list->cpp_type();
    return List::create(cpp_type, *data, dst_size);
  }
  BLI_assert_unreachable();
  return {};
}

void execute_multi_function_on_value_variant__list(const MultiFunction &fn,
                                                   const Span<SocketValueVariant *> input_values,
                                                   const Span<SocketValueVariant *> output_values,
                                                   GeoNodesUserData *user_data)
{
  int64_t max_size = 0;
  for (const int i : input_values.index_range()) {
    SocketValueVariant &input_variant = *input_values[i];
    if (input_variant.is_single()) {
      max_size = std::max<int64_t>(max_size, 1);
    }
    else if (input_variant.is_list()) {
      ListPtr list = input_variant.get<ListPtr>();
      max_size = std::max(max_size, list->size());
    }
  }

  const IndexMask mask(max_size);
  mf::ParamsBuilder params{fn, &mask};
  mf::ContextBuilder context;
  context.user_data(user_data);

  Array<ListPtr, 8> repeated_lists(input_values.size());
  for (const int i : input_values.index_range()) {
    const mf::ParamType param_type = fn.param_type(params.next_param_index());
    const CPPType &cpp_type = param_type.data_type().single_type();
    SocketValueVariant &input_variant = *input_values[i];
    if (input_variant.is_single()) {
      const void *value = input_variant.get_single_ptr_raw();
      params.add_readonly_single_input(GPointer{cpp_type, value});
    }
    else if (input_variant.is_list()) {
      repeated_lists[i] = create_repeated_list(input_variant.get<ListPtr>(), max_size);
      const List &list = *repeated_lists[i];
      if (const auto *array_data = std::get_if<nodes::ArrayData>(&list.data())) {
        params.add_readonly_single_input(GSpan(list.cpp_type(), array_data->data, list.size()));
      }
      else if (const auto *single_data = std::get_if<nodes::SingleData>(&list.data())) {
        params.add_readonly_single_input(GPointer(list.cpp_type(), single_data->value));
      }
    }
  }
  for (const int i : output_values.index_range()) {
    if (output_values[i] == nullptr) {
      params.add_ignored_single_output("");
      continue;
    }
    SocketValueVariant &output_variant = *output_values[i];
    const mf::ParamType param_type = fn.param_type(params.next_param_index());
    const CPPType &cpp_type = param_type.data_type().single_type();
    ArrayData array_data = ArrayData::ForUninitialized(cpp_type, max_size);

    params.add_uninitialized_single_output(GMutableSpan(cpp_type, array_data.data, max_size));
    output_variant.set(List::create(cpp_type, std::move(array_data), max_size));
  }
  fn.call(mask, params, context);
}

}  // namespace blender::nodes
