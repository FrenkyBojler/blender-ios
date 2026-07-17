/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "NOD_geometry_nodes_debug_view.hh"

namespace blender::nodes::debug_view::tests {

static Identifier identifier(const uint64_t context, const int32_t node)
{
  Identifier identifier;
  identifier.compute_context_hash.v1 = context;
  identifier.compute_context_hash.v2 = context * 17;
  identifier.viewer_node_id = node;
  return identifier;
}

static Candidate candidate(const uint64_t context,
                           const int32_t node,
                           const std::initializer_list<int> sort_order,
                           const std::initializer_list<const char *> context_names,
                           const StringRef viewer_name)
{
  Candidate candidate;
  candidate.identifier = identifier(context, node);
  candidate.sort_order.extend(sort_order);
  for (const char *context_name : context_names) {
    candidate.context_names.append(context_name);
  }
  candidate.viewer_name = viewer_name;
  return candidate;
}

TEST(GeometryNodesDebugView, MeaningfulViewerName)
{
  EXPECT_EQ(viewer_name_from_node("Surface", "Viewer.001"), "Surface");
  EXPECT_EQ(viewer_name_from_node("", "Topology"), "Topology");
  EXPECT_EQ(viewer_name_from_node("", "Viewer"), "");
  EXPECT_EQ(viewer_name_from_node("", "Viewer.001"), "");
}

TEST(GeometryNodesDebugView, DeterministicTreeOrder)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(30, 3, {2}, {}, "Third"));
  candidates.append(candidate(10, 1, {1, 5}, {"Group"}, "Second"));
  candidates.append(candidate(20, 2, {1, 3}, {"Group"}, "First"));

  finalize_candidates(candidates);

  EXPECT_EQ(candidates[0].viewer_name, "First");
  EXPECT_EQ(candidates[1].viewer_name, "Second");
  EXPECT_EQ(candidates[2].viewer_name, "Third");
}

TEST(GeometryNodesDebugView, DuplicateNamesUseContextThenSuffix)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "Surface"));
  candidates.append(candidate(20, 2, {2, 1}, {"Inner Group"}, "Surface"));
  candidates.append(candidate(30, 3, {3, 1}, {"Inner Group"}, "Surface"));

  finalize_candidates(candidates);

  EXPECT_EQ(candidates[0].display_name, "Surface");
  EXPECT_EQ(candidates[1].display_name, "Inner Group / Surface");
  EXPECT_EQ(candidates[2].display_name, "Inner Group / Surface (2)");
  EXPECT_EQ(candidates[1].full_name, "Inner Group / Surface");
}

TEST(GeometryNodesDebugView, DuplicateIdentityIsRemoved)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "Surface"));
  candidates.append(candidate(10, 1, {1}, {}, "Surface"));

  finalize_candidates(candidates);

  EXPECT_EQ(candidates.size(), 1);
}

TEST(GeometryNodesDebugView, GeometryEligibility)
{
  EXPECT_TRUE(is_geometry_candidate(true, true, true));
  EXPECT_FALSE(is_geometry_candidate(true, false, true));
  EXPECT_FALSE(is_geometry_candidate(true, true, false));
  EXPECT_FALSE(is_geometry_candidate(false, true, true));
}

TEST(GeometryNodesDebugView, ResolvePreservesOrFallsBack)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "First"));
  candidates.append(candidate(20, 2, {2}, {}, "Second"));
  finalize_candidates(candidates);

  std::optional<Identifier> selection = candidates[1].identifier;
  EXPECT_EQ(resolve_candidate_index(candidates.as_span(), selection), 1);
  EXPECT_EQ(selection, candidates[1].identifier);

  Vector<Candidate> reevaluated_candidates = candidates;
  EXPECT_EQ(resolve_candidate_index(reevaluated_candidates.as_span(), selection), 1);

  reevaluated_candidates.remove(1);
  EXPECT_EQ(resolve_candidate_index(reevaluated_candidates.as_span(), selection), 0);
  EXPECT_EQ(selection, reevaluated_candidates[0].identifier);
}

TEST(GeometryNodesDebugView, ZeroOneAndManySelection)
{
  std::optional<Identifier> selection = identifier(99, 99);
  EXPECT_EQ(resolve_candidate_index(Span<Candidate>(), selection), -1);
  EXPECT_FALSE(selection.has_value());

  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "First"));
  candidates.append(candidate(20, 2, {2}, {}, "Second"));
  candidates.append(candidate(30, 3, {3}, {}, "Third"));
  finalize_candidates(candidates);

  EXPECT_EQ(resolve_candidate_index(candidates.as_span().take_front(1), selection), 0);
  EXPECT_EQ(cycle_candidate_index(candidates.as_span(), 1, selection), 1);
  EXPECT_EQ(cycle_candidate_index(candidates.as_span(), 1, selection), 2);
  EXPECT_EQ(cycle_candidate_index(candidates.as_span(), 1, selection), 0);
  EXPECT_EQ(cycle_candidate_index(candidates.as_span(), -1, selection), 2);
  EXPECT_EQ(select_candidate_index(candidates.as_span(), 1, selection), 1);
  EXPECT_EQ(selection, candidates[1].identifier);
  EXPECT_EQ(select_candidate_index(candidates.as_span(), 10, selection), -1);
  EXPECT_EQ(selection, candidates[1].identifier);
}

TEST(GeometryNodesDebugView, SelectionStateIsIndependent)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "First"));
  candidates.append(candidate(20, 2, {2}, {}, "Second"));
  finalize_candidates(candidates);

  std::optional<Identifier> modifier_a;
  std::optional<Identifier> modifier_b;
  select_candidate_index(candidates.as_span(), 0, modifier_a);
  select_candidate_index(candidates.as_span(), 1, modifier_b);
  cycle_candidate_index(candidates.as_span(), 1, modifier_a);

  EXPECT_EQ(modifier_a, candidates[1].identifier);
  EXPECT_EQ(modifier_b, candidates[1].identifier);
  cycle_candidate_index(candidates.as_span(), 1, modifier_a);
  EXPECT_EQ(modifier_a, candidates[0].identifier);
  EXPECT_EQ(modifier_b, candidates[1].identifier);
}

}  // namespace blender::nodes::debug_view::tests
