/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#pragma once

#include "FN_multi_function.hh"

#include "NOD_geometry_exec.hh"

namespace blender::nodes {

namespace list::multi_function_eval {

class ListFieldContext : public fn::FieldContext {
 public:
  ListFieldContext() = default;

  GVArray get_varray_for_input(const fn::FieldInput &field_input,
                               const IndexMask &mask,
                               ResourceScope & /*scope*/) const override;
};

/**
 * An input can either be a single value, a grid or a field (which is evaluated for each
 * voxel/tile).
 */
using InputVariant = std::variant<GPointer, const nodes::GList *, const fn::GField *>;

struct EvalResult {
  struct Success {
    /** The computed grids. A grid may be null if it was not required (see #output_usages). */
    Array<nodes::GListPtr> output_lists;
  };
  struct Failure {
    std::string error_message;
  };

  std::variant<Success, Failure> result;
};

/**
 * Evaluate a multi-function on the given inputs. At least one of the inputs must be a list or
 * this will return a failure.
 *
 * \param output_usages: A boolean for each output indicating whether the output is required.
 */
EvalResult evaluate_multi_function_on_list(const mf::MultiFunction &fn,
                                           Span<InputVariant> input_values,
                                           Span<bool> output_usages);
}  // namespace list::multi_function_eval

bool execute_multi_function_on_value_variant__list(const MultiFunction &fn,
                                                   Span<SocketValueVariant *> input_values,
                                                   Span<SocketValueVariant *> output_values);

GListPtr evaluate_field_to_list(GField field, int64_t count);

}  // namespace blender::nodes
