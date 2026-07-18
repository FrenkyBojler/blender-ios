/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_compute_contexts.hh"
#include "BKE_gtest_setup.hh"

#include "NOD_eval_log.hh"
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

  EXPECT_EQ(candidate_display_name(candidates, 0, "Viewer"), "Surface");
  EXPECT_EQ(candidate_display_name(candidates, 1, "Viewer"), "Inner Group / Surface");
  EXPECT_EQ(candidate_display_name(candidates, 2, "Viewer"), "Inner Group / Surface (2)");
  EXPECT_EQ(candidate_full_name(candidates[1], "Viewer"), "Inner Group / Surface");
}

TEST(GeometryNodesDebugView, DefaultNameIsTranslatedAtPresentationTime)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {"Default Group"}, ""));
  candidates.append(candidate(20, 2, {2}, {"Custom Group"}, "Viewer"));
  finalize_candidates(candidates);

  EXPECT_TRUE(candidates[0].viewer_name.empty());
  EXPECT_EQ(candidate_display_name(candidates, 0, "Translated Viewer"), "Translated Viewer");
  EXPECT_EQ(candidate_display_name(candidates, 1, "Translated Viewer"), "Viewer");

  EXPECT_EQ(candidate_display_name(candidates, 0, "Viewer"), "Default Group / Viewer");
  EXPECT_EQ(candidate_display_name(candidates, 1, "Viewer"), "Custom Group / Viewer");
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

TEST(GeometryNodesDebugView, FindFallbackDoesNotChangeSelection)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "First"));
  candidates.append(candidate(20, 2, {2}, {}, "Second"));
  finalize_candidates(candidates);

  std::optional<Identifier> selection = identifier(99, 99);
  EXPECT_EQ(find_candidate_index(candidates, selection), 0);
  EXPECT_EQ(selection, identifier(99, 99));
  EXPECT_EQ(find_candidate_index({}, selection), -1);
  EXPECT_EQ(selection, identifier(99, 99));
}

TEST(GeometryNodesDebugView, PreferredSelectionReturnsAfterFallback)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "First"));
  candidates.append(candidate(20, 2, {2}, {}, "Preferred"));
  finalize_candidates(candidates);

  const Identifier preferred = candidates[1].identifier;
  std::optional<Identifier> selection = preferred;
  EXPECT_EQ(find_candidate_index(candidates.as_span().take_front(1), selection), 0);
  EXPECT_EQ(selection, preferred);

  EXPECT_EQ(find_candidate_index(candidates, selection), 1);
  EXPECT_EQ(selection, preferred);
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
  EXPECT_EQ(select_candidate(candidates, candidates[1].identifier, selection), 1);
  EXPECT_EQ(selection, candidates[1].identifier);
  EXPECT_EQ(select_candidate(candidates, identifier(99, 99), selection), -1);
  EXPECT_EQ(selection, candidates[1].identifier);
}

TEST(GeometryNodesDebugView, SelectionUsesStableIdentity)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "First"));
  candidates.append(candidate(20, 2, {2}, {}, "Second"));
  candidates.append(candidate(30, 3, {3}, {}, "Third"));
  finalize_candidates(candidates);

  const Identifier clicked_identifier = candidates[1].identifier;
  std::optional<Identifier> selection = candidates[0].identifier;
  Vector<Candidate> reevaluated_candidates = {candidates[2], candidates[0]};
  EXPECT_EQ(select_candidate(reevaluated_candidates, clicked_identifier, selection), -1);
  EXPECT_EQ(selection, candidates[0].identifier);

  reevaluated_candidates.append(candidates[1]);
  EXPECT_EQ(select_candidate(reevaluated_candidates, clicked_identifier, selection), 2);
  EXPECT_EQ(selection, clicked_identifier);
}

TEST(GeometryNodesDebugView, SelectionStateIsIndependent)
{
  Vector<Candidate> candidates;
  candidates.append(candidate(10, 1, {1}, {}, "First"));
  candidates.append(candidate(20, 2, {2}, {}, "Second"));
  finalize_candidates(candidates);

  std::optional<Identifier> modifier_a;
  std::optional<Identifier> modifier_b;
  select_candidate(candidates, candidates[0].identifier, modifier_a);
  select_candidate(candidates, candidates[1].identifier, modifier_b);
  cycle_candidate_index(candidates.as_span(), 1, modifier_a);

  EXPECT_EQ(modifier_a, candidates[1].identifier);
  EXPECT_EQ(modifier_b, candidates[1].identifier);
  cycle_candidate_index(candidates.as_span(), 1, modifier_a);
  EXPECT_EQ(modifier_a, candidates[0].identifier);
  EXPECT_EQ(modifier_b, candidates[1].identifier);
}

static void add_viewer_log(eval_log::NodeTreeLogger &tree_logger,
                           const int32_t node_id,
                           const int node_order,
                           const StringRef name)
{
  destruct_ptr<eval_log::ViewerNodeLog> viewer_log =
      tree_logger.allocator->construct<eval_log::ViewerNodeLog>();
  viewer_log->is_debug_view = true;
  viewer_log->node_order = node_order;
  viewer_log->debug_view_name = name;
  viewer_log->items.add_new({0, "Geometry", bke::SocketValueVariant::From(bke::GeometrySet())});
  tree_logger.viewer_node_logs.append(*tree_logger.allocator, {node_id, std::move(viewer_log)});
}

class CandidateCollectionTest : public testing::Test {
 public:
  static void SetUpTestSuite()
  {
    bke::gtest_setup();
  }

  static void TearDownTestSuite()
  {
    bke::gtest_teardown();
  }
};

TEST_F(CandidateCollectionTest, IsCached)
{
  eval_log::NodesEvalLog root_log;
  bke::DataBlockComputeContext compute_context(nullptr, 1);
  eval_log::NodeTreeLogger &tree_logger = root_log.get_local_tree_logger(compute_context);
  add_viewer_log(tree_logger, 20, 2, "Second");
  add_viewer_log(tree_logger, 10, 1, "First");

  const Span<Candidate> first_candidates = root_log.debug_view_candidates();
  ASSERT_EQ(first_candidates.size(), 2);
  EXPECT_EQ(candidate_display_name(first_candidates, 0, "Viewer"), "First");
  EXPECT_EQ(candidate_display_name(first_candidates, 1, "Viewer"), "Second");

  const Span<Candidate> second_candidates = root_log.debug_view_candidates();
  EXPECT_EQ(second_candidates.data(), first_candidates.data());
  EXPECT_EQ(second_candidates[0].identifier, first_candidates[0].identifier);
  EXPECT_EQ(second_candidates[1].identifier, first_candidates[1].identifier);
}

}  // namespace blender::nodes::debug_view::tests
