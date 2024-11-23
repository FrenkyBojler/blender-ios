/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_execute.hh"
#include "NOD_geometry_nodes_lazy_function.hh"
#include "NOD_node_declaration.hh"
#include "NOD_socket.hh"

#include "BKE_compute_contexts.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_geometry_set.hh"
#include "BKE_idprop.hh"
#include "BKE_node_enum.hh"
#include "BKE_node_socket_value.hh"

#include "FN_lazy_function_execute.hh"

#include "editors/sculpt_paint/sculpt_intern.hh"

namespace blender::ed::sculpt_paint {
  void sculpting_geo_nodes_execute(const Depsgraph &depsgraph,
    Object &object,
    StrokeCache& cache,
    Span<float3> positions,
    Span<int> indices,
    MutableSpan<float3> translations);
}
