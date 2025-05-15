/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_modifier_types.h"
#include "DNA_node_types.h"

#include "BKE_compute_context_cache.hh"
#include "BKE_compute_contexts.hh"

#include <ostream>

namespace blender::bke {

ModifierComputeContext::ModifierComputeContext(const ComputeContext *parent,
                                               const NodesModifierData &nmd)
    : ModifierComputeContext(parent, nmd.modifier.persistent_uid)
{
  nmd_ = &nmd;
}

ModifierComputeContext::ModifierComputeContext(const ComputeContext *parent,
                                               const int modifier_uid)
    : ComputeContext(parent), modifier_uid_(std::move(modifier_uid))
{
  hash_ = ComputeContextHash::from(parent, "MODIFIER", modifier_uid_);
}

void ModifierComputeContext::print_current_in_line(std::ostream &stream) const
{
  if (nmd_) {
    stream << "Modifier: " << nmd_->modifier.name;
  }
}

GroupNodeComputeContext::GroupNodeComputeContext(const ComputeContext *parent,
                                                 const int32_t node_id)
    : ComputeContext(parent), node_id_(node_id)
{
  hash_ = ComputeContextHash::from(parent, "NODE_GROUP", node_id);
}

GroupNodeComputeContext::GroupNodeComputeContext(const ComputeContext *parent,
                                                 const bNode &caller_group_node,
                                                 const bNodeTree &caller_tree)
    : GroupNodeComputeContext(parent, caller_group_node.identifier)
{
  caller_group_node_ = &caller_group_node;
  caller_tree_ = &caller_tree;
}

void GroupNodeComputeContext::print_current_in_line(std::ostream &stream) const
{
  if (caller_group_node_ != nullptr) {
    stream << "Node: " << caller_group_node_->name;
    return;
  }
}

SimulationZoneComputeContext::SimulationZoneComputeContext(const ComputeContext *parent,
                                                           const int32_t output_node_id)
    : ComputeContext(parent), output_node_id_(output_node_id)
{
  hash_ = ComputeContextHash::from(parent, "SIM", output_node_id);
}

SimulationZoneComputeContext::SimulationZoneComputeContext(const ComputeContext *parent,
                                                           const bNode &node)
    : SimulationZoneComputeContext(parent, node.identifier)
{
}

void SimulationZoneComputeContext::print_current_in_line(std::ostream &stream) const
{
  stream << "Simulation Zone ID: " << output_node_id_;
}

RepeatZoneComputeContext::RepeatZoneComputeContext(const ComputeContext *parent,
                                                   const int32_t output_node_id,
                                                   const int iteration)
    : ComputeContext(parent), output_node_id_(output_node_id), iteration_(iteration)
{
  hash_ = ComputeContextHash::from(parent, "REPEAT", output_node_id, iteration);
}

RepeatZoneComputeContext::RepeatZoneComputeContext(const ComputeContext *parent,
                                                   const bNode &node,
                                                   const int iteration)
    : RepeatZoneComputeContext(parent, node.identifier, iteration)
{
}

void RepeatZoneComputeContext::print_current_in_line(std::ostream &stream) const
{
  stream << "Repeat Zone ID: " << output_node_id_;
}

ForeachGeometryElementZoneComputeContext::ForeachGeometryElementZoneComputeContext(
    const ComputeContext *parent, const int32_t output_node_id, const int index)
    : ComputeContext(parent), output_node_id_(output_node_id), index_(index)
{
  hash_ = ComputeContextHash::from(parent, "FOREACH_GEOMETRY_ELEMENT", output_node_id, index);
}

ForeachGeometryElementZoneComputeContext::ForeachGeometryElementZoneComputeContext(
    const ComputeContext *parent, const bNode &node, const int index)
    : ForeachGeometryElementZoneComputeContext(parent, node.identifier, index)
{
}

void ForeachGeometryElementZoneComputeContext::print_current_in_line(std::ostream &stream) const
{
  stream << "Foreach Geometry Element Zone ID: " << output_node_id_;
}

EvaluateClosureComputeContext::EvaluateClosureComputeContext(const ComputeContext *parent,
                                                             const int32_t node_id)
    : ComputeContext(parent), node_id_(node_id)
{
  hash_ = ComputeContextHash::from(parent, "EVAL_CLOSURE", node_id);
}

EvaluateClosureComputeContext::EvaluateClosureComputeContext(
    const ComputeContext *parent,
    const int32_t evaluate_node_id,
    const bNode *evaluate_node,
    const std::optional<nodes::ClosureSourceLocation> &closure_source_location)
    : EvaluateClosureComputeContext(parent, evaluate_node_id)
{
  evaluate_node_ = evaluate_node;
  closure_source_location_ = closure_source_location;
}

void EvaluateClosureComputeContext::print_current_in_line(std::ostream &stream) const
{
  if (evaluate_node_ != nullptr) {
    stream << "Evaluate Closure: " << evaluate_node_->name;
    return;
  }
}

OperatorComputeContext::OperatorComputeContext() : OperatorComputeContext(nullptr) {}

OperatorComputeContext::OperatorComputeContext(const ComputeContext *parent)
    : ComputeContext(parent)
{
  hash_ = ComputeContextHash::from(parent, "OPERATOR");
}

OperatorComputeContext::OperatorComputeContext(const ComputeContext *parent, const bNodeTree &tree)
    : OperatorComputeContext(parent)
{
  tree_ = &tree;
}

void OperatorComputeContext::print_current_in_line(std::ostream &stream) const
{
  stream << "Operator";
}

const ModifierComputeContext &ComputeContextCache::for_modifier(const ComputeContext *parent,
                                                                const NodesModifierData &nmd)
{
  return *modifier_contexts_cache_.lookup_or_add_cb(
      std::pair{parent, nmd.modifier.persistent_uid},
      [&]() { return &this->for_any_uncached<ModifierComputeContext>(parent, nmd); });
}

const ModifierComputeContext &ComputeContextCache::for_modifier(const ComputeContext *parent,
                                                                const int modifier_uid)
{
  return *modifier_contexts_cache_.lookup_or_add_cb(std::pair{parent, modifier_uid}, [&]() {
    return &this->for_any_uncached<ModifierComputeContext>(parent, modifier_uid);
  });
}

const OperatorComputeContext &ComputeContextCache::for_operator(const ComputeContext *parent)
{
  return *operator_contexts_cache_.lookup_or_add_cb(
      parent, [&]() { return &this->for_any_uncached<OperatorComputeContext>(parent); });
}

const OperatorComputeContext &ComputeContextCache::for_operator(const ComputeContext *parent,
                                                                const bNodeTree &tree)
{
  return *operator_contexts_cache_.lookup_or_add_cb(
      parent, [&]() { return &this->for_any_uncached<OperatorComputeContext>(parent, tree); });
}

const GroupNodeComputeContext &ComputeContextCache::for_group_node(const ComputeContext *parent,
                                                                   const int32_t node_id)
{
  return *group_node_contexts_cache_.lookup_or_add_cb(std::pair{parent, node_id}, [&]() {
    return &this->for_any_uncached<GroupNodeComputeContext>(parent, node_id);
  });
}

const GroupNodeComputeContext &ComputeContextCache::for_group_node(const ComputeContext *parent,
                                                                   const bNode &caller_group_node,
                                                                   const bNodeTree &caller_tree)
{
  return *group_node_contexts_cache_.lookup_or_add_cb(
      std::pair{parent, caller_group_node.identifier}, [&]() {
        return &this->for_any_uncached<GroupNodeComputeContext>(
            parent, caller_group_node, caller_tree);
      });
}

const SimulationZoneComputeContext &ComputeContextCache::for_simulation_zone(
    const ComputeContext *parent, int output_node_id)
{
  return *simulation_zone_contexts_cache_.lookup_or_add_cb(
      std::pair{parent, output_node_id}, [&]() {
        return &this->for_any_uncached<SimulationZoneComputeContext>(parent, output_node_id);
      });
}

const SimulationZoneComputeContext &ComputeContextCache::for_simulation_zone(
    const ComputeContext *parent, const bNode &output_node)
{
  return *simulation_zone_contexts_cache_.lookup_or_add_cb(
      std::pair{parent, output_node.identifier}, [&]() {
        return &this->for_any_uncached<SimulationZoneComputeContext>(parent, output_node);
      });
}

const RepeatZoneComputeContext &ComputeContextCache::for_repeat_zone(const ComputeContext *parent,
                                                                     int32_t output_node_id,
                                                                     int iteration)
{
  return *repeat_zone_contexts_cache_.lookup_or_add_cb(
      std::pair{parent, std::pair{output_node_id, iteration}}, [&]() {
        return &this->for_any_uncached<RepeatZoneComputeContext>(
            parent, output_node_id, iteration);
      });
}

const RepeatZoneComputeContext &ComputeContextCache::for_repeat_zone(const ComputeContext *parent,
                                                                     const bNode &output_node,
                                                                     int iteration)
{
  return *repeat_zone_contexts_cache_.lookup_or_add_cb(
      std::pair{parent, std::pair{output_node.identifier, iteration}}, [&]() {
        return &this->for_any_uncached<RepeatZoneComputeContext>(parent, output_node, iteration);
      });
}

const ForeachGeometryElementZoneComputeContext &ComputeContextCache::
    for_foreach_geometry_element_zone(const ComputeContext *parent,
                                      int32_t output_node_id,
                                      int index)
{
  return *foreach_geometry_element_zone_contexts_cache_.lookup_or_add_cb(
      std::pair{parent, std::pair{output_node_id, index}}, [&]() {
        return &this->for_any_uncached<ForeachGeometryElementZoneComputeContext>(
            parent, output_node_id, index);
      });
}

const ForeachGeometryElementZoneComputeContext &ComputeContextCache::
    for_foreach_geometry_element_zone(const ComputeContext *parent,
                                      const bNode &output_node,
                                      int index)
{
  return *foreach_geometry_element_zone_contexts_cache_.lookup_or_add_cb(
      std::pair{parent, std::pair{output_node.identifier, index}}, [&]() {
        return &this->for_any_uncached<ForeachGeometryElementZoneComputeContext>(
            parent, output_node, index);
      });
}

const EvaluateClosureComputeContext &ComputeContextCache::for_evaluate_closure(
    const ComputeContext *parent, int32_t node_id)
{
  return *evaluate_closure_contexts_cache_.lookup_or_add_cb(std::pair{parent, node_id}, [&]() {
    return &this->for_any_uncached<EvaluateClosureComputeContext>(parent, node_id);
  });
}

const EvaluateClosureComputeContext &ComputeContextCache::for_evaluate_closure(
    const ComputeContext *parent,
    const int32_t evaluate_node_id,
    const bNode *evaluate_node,
    const std::optional<nodes::ClosureSourceLocation> &closure_source_location)
{
  return *evaluate_closure_contexts_cache_.lookup_or_add_cb(
      std::pair{parent, evaluate_node_id}, [&]() {
        return &this->for_any_uncached<EvaluateClosureComputeContext>(
            parent, evaluate_node_id, evaluate_node, closure_source_location);
      });
}

}  // namespace blender::bke
