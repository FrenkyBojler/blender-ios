/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef WITH_OPENVDB

#  include <openvdb/openvdb.h>
#  include <variant>

#  include "BLI_generic_pointer.hh"

#  include "FN_field.hh"

namespace blender::bke::volume_grid::multi_function_eval {

/**
 * An input can either be a single value, a grid or a field (which is evaluated for each
 * voxel/tile).
 */
using InputVariant = std::variant<GPointer, const openvdb::GridBase *, const fn::GField *>;

struct EvalResult {
  struct Success {
    Array<openvdb::GridBase::Ptr> output_grids;
  };
  struct Failure {
    std::string error_message;
  };

  std::variant<Success, Failure> result;
};

EvalResult evaluate_multi_function_on_grid(const mf::MultiFunction &fn,
                                           const Span<InputVariant> input_values,
                                           const Span<bool> output_usages);

}  // namespace blender::bke::volume_grid::multi_function_eval

#endif
