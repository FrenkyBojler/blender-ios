/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <string>

struct SpaceNode;
struct bNode;
struct ReportList;
struct bContext;
struct bNodeTree;
struct Main;

namespace blender::nodes {

void sync_sockets_evaluate_closure(SpaceNode &snode,
                                   bNode &evaluate_closure_node,
                                   ReportList *reports);
void sync_sockets_separate_bundle(SpaceNode &snode,
                                  bNode &separate_bundle_node,
                                  ReportList *reports);
void sync_sockets_combine_bundle(SpaceNode &snode,
                                 bNode &combine_bundle_node,
                                 ReportList *reports);
void sync_sockets_closure(SpaceNode &snode,
                          bNode &closure_input_node,
                          bNode &closure_output_node,
                          const bool initialize_internal_links,
                          ReportList *reports);

enum class NodeSyncState {
  Synced,
  CanBeSynced,
  NoSyncSource,
  ConflictingSyncSources,
};

NodeSyncState sync_sockets_state_separate_bundle(const SpaceNode &snode,
                                                 const bNode &separate_bundle_node);
NodeSyncState sync_sockets_state_combine_bundle(const SpaceNode &snode,
                                                const bNode &combine_bundle_node);
NodeSyncState sync_sockets_state_closure_output(const SpaceNode &snode,
                                                const bNode &closure_output_node);
NodeSyncState sync_sockets_state_evaluate_closure(const SpaceNode &snode,
                                                  const bNode &evaluate_closure_node);

bool node_can_sync_sockets(const bContext &C, const bNodeTree &tree, const bNode &node);
void node_can_sync_cache_clear(Main &bmain);

void sync_node(bContext &C, bNode &node, ReportList *reports);
std::string sync_node_description_get(const bContext &C, const bNode &node);

}  // namespace blender::nodes
