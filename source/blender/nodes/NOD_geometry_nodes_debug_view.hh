/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>
#include <string>

#include "BLI_compute_context.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

namespace blender::nodes {

namespace eval_log {
class ViewerNodeLog;
}

namespace debug_view {

/** Prefer a custom node label, then a meaningfully customized node name. */
std::string viewer_name_from_node(StringRef label, StringRef node_name);

/** Decide whether logged Viewer data can replace modifier geometry. */
bool is_geometry_candidate(bool is_debug_view, bool is_shown, bool has_geometry);

/** Stable identity of a Viewer node within one Geometry Nodes modifier evaluation. */
struct Identifier {
  ComputeContextHash compute_context_hash;
  int32_t viewer_node_id = 0;

  friend bool operator==(const Identifier &, const Identifier &) = default;
};

/**
 * A selectable persistent geometry Debug View.
 *
 * The log pointer is only valid while the owning #NodesEvalLog is kept alive. It is never stored
 * as modifier selection state.
 */
struct Candidate {
  Identifier identifier;
  const eval_log::ViewerNodeLog *viewer_log = nullptr;

  /** Node order at every nested group level, followed by the Viewer node order. */
  Vector<int> sort_order;
  /** Human-readable names of the nested group instances. */
  Vector<std::string> context_names;
  std::string viewer_name;

  /** Concise unique name shown in the modifier. */
  std::string display_name;
  /** Complete context path used in tooltips. */
  std::string full_name;
};

/** Sort candidates deterministically, remove duplicate identities, and disambiguate their names.
 */
void finalize_candidates(Vector<Candidate> &candidates);

/** Keep a valid selection or choose the first candidate as deterministic fallback. */
int resolve_candidate_index(Span<Candidate> candidates, std::optional<Identifier> &selection);

/** Select an exact candidate index when it is valid. */
int select_candidate_index(Span<Candidate> candidates,
                           int index,
                           std::optional<Identifier> &selection);

/** Cycle relative to the resolved selection, wrapping at both ends. */
int cycle_candidate_index(Span<Candidate> candidates,
                          int step,
                          std::optional<Identifier> &selection);

}  // namespace debug_view
}  // namespace blender::nodes
