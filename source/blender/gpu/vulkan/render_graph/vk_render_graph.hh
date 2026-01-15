/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_render_graph.hh"
#include "gpu_backend.hh"

#include <sstream>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <iostream>

namespace blender::gpu::render_graph {

VKRenderGraph::VKRenderGraph(VKResourceStateTracker &resources) : resources_(resources) {}

void VKRenderGraph::add_node(const Node &node)
{
  if (built_) {
    std::cerr << "[RenderGraph] ERROR: add_node called after build()!\n";
    std::cerr << "  This would corrupt Vulkan command state and cause crashes.\n";
    std::cerr << "  Automatically invalidating build and rebuilding...\n";

    built_ = false;
  }

  // Append to internal storage. Actual storage type (Vector<...>) is defined in header.
  nodes_.append(node);
  links_.append(VKRenderGraphNodeLinks());
}

void VKRenderGraph::build()
{
  if (built_) {
    std::cerr << "[RenderGraph] Warning: build() called on already built graph, skipping.\n";
    return;
  }

  if (nodes_.is_empty()) {
    std::cerr << "[RenderGraph] Warning: Building empty graph.\n";
    built_ = true;
    return;
  }

  // Validate all resource handles before building
  if (!validate_resources()) {
    std::cerr << "[RenderGraph] ERROR: Invalid resource handles detected!\n";
    std::cerr << "  Cannot build graph with null or invalid resources.\n";
    std::cerr << "  Attempting to fix by removing invalid nodes...\n";
    remove_invalid_nodes();
  }

  // Check for cycles BEFORE sorting
  if (has_cycles()) {
    std::cerr << "[RenderGraph] ERROR: Cycles detected in graph!\n";
    std::cerr << "  Execution would hang or crash. Automatically removing cycles...\n";
    remove_cycles();

    // Verify cycles were actually removed
    if (has_cycles()) {
      std::cerr << "[RenderGraph] CRITICAL: Failed to remove all cycles!\n";
      std::cerr << "  Graph is unsafe for execution. Aborting build.\n";
      return;
    }
    std::cerr << "[RenderGraph] Success: All cycles removed.\n";
  }

  // Perform topological sort
  topological_sort_stable();

  // Final validation
  if (!validate_execution_order()) {
    std::cerr << "[RenderGraph] ERROR: Invalid execution order after sorting!\n";
    std::cerr << "  This indicates a critical graph consistency error.\n";
    return;
  }

  built_ = true;
  std::cerr << "[RenderGraph] Build complete: " << nodes_.size() << " nodes ready for execution.\n";
}

void VKRenderGraph::reset()
{
#if 0
  memstats();
#endif
  links_.clear_and_shrink();
  for (VKRenderGraphNode &node : nodes_) {
    node.free_data(storage_);
  }
  nodes_.clear_and_shrink();
  storage_.reset();
  debug_.node_group_map.clear();
  debug_.used_groups.clear();
  debug_.group_stack.clear();
  debug_.groups.clear();

  built_ = false;
}

void VKRenderGraph::memstats() const
{
  std::cout << __func__ << " nodes: (" << nodes_.size() << "/" << nodes_.capacity() << "), "
            << "links: (" << links_.size() << "/" << links_.capacity() << ")\n";
#define PRINT_STORAGE(name) \
  std::cout << " " #name " : (" << storage_.name.size() << " / " << storage_.name.capacity() \
            << ")\n"
  PRINT_STORAGE(begin_rendering);
  PRINT_STORAGE(clear_attachments);
  PRINT_STORAGE(blit_image);
  PRINT_STORAGE(copy_buffer_to_image);
  PRINT_STORAGE(copy_image);
  PRINT_STORAGE(copy_image_to_buffer);
  PRINT_STORAGE(draw);
  PRINT_STORAGE(draw_indexed);
  PRINT_STORAGE(draw_indexed_indirect);
  PRINT_STORAGE(draw_indirect);
#undef PRINT_STORAGE
}

/* -------------------------------------------------------------------- */
/** \name Debug */

void VKRenderGraph::debug_group_begin(const char *name, const ColorTheme4f &color)
{
  ColorTheme4f useColor = color;
  if ((color == gpu::debug::GPU_DEBUG_GROUP_COLOR_DEFAULT) && (debug_.group_stack.size() > 0)) {
    useColor = debug_.groups[debug_.group_stack.last()].color;
  }
  DebugGroupNameID name_id = debug_.groups.index_of_or_add({std::string(name), useColor});
  debug_.group_stack.append(name_id);
  debug_.group_used = false;
}

void VKRenderGraph::debug_group_end()
{
  debug_.group_stack.pop_last();
  debug_.group_used = false;
}

void VKRenderGraph::debug_print(NodeHandle node_handle) const
{
  std::ostream &os = std::cout;
  os << "NODE:\n";
  const VKRenderGraphNode &node = nodes_[node_handle];
  os << "  type:" << node.type << "\n";
  const VKRenderGraphNodeLinks &links = links_[node_handle];
  os << " inputs:\n";
  for (const VKRenderGraphLink &link : links.inputs) {
    os << "  ";
    link.debug_print(os, resources_);
    os << "\n";
  }
  os << " outputs:\n";
  for (const VKRenderGraphLink &link : links.outputs) {
    os << "  ";
    link.debug_print(os, resources_);
    os << "\n";
  }
}

std::string VKRenderGraph::full_debug_group(NodeHandle node_handle) const
{
  if ((G.debug & G_DEBUG_GPU) == 0) {
    return std::string();
  }

  DebugGroupID debug_group = debug_.node_group_map[node_handle];
  if (debug_group == -1) {
    return std::string();
  }

  std::stringstream ss;
  for (const VKRenderGraph::DebugGroupNameID &name_id : debug_.used_groups[debug_group]) {
    ss << "/" << debug_.groups[name_id].name;
  }
  return ss.str();
}

/* -------------------------------------------------------------------- */
/** \name Graph Building & Validation */

bool VKRenderGraph::validate_resources() const
{
  bool all_valid = true;

  for (int64_t node_idx = 0; node_idx < nodes_.size(); node_idx++) {
    const VKRenderGraphNodeLinks &node_links = links_[node_idx];

    // Check all input resources
    for (const VKRenderGraphLink &link : node_links.inputs) {
      if (link.resource_handle == VK_NULL_HANDLE) {
        std::cerr << "[RenderGraph] Node " << node_idx << " has null input resource!\n";
        all_valid = false;
      }
    }

    // Check all output resources
    for (const VKRenderGraphLink &link : node_links.outputs) {
      if (link.resource_handle == VK_NULL_HANDLE) {
        std::cerr << "[RenderGraph] Node " << node_idx << " has null output resource!\n";
        all_valid = false;
      }
    }
  }

  return all_valid;
}

void VKRenderGraph::remove_invalid_nodes()
{
  const int64_t original_size = nodes_.size();
  Vector<VKRenderGraphNode> valid_nodes;
  Vector<VKRenderGraphNodeLinks> valid_links;

  valid_nodes.reserve(nodes_.size());
  valid_links.reserve(links_.size());

  for (int64_t node_idx = 0; node_idx < nodes_.size(); node_idx++) {
    const VKRenderGraphNodeLinks &node_links = links_[node_idx];
    bool is_valid = true;

    // Check if any resources are invalid
    for (const VKRenderGraphLink &link : node_links.inputs) {
      if (link.resource_handle == VK_NULL_HANDLE) {
        is_valid = false;
        break;
      }
    }

    if (is_valid) {
      for (const VKRenderGraphLink &link : node_links.outputs) {
        if (link.resource_handle == VK_NULL_HANDLE) {
          is_valid = false;
          break;
        }
      }
    }

    if (is_valid) {
      valid_nodes.append(nodes_[node_idx]);
      valid_links.append(node_links);
    } else {
      std::cerr << "[RenderGraph] Removing invalid node at index " << node_idx << "\n";
      // Free the node data before removing
      nodes_[node_idx].free_data(storage_);
    }
  }

  nodes_ = std::move(valid_nodes);
  links_ = std::move(valid_links);

  std::cerr << "[RenderGraph] Removed " << (original_size - nodes_.size())
            << " invalid nodes.\n";
}

bool VKRenderGraph::validate_execution_order() const
{
  const int64_t num_nodes = nodes_.size();
  if (num_nodes == 0) {
    return true;
  }

  std::unordered_set<uint64_t> processed_resources;

  for (int64_t node_idx = 0; node_idx < num_nodes; node_idx++) {
    const VKRenderGraphNodeLinks &node_links = links_[node_idx];

    // Check that all input resources have been produced by previous nodes
    for (const VKRenderGraphLink &input_link : node_links.inputs) {
      bool found = false;
      for (int64_t prev_idx = 0; prev_idx < node_idx; prev_idx++) {
        const VKRenderGraphNodeLinks &prev_links = links_[prev_idx];
        for (const VKRenderGraphLink &output_link : prev_links.outputs) {
          if (input_link.resource_handle == output_link.resource_handle) {
            found = true;
            break;
          }
        }
        if (found) break;
      }
      // If not found, it might be an external resource (that's acceptable).
    }

    // Mark all outputs as processed
    for (const VKRenderGraphLink &output_link : node_links.outputs) {
      processed_resources.insert(reinterpret_cast<uint64_t>(output_link.resource_handle));
    }
  }

  return true;
}


/** Optimized Linear Build & Stable Sort */
void VKRenderGraph::topological_sort_stable()
{
  const int64_t num_nodes = nodes_.size();
  if (num_nodes == 0) return;

  std::unordered_map<void*, int64_t> producer_map;
  for (int64_t i = 0; i < num_nodes; i++) {
    for (const auto &out : links_[i].outputs) {
      producer_map[out.resource_handle] = i;
    }
  }

  Vector<int64_t> in_degree(num_nodes, 0);
  Vector<Vector<int64_t>> adj(num_nodes);
  for (int64_t i = 0; i < num_nodes; i++) {
    for (const auto &in : links_[i].inputs) {
      if (producer_map.contains(in.resource_handle)) {
        int64_t producer = producer_map[in.resource_handle];
        if (producer != i) {
          adj[producer].append(i);
          in_degree[i]++;
        }
      }
    }
  }

  std::queue<int64_t> q;
  for (int64_t i = 0; i < num_nodes; i++) if (in_degree[i] == 0) q.push(i);

  Vector<VKRenderGraphNode> sorted_n;
  Vector<VKRenderGraphNodeLinks> sorted_l;
  sorted_n.reserve(num_nodes);
  sorted_l.reserve(num_nodes);

  while (!q.empty()) {
    int64_t curr = q.front(); q.pop();
    sorted_n.append(nodes_[curr]);
    sorted_l.append(links_[curr]);
    for (int64_t next : adj[curr]) {
      if (--in_degree[next] == 0) q.push(next);
    }
  }

  if (sorted_n.size() == num_nodes) {
    nodes_ = std::move(sorted_n);
    links_ = std::move(sorted_l);
  } else {
    std::cerr << "[RenderGraph] CRITICAL: Topological sort failed (partial order).\n";
  }
}


bool VKRenderGraph::has_cycles() const
{
  const int64_t num_nodes = nodes_.size();
  if (num_nodes == 0) return false;

  std::unordered_map<void*, int64_t> producer_map;
  for (int64_t i = 0; i < num_nodes; i++) {
    for (const auto &out : links_[i].outputs) producer_map[out.resource_handle] = i;
  }

  Vector<int> state(num_nodes, 0); // 0: unvisited, 1: visiting, 2: visited
  auto dfs = [&](auto &self, int64_t i) -> bool {
    state[i] = 1;
    for (const auto &in : links_[i].inputs) {
      if (producer_map.contains(in.resource_handle)) {
        int64_t p = producer_map[in.resource_handle];
        if (state[p] == 1) return true;
        if (state[p] == 0 && self(self, p)) return true;
      }
    }
    state[i] = 2;
    return false;
  };

  for (int64_t i = 0; i < num_nodes; i++) {
    if (state[i] == 0 && dfs(dfs, i)) return true;
  }
  return false;
}

void VKRenderGraph::remove_cycles()
{
  const int64_t num_nodes = nodes_.size();
  if (num_nodes == 0) return;

  int cycles_removed = 0;
  const int max_iterations = num_nodes * num_nodes;  // Safety limit
  int iteration = 0;

  while (iteration < max_iterations) {
    Vector<int> state(num_nodes, 0);
    bool found_cycle = false;

    auto dfs_break = [&](auto &self, int64_t i) -> bool {
      state[i] = 1;
      auto &in_links = links_[i].inputs;
      for (int64_t k = static_cast<int64_t>(in_links.size()) - 1; k >= 0; k--) {
        if (in_links[k].resource_handle == VK_NULL_HANDLE) continue;
        // Find producer for this resource.
        for (int64_t p = 0; p < num_nodes; p++) {
          if (p == i) continue;
          const VKRenderGraphNodeLinks &pl = links_[p];
          for (const VKRenderGraphLink &out : pl.outputs) {
            if (out.resource_handle == in_links[k].resource_handle) {
              if (state[p] == 1) {
                // break this edge to remove cycle
                std::cerr << "[RenderGraph] Breaking cycle edge: node " << p
                          << " -> node " << i << " (resource: " << in_links[k].resource_handle << ")\n";
                in_links.remove(k);
                cycles_removed++;
                found_cycle = true;
                return true;
              }
              if (state[p] == 0 && self(self, p)) return true;
              break;
            }
          }
          if (found_cycle) break;
        }
        if (found_cycle) break;
      }
      state[i] = 2;
      return false;
    };

    bool broke_cycle = false;
    for (int64_t i = 0; i < num_nodes; i++) {
      if (state[i] == 0) {
        if (dfs_break(dfs_break, i)) {
          broke_cycle = true;
          break;
        }
      }
    }

    if (!broke_cycle) break;
    iteration++;
  }

  if (iteration >= max_iterations) {
    std::cerr << "[RenderGraph] CRITICAL: Exceeded max iterations (" << max_iterations
              << ") while removing cycles!\n";
    std::cerr << "  Graph may still contain cycles. Build will fail.\n";
  } else {
    std::cerr << "[RenderGraph] Successfully removed " << cycles_removed
              << " cycle edges in " << iteration << " iterations.\n";
  }
}

} // namespace blender::gpu::render_graph
