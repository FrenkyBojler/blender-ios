/* SPDX-FileCopyrightText: 2013 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 *
 * Methods for constructing depsgraph
 */

#include "intern/builder/deg_builder_relations.h"

#include <cstdlib>
#include <cstring> /* required for STREQ later on. */

#include "DNA_collection_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase.h"

#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "intern/builder/deg_builder.h"

#include "intern/node/deg_node.hh"
#include "intern/node/deg_node_component.hh"
#include "intern/node/deg_node_id.hh"
#include "intern/node/deg_node_operation.hh"

namespace blender::deg {

void DepsgraphRelationBuilder::build_layer_collection_recursive(
    LayerCollection *layer_collection, const ComponentKey &parent_hierarchy_key)
{
  const int hide_flag = (graph_->mode == DAG_EVAL_VIEWPORT) ? COLLECTION_HIDE_VIEWPORT :
                                                              COLLECTION_HIDE_RENDER;

  Collection *collection = layer_collection->collection;

  /* Hidden collections (COLLECTION_HIDE_VIEWPORT / COLLECTION_HIDE_RENDER) are a hard stop:
   * DepsgraphNodeBuilder::build_layer_collections() also `continue`s on this flag without
   * recursing into children, so we mirror that here. */
  if (collection->flag & hide_flag) {
    return;
  }

  if (layer_collection->flag & LAYER_COLLECTION_EXCLUDE) {
    /* Exclude is per-LayerCollection and not inherited (see BKE layer.cc): non-excluded
     * descendants of an excluded ancestor still contribute objects to the view layer base
     * list, and DepsgraphNodeBuilder::build_layer_collections() builds those collections.
     * Recurse through this excluded LC so the relation builder visits the same set of
     * collections, and propagate the unchanged parent hierarchy key so a non-excluded
     * descendant attaches its hierarchy relation to the nearest non-excluded ancestor
     * (or scene). This avoids the `do_sanity_checks` mismatch ("Some IDs missed nodes or
     * relations building") that previously left collection IDs without HIERARCHY relations,
     * which in turn caused a parallel-evaluation race in Mesh::bounds_min_max(). */
    for (LayerCollection &child : layer_collection->layer_collections) {
      build_layer_collection_recursive(&child, parent_hierarchy_key);
    }
    return;
  }

  build_collection(layer_collection, collection);

  const ComponentKey collection_hierarchy_key{&collection->id, NodeType::HIERARCHY};
  /* Preserve the pre-fix "Scene -> Collection hierarchy" / "Collection hierarchy" diagnostic
   * strings depending on whether the parent edge is the scene or another collection. The
   * dependency graph itself is identified by ComponentKey, not by description, but keeping
   * the descriptions stable helps when diffing depsgraph dumps against older builds. */
  const char *relation_description = (parent_hierarchy_key.id == &scene_->id) ?
                                         "Scene -> Collection hierarchy" :
                                         "Collection hierarchy";
  add_relation(parent_hierarchy_key, collection_hierarchy_key, relation_description);

  for (LayerCollection &child : layer_collection->layer_collections) {
    build_layer_collection_recursive(&child, collection_hierarchy_key);
  }
}

bool DepsgraphRelationBuilder::build_layer_collection(LayerCollection *layer_collection)
{
  const int hide_flag = (graph_->mode == DAG_EVAL_VIEWPORT) ? COLLECTION_HIDE_VIEWPORT :
                                                              COLLECTION_HIDE_RENDER;

  Collection *collection = layer_collection->collection;

  const bool is_collection_hidden = collection->flag & hide_flag;
  const bool is_layer_collection_excluded = layer_collection->flag & LAYER_COLLECTION_EXCLUDE;

  if (is_collection_hidden) {
    return false;
  }

  const ComponentKey scene_hierarchy_key{&scene_->id, NodeType::HIERARCHY};
  build_layer_collection_recursive(layer_collection, scene_hierarchy_key);

  return !is_layer_collection_excluded;
}

void DepsgraphRelationBuilder::build_view_layer_collections(ViewLayer *view_layer)
{
  const ComponentKey scene_hierarchy_key{&scene_->id, NodeType::HIERARCHY};

  for (LayerCollection &layer_collection : view_layer->layer_collections) {
    build_layer_collection_recursive(&layer_collection, scene_hierarchy_key);
  }
}

void DepsgraphRelationBuilder::build_freestyle_lineset(FreestyleLineSet *fls)
{
  if (fls->group != nullptr) {
    build_collection(nullptr, fls->group);
  }
  if (fls->linestyle != nullptr) {
    build_freestyle_linestyle(fls->linestyle);
  }
}

void DepsgraphRelationBuilder::build_view_layer(Scene *scene,
                                                ViewLayer *view_layer,
                                                eDepsNode_LinkedState_Type linked_state)
{
  /* Setup currently building context. */
  scene_ = scene;
  BKE_view_layer_synced_ensure(*bmain_, scene, view_layer);
  /* Scene objects. */
  /* NOTE: Nodes builder requires us to pass evaluated base because it's being
   * passed to the evaluation functions. During relations builder we only
   * do nullptr-pointer check of the base, so it's fine to pass original one. */
  for (Base &base : *BKE_view_layer_object_bases_get(view_layer)) {
    if (need_pull_base_into_graph(&base)) {
      build_object_from_view_layer_base(base.object);
    }
  }

  build_view_layer_collections(view_layer);

  build_scene_camera(scene);
  /* Rigidbody. */
  if (scene->rigidbody_world != nullptr) {
    build_rigidbody(scene);
  }
  /* Scene's animation and drivers. */
  if (scene->adt != nullptr) {
    build_animdata(&scene->id);
  }
  /* World. */
  if (scene->world != nullptr) {
    build_world(scene->world);
  }
  /* Cache file. */
  for (CacheFile &cachefile : bmain_->cachefiles) {
    build_cachefile(&cachefile);
  }
  /* Masks. */
  for (Mask &mask : bmain_->masks) {
    build_mask(&mask);
  }
  /* Movie clips. */
  for (MovieClip &clip : bmain_->movieclips) {
    build_movieclip(&clip);
  }
  /* Material override. */
  if (view_layer->mat_override != nullptr) {
    build_material(view_layer->mat_override);
  }
  /* World override */
  if (view_layer->world_override != nullptr) {
    build_world(view_layer->world_override);
  }
  /* Freestyle linesets. */
  for (FreestyleLineSet &fls : view_layer->freestyle_config.linesets) {
    build_freestyle_lineset(&fls);
  }
  /* Scene parameters, compositor and such. */
  build_scene_compositor(scene);
  build_scene_parameters(scene);
  /* Make final scene evaluation dependent on view layer evaluation. */
  OperationKey scene_view_layer_key(
      &scene->id, NodeType::LAYER_COLLECTIONS, OperationCode::VIEW_LAYER_EVAL);
  ComponentKey scene_eval_key(&scene->id, NodeType::SCENE);
  add_relation(scene_view_layer_key, scene_eval_key, "View Layer -> Scene Eval");
  /* Sequencer. */
  if (linked_state == DEG_ID_LINKED_DIRECTLY) {
    build_scene_audio(scene);
    build_scene_sequencer(scene);
  }
  /* Build all set scenes. */
  if (scene->set != nullptr) {
    ViewLayer *set_view_layer = BKE_view_layer_default_render(scene->set);
    build_view_layer(scene->set, set_view_layer, DEG_ID_LINKED_VIA_SET);
  }
}

}  // namespace blender::deg
