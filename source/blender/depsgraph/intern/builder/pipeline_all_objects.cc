/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "pipeline_all_objects.h"

#include "intern/builder/deg_builder_nodes.h"
#include "intern/builder/deg_builder_relations.h"
#include "intern/depsgraph.hh"

#include "DNA_layer_types.h"

namespace blender::deg {

namespace {

class AllObjectsNodeBuilder : public DepsgraphNodeBuilder {
 public:
  AllObjectsNodeBuilder(Main *bmain,
                        Depsgraph *graph,
                        DepsgraphBuilderCache *cache,
                        blender::bke::DynamicOverridesEvaluationData *dynoverride)
      : DepsgraphNodeBuilder(bmain, graph, cache, dynoverride)
  {
  }

  bool need_pull_base_into_graph(const Base * /*base*/) override
  {
    return true;
  }
};

class AllObjectsRelationBuilder : public DepsgraphRelationBuilder {
 public:
  AllObjectsRelationBuilder(Main *bmain,
                            Depsgraph *graph,
                            DepsgraphBuilderCache *cache,
                            blender::bke::DynamicOverridesEvaluationData *dynoverride)
      : DepsgraphRelationBuilder(bmain, graph, cache, dynoverride)
  {
  }

  bool need_pull_base_into_graph(const Base * /*base*/) override
  {
    return true;
  }
};

}  // namespace

AllObjectsBuilderPipeline::AllObjectsBuilderPipeline(::Depsgraph *graph)
    : ViewLayerBuilderPipeline(graph)
{
}

std::unique_ptr<DepsgraphNodeBuilder> AllObjectsBuilderPipeline::construct_node_builder()
{
  return std::make_unique<AllObjectsNodeBuilder>(
      bmain_, deg_graph_, &builder_cache_, dynoverride_);
}

std::unique_ptr<DepsgraphRelationBuilder> AllObjectsBuilderPipeline::construct_relation_builder()
{
  return std::make_unique<AllObjectsRelationBuilder>(
      bmain_, deg_graph_, &builder_cache_, dynoverride_);
}

}  // namespace blender::deg
