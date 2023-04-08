/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "DNA_node_types.h"

#include "BKE_anonymous_attribute_id.hh"

struct Mesh;

namespace blender::geometry {

struct LoopCutAttributeOutputs {
  std::optional<std::string> loop_cut_edge_selection;
};

std::optional<Mesh *> loop_cut(const Mesh &src_mesh,
                               Span<bool> affected_faces,
                               IndexMask loop_cut_from,
                               OffsetIndices<int> loop_cut_factor_offsets,
                               Span<float> loop_cut_factors,
                               const LoopCutAttributeOutputs &attribute_outputs,
                               const bke::AttributeFilter &attribute_filter);

}  // namespace blender::geometry
