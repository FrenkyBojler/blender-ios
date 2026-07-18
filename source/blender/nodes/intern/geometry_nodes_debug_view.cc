/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>

#include <fmt/format.h>

#include "BLI_string_utils.hh"

#include "NOD_geometry_nodes_debug_view.hh"

namespace blender::nodes::debug_view {

std::string viewer_name_from_node(const StringRef label, const StringRef node_name)
{
  if (!label.is_empty()) {
    return label;
  }
  int name_number;
  const StringRef name_base = BLI_string_split_name_number(node_name, '.', name_number);
  if (name_base != "Viewer") {
    return node_name;
  }
  return {};
}

bool is_geometry_candidate(const bool is_debug_view, const bool is_shown, const bool has_geometry)
{
  return is_debug_view && is_shown && has_geometry;
}

static bool candidate_less(const Candidate &a, const Candidate &b)
{
  if (std::lexicographical_compare(
          a.sort_order.begin(), a.sort_order.end(), b.sort_order.begin(), b.sort_order.end()))
  {
    return true;
  }
  if (std::lexicographical_compare(
          b.sort_order.begin(), b.sort_order.end(), a.sort_order.begin(), a.sort_order.end()))
  {
    return false;
  }
  if (a.identifier.compute_context_hash.v1 != b.identifier.compute_context_hash.v1) {
    return a.identifier.compute_context_hash.v1 < b.identifier.compute_context_hash.v1;
  }
  if (a.identifier.compute_context_hash.v2 != b.identifier.compute_context_hash.v2) {
    return a.identifier.compute_context_hash.v2 < b.identifier.compute_context_hash.v2;
  }
  return a.identifier.viewer_node_id < b.identifier.viewer_node_id;
}

static std::string full_name_for_candidate(const Candidate &candidate)
{
  std::string name;
  for (const std::string &context_name : candidate.context_names) {
    if (!name.empty()) {
      name += " / ";
    }
    name += context_name;
  }
  if (!name.empty()) {
    name += " / ";
  }
  name += candidate.viewer_name;
  return name;
}

void finalize_candidates(Vector<Candidate> &candidates)
{
  std::ranges::sort(candidates, candidate_less);

  Vector<Candidate> unique_candidates;
  unique_candidates.reserve(candidates.size());
  for (Candidate &candidate : candidates) {
    if (!unique_candidates.is_empty() &&
        unique_candidates.last().identifier == candidate.identifier)
    {
      continue;
    }
    unique_candidates.append(std::move(candidate));
  }
  candidates = std::move(unique_candidates);

  for (Candidate &candidate : candidates) {
    candidate.full_name = full_name_for_candidate(candidate);
    candidate.display_name = candidate.viewer_name;
  }

  /* Add context only where the Viewer name alone is ambiguous. */
  for (const int i : candidates.index_range()) {
    const bool has_duplicate_name = std::ranges::any_of(candidates, [&](const Candidate &other) {
      return &other != &candidates[i] && other.viewer_name == candidates[i].viewer_name;
    });
    if (has_duplicate_name) {
      candidates[i].display_name = candidates[i].full_name;
    }
  }

  /* Identical context paths are possible when the same group is instantiated more than once. */
  Vector<std::string> names_before_suffix;
  names_before_suffix.reserve(candidates.size());
  for (const Candidate &candidate : candidates) {
    names_before_suffix.append(candidate.display_name);
  }
  for (const int i : candidates.index_range()) {
    int duplicate_number = 1;
    for (const int previous_i : IndexRange(i)) {
      if (names_before_suffix[previous_i] == names_before_suffix[i]) {
        duplicate_number++;
      }
    }
    if (duplicate_number > 1) {
      candidates[i].display_name = fmt::format(
          "{} ({})", names_before_suffix[i], duplicate_number);
    }
  }
}

int find_candidate_index(const Span<Candidate> candidates,
                         const std::optional<Identifier> &selection)
{
  if (candidates.is_empty()) {
    return -1;
  }
  if (selection) {
    for (const int i : candidates.index_range()) {
      if (candidates[i].identifier == *selection) {
        return i;
      }
    }
  }
  return 0;
}

int resolve_candidate_index(const Span<Candidate> candidates, std::optional<Identifier> &selection)
{
  const int index = find_candidate_index(candidates, selection);
  if (index == -1) {
    selection.reset();
    return -1;
  }
  selection = candidates[index].identifier;
  return index;
}

int select_candidate(const Span<Candidate> candidates,
                     const Identifier &identifier,
                     std::optional<Identifier> &selection)
{
  for (const int i : candidates.index_range()) {
    if (candidates[i].identifier == identifier) {
      selection = identifier;
      return i;
    }
  }
  return -1;
}

int cycle_candidate_index(const Span<Candidate> candidates,
                          const int step,
                          std::optional<Identifier> &selection)
{
  const int current_index = resolve_candidate_index(candidates, selection);
  if (current_index == -1) {
    return -1;
  }
  const int candidates_num = candidates.size();
  const int next_index = ((current_index + step) % candidates_num + candidates_num) %
                         candidates_num;
  selection = candidates[next_index].identifier;
  return next_index;
}

}  // namespace blender::nodes::debug_view
