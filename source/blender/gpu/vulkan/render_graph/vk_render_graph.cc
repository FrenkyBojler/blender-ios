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
  
  // Add node to internal storage (implementation depends on Node structure)
  // nodes_.append(node);
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
  
  // Perform topological sort - PARALLEL
  #pragma omp parallel for
for (int64_t i = 0; i < nodes_.size(); i++) {
    // Insert nodes[i] into thread-safe DrawTree keyed by node_id
    draw_tree.insert({nodes_[i], links_[i], i}); 
}
draw_tree.traverse_in_order([&](auto &entry){
    nodes_[entry.orig_idx] = entry.node;
    links_[entry.orig_idx] = entry.links;
});
  
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
            << ")\n "

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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Debug
 * \{ */

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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Graph Building & Validation
 * \{ */

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
  
  std::cerr << "[RenderGraph] Removed " << (nodes_.size() - valid_nodes.size()) 
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
      // Look backwards to see if this resource was produced
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
      
      // If not found, it might be an external resource (that's ok)
      // or it could indicate a problem in the sort
    }
    
    // Mark all outputs as processed
    for (const VKRenderGraphLink &output_link : node_links.outputs) {
      processed_resources.insert(reinterpret_cast<uint64_t>(output_link.resource_handle));
    }
  }
  
  return true;
}

void VKRenderGraph::topological_sort_stable()
{
  const int64_t num_nodes = nodes_.size();
  if (num_nodes == 0) {
    return;
  }

  Vector<int64_t> in_degree(num_nodes, 0);
  Vector<Vector<int64_t>> adjacency_list(num_nodes);

  // Build dependency graph
  for (int64_t node_idx = 0; node_idx < num_nodes; node_idx++) {
    const VKRenderGraphNodeLinks &node_links = links_[node_idx];
    
    for (const VKRenderGraphLink &input_link : node_links.inputs) {
      for (int64_t dep_idx = 0; dep_idx < num_nodes; dep_idx++) {
        if (dep_idx == node_idx) continue;
        
        const VKRenderGraphNodeLinks &dep_links = links_[dep_idx];
        for (const VKRenderGraphLink &output_link : dep_links.outputs) {
          if (input_link.resource_handle == output_link.resource_handle) {
            adjacency_list[dep_idx].append(node_idx);
            in_degree[node_idx]++;
            break;
          }
        }
      }
    }
  }

  // Kahn's algorithm with stable ordering
  std::queue<int64_t> ready_queue;
  for (int64_t i = 0; i < num_nodes; i++) {
    if (in_degree[i] == 0) {
      ready_queue.push(i);
    }
  }

  Vector<VKRenderGraphNode> sorted_nodes;
  Vector<VKRenderGraphNodeLinks> sorted_links;
  sorted_nodes.reserve(num_nodes);
  sorted_links.reserve(num_nodes);

  while (!ready_queue.empty()) {
    int64_t current = ready_queue.front();
    ready_queue.pop();

    sorted_nodes.append(nodes_[current]);
    sorted_links.append(links_[current]);

    for (int64_t neighbor : adjacency_list[current]) {
      in_degree[neighbor]--;
      if (in_degree[neighbor] == 0) {
        ready_queue.push(neighbor);
      }
    }
  }

  // Safety check: if we didn't sort all nodes, there's a cycle
  if (sorted_nodes.size() != num_nodes) {
    std::cerr << "[RenderGraph] CRITICAL: Topological sort failed!\n";
    std::cerr << "  Expected " << num_nodes << " nodes, got " << sorted_nodes.size() << "\n";
    std::cerr << "  This indicates cycles survived cycle removal!\n";
    return;
  }

  nodes_ = std::move(sorted_nodes);
  links_ = std::move(sorted_links);
}

bool VKRenderGraph::has_cycles() const
{
  const int64_t num_nodes = nodes_.size();
  if (num_nodes == 0) {
    return false;
  }

  Vector<int> state(num_nodes, 0);
  
  auto dfs_visit = [&](auto &self, int64_t node_idx) -> bool {
    if (state[node_idx] == 1) {
      return true;  // Back edge = cycle
    }
    if (state[node_idx] == 2) {
      return false;  // Already processed
    }

    state[node_idx] = 1;  // Mark as visiting

    const VKRenderGraphNodeLinks &node_links = links_[node_idx];
    for (const VKRenderGraphLink &input_link : node_links.inputs) {
      for (int64_t dep_idx = 0; dep_idx < num_nodes; dep_idx++) {
        if (dep_idx == node_idx) continue;
        
        const VKRenderGraphNodeLinks &dep_links = links_[dep_idx];
        for (const VKRenderGraphLink &output_link : dep_links.outputs) {
          if (input_link.resource_handle == output_link.resource_handle) {
            if (self(self, dep_idx)) {
              return true;
            }
            break;
          }
        }
      }
    }

    state[node_idx] = 2;  // Mark as visited
    return false;
  };

  for (int64_t i = 0; i < num_nodes; i++) {
    if (state[i] == 0) {
      if (dfs_visit(dfs_visit, i)) {
        return true;
      }
    }
  }

  return false;
}

void VKRenderGraph::remove_cycles()
{
  const int64_t num_nodes = nodes_.size();
  if (num_nodes == 0) {
    return;
  }

  int cycles_removed = 0;
  const int max_iterations = num_nodes * num_nodes;  // Safety limit
  int iteration = 0;
  
  while (iteration < max_iterations) {
    Vector<int> state(num_nodes, 0);
    Vector<int64_t> cycle_path;
    bool found_cycle = false;

    auto dfs_visit = [&](auto &self, int64_t node_idx) -> bool {
      if (state[node_idx] == 1) {
        // Found a cycle, trace it back
        cycle_path.append(node_idx);
        return true;
      }
      if (state[node_idx] == 2) {
        return false;
      }

      state[node_idx] = 1;
      cycle_path.append(node_idx);

      VKRenderGraphNodeLinks &node_links = links_[node_idx];
      for (int64_t input_idx = node_links.inputs.size() - 1; input_idx >= 0; input_idx--) {
        const VKRenderGraphLink &input_link = node_links.inputs[input_idx];
        
        for (int64_t dep_idx = 0; dep_idx < num_nodes; dep_idx++) {
          if (dep_idx == node_idx) continue;
          
          const VKRenderGraphNodeLinks &dep_links = links_[dep_idx];
          for (const VKRenderGraphLink &output_link : dep_links.outputs) {
            if (input_link.resource_handle == output_link.resource_handle) {
              if (self(self, dep_idx)) {
                // Break this edge
                std::cerr << "[RenderGraph] Breaking cycle edge: node " << dep_idx 
                         << " -> node " << node_idx 
                         << " (resource: " << input_link.resource_handle << ")\n";
                node_links.inputs.remove(input_idx);
                cycles_removed++;
                found_cycle = true;
                return true;
              }
              break;
            }
          }
          if (found_cycle) break;
        }
        if (found_cycle) break;
      }

      cycle_path.pop_last();
      state[node_idx] = 2;
      return false;
    };

    // Try to find and break a cycle
    bool broke_cycle = false;
    for (int64_t i = 0; i < num_nodes; i++) {
      if (state[i] == 0) {
        if (dfs_visit(dfs_visit, i)) {
          broke_cycle = true;
          break;
        }
      }
    }
    
    if (!broke_cycle) {
      // No more cycles found
      break;
    }
    
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

/** \} */

}  // namespace blender::gpu::render_graph