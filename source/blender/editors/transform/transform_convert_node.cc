/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <algorithm>
#include <cmath>

#include "DNA_space_types.h"
#include "DNA_userdef_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_rect.hh"
#include "BLI_span.hh"
#include "BLI_time.hh"

#include "BKE_context.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "ED_node.hh"

#include "UI_view2d.hh"

#include "transform.hh"
#include "transform_convert.hh"
#include "transform_snap.hh"

#include "WM_api.hh"

namespace blender::ed::transform {

struct NodeShakeSample {
  float2 mval;
  double time;
};

struct TransCustomDataNode {
  ui::View2DEdgePanData edgepan_data{};

  /* Compare if the view has changed so we can update with `transformViewUpdate`. */
  rctf viewrect_prev{};

  bool is_new_node = false;

  Map<bNode *, bNode *> old_parent_by_detached_node;

  Vector<NodeShakeSample, 32> shake_samples;
  bool shake_available = false;
  bool shake_triggered = false;
};

/* -------------------------------------------------------------------- */
/** \name Node Transform Creation
 * \{ */

static void create_transform_data_for_node(TransData &td,
                                           TransData2D &td2d,
                                           bNode &node,
                                           const float dpi_fac)
{
  /* Account for parents (nested nodes). */
  float2 loc = float2(node.location) * dpi_fac;

  /* Use top-left corner as the transform origin for nodes. */
  /* Weirdo - but the node system is a mix of free 2d elements and DPI sensitive UI. */
  td2d.loc[0] = loc.x;
  td2d.loc[1] = loc.y;
  td2d.loc[2] = 0.0f;
  td2d.loc2d = td2d.loc; /* Current location. */

  td.loc = td2d.loc;
  copy_v3_v3(td.iloc, td.loc);
  /* Use node center instead of origin (top-left corner). */
  td.center[0] = td2d.loc[0];
  td.center[1] = td2d.loc[1];
  td.center[2] = 0.0f;

  memset(td.axismtx, 0, sizeof(td.axismtx));
  td.axismtx[2][2] = 1.0f;

  td.val = nullptr;

  td.flag = TD_SELECTED;
  td.dist = 0.0f;

  unit_m3(td.mtx);
  unit_m3(td.smtx);

  td.extra = &node;
}

static bool is_node_parent_select(const bNode *node)
{
  while ((node = node->parent)) {
    if (node->flag & NODE_SELECT) {
      return true;
    }
  }
  return false;
}

/**
 * Some nodes are transformed together with other nodes:
 * - Parent frames with shrinking turned on are automatically resized based on their children.
 * - Child nodes of frames that are manually resizable are transformed together with their parent
 *   frame.
 */
static bool transform_tied_to_other_node(bNode *node, VectorSet<bNode *> transformed_nodes)
{
  /* Check for frame nodes that adjust their size based on the contained child nodes. */
  if (node->is_frame()) {
    const NodeFrame *data = static_cast<const NodeFrame *>(node->storage);
    const bool shrinking = data->flag & NODE_FRAME_SHRINK;
    const bool is_parent = !node->direct_children_in_frame().is_empty();

    if (is_parent && shrinking) {
      return true;
    }
  }

  /* Now check for child nodes of manually resized frames. */
  while ((node = node->parent)) {
    const NodeFrame *parent_data = static_cast<const NodeFrame *>(node->storage);
    const bool parent_shrinking = parent_data->flag & NODE_FRAME_SHRINK;
    const bool parent_transformed = transformed_nodes.contains(node);

    if (parent_transformed && !parent_shrinking) {
      return true;
    }
  }

  return false;
}

static VectorSet<bNode *> get_transformed_nodes(bNodeTree &node_tree)
{
  VectorSet<bNode *> nodes = node_tree.all_nodes();

  /* Keep only nodes that are selected or inside a frame that is selected. */
  nodes.remove_if([&](bNode *node) {
    const bool node_selected = node->flag & NODE_SELECT;
    const bool parent_selected = is_node_parent_select(node);
    return (!node_selected && !parent_selected);
  });

  /* Remove nodes that are transformed together with their parent or child nodes. */
  nodes.remove_if([&](bNode *node) { return transform_tied_to_other_node(node, nodes); });

  return nodes;
}

static void createTransNodeData(bContext * /*C*/, TransInfo *t)
{
  SpaceNode *snode = static_cast<SpaceNode *>(t->area->spacedata.first);
  bNodeTree *node_tree = snode->edittree;
  if (!node_tree) {
    return;
  }

  /* Custom data to enable edge panning during the node transform. */
  TransCustomDataNode *customdata = MEM_new<TransCustomDataNode>(__func__);
  space_node::node_shake_preview_clear(*snode);
  view2d_edge_pan_init(t->context,
                       &customdata->edgepan_data,
                       NODE_EDGE_PAN_INSIDE_PAD,
                       NODE_EDGE_PAN_OUTSIDE_PAD,
                       NODE_EDGE_PAN_SPEED_RAMP,
                       NODE_EDGE_PAN_MAX_SPEED,
                       NODE_EDGE_PAN_DELAY,
                       NODE_EDGE_PAN_ZOOM_INFLUENCE);
  customdata->viewrect_prev = customdata->edgepan_data.initial_rect;
  customdata->is_new_node = t->remove_on_cancel;
  customdata->shake_available = space_node::node_shake_detach_is_enabled(*node_tree);
  customdata->shake_samples.append({t->mval, BLI_time_now_seconds()});

  if (t->region) {
    space_node::node_insert_on_link_flags_set(
        *snode, *t->region, t->modifiers & MOD_NODE_ATTACH, customdata->is_new_node);
    space_node::node_insert_on_frame_flag_set(*snode, *t->region, int2(t->mval));
  }

  t->custom.type.data = customdata;
  t->custom.type.free_cb = [](TransInfo *, TransDataContainer *, TransCustomData *custom_data) {
    TransCustomDataNode *data = static_cast<TransCustomDataNode *>(custom_data->data);
    MEM_delete(data);
    custom_data->data = nullptr;
  };

  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);

  /* Nodes don't support proportional editing and probably never will. */
  t->flag = t->flag & ~T_PROP_EDIT_ALL;

  VectorSet<bNode *> nodes = get_transformed_nodes(*node_tree);
  if (nodes.is_empty()) {
    return;
  }

  tc->data_len = nodes.size();
  tc->data = MEM_new_array_zeroed<TransData>(tc->data_len, __func__);
  tc->data_2d = MEM_new_array_zeroed<TransData2D>(tc->data_len, __func__);

  for (const int i : nodes.index_range()) {
    create_transform_data_for_node(tc->data[i], tc->data_2d[i], *nodes[i], UI_SCALE_FAC);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Flush Transform Nodes
 * \{ */

static void node_snap_grid_apply(TransInfo *t)
{
  if (!(transform_snap_is_active(t) &&
        (t->tsnap.mode & (SCE_SNAP_TO_INCREMENT | SCE_SNAP_TO_GRID))))
  {
    return;
  }

  float2 grid_size = t->snap_spatial;
  if (t->modifiers & MOD_PRECISION) {
    grid_size *= t->snap_spatial_precision;
  }

  /* Early exit on unusable grid size. */
  if (math::is_zero(grid_size)) {
    return;
  }

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    for (const int i : IndexRange(tc->data_len)) {
      TransData &td = tc->data[i];
      if (td.flag & TD_SKIP) {
        continue;
      }

      if ((t->flag & T_PROP_EDIT) && (td.factor == 0.0f)) {
        continue;
      }

      /* Nodes are snapped to the grid by first aligning their initial position to the grid and
       * then offsetting them in grid increments.
       *
       * This ensures that multiple unsnapped nodes snap to the grid in sync while moving.
       */

      const float2 inital_location = td.iloc;
      const float2 target_location = td.loc;
      const float2 offset = target_location - inital_location;

      const float2 snapped_inital_location = math::round(inital_location / grid_size) * grid_size;
      const float2 snapped_offset = math::round(offset / grid_size) * grid_size;
      const float2 snapped_target_location = snapped_inital_location + snapped_offset;

      copy_v2_v2(td.loc, snapped_target_location);
    }
  }
}

static Vector<bNode *> node_shake_transformed_nodes_get(TransInfo *t)
{
  Vector<bNode *> nodes;
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    for (const int i : IndexRange(tc->data_len)) {
      TransData &td = tc->data[i];
      if (td.flag & TD_SKIP) {
        continue;
      }
      if (bNode *node = static_cast<bNode *>(td.extra)) {
        nodes.append(node);
      }
    }
  }
  return nodes;
}

static float node_shake_min_leg_distance()
{
  const float sensitivity = float(std::clamp<int>(U.node_shake_sensitivity, 1, 10) - 1) / 9.0f;
  return 54.0f + (14.0f - 54.0f) * sensitivity;
}

static bool node_shake_detected_on_axis(const Span<NodeShakeSample> samples,
                                        const int axis,
                                        const float min_leg)
{
  float min_sample = samples.first().mval[axis];
  float max_sample = samples.first().mval[axis];
  for (const NodeShakeSample &sample : samples.drop_front(1)) {
    min_sample = std::min(min_sample, sample.mval[axis]);
    max_sample = std::max(max_sample, sample.mval[axis]);
  }
  const float axis_span = max_sample - min_sample;
  if (axis_span < min_leg) {
    return false;
  }

  float extreme = samples.first().mval[axis];
  int direction = 0;
  int reversals = 0;
  float travel = 0.0f;
  for (const NodeShakeSample &sample : samples.drop_front(1)) {
    const float value = sample.mval[axis];
    if (direction == 0) {
      const float delta = value - extreme;
      if (std::abs(delta) >= min_leg) {
        direction = delta > 0.0f ? 1 : -1;
        travel += std::abs(delta);
        extreme = value;
      }
      continue;
    }

    if (direction > 0) {
      if (value > extreme) {
        travel += value - extreme;
        extreme = value;
      }
      else if (extreme - value >= min_leg) {
        reversals++;
        direction = -1;
        travel += extreme - value;
        extreme = value;
      }
    }
    else {
      if (value < extreme) {
        travel += extreme - value;
        extreme = value;
      }
      else if (value - extreme >= min_leg) {
        reversals++;
        direction = 1;
        travel += value - extreme;
        extreme = value;
      }
    }
  }

  return reversals >= 2 && travel >= min_leg * 2.5f && travel >= axis_span * 1.6f;
}

static bool node_shake_detected(Span<NodeShakeSample> samples)
{
  const double time_limit = double(std::clamp<int>(U.node_shake_time, 150, 1000)) / 1000.0;
  /* Keep long sample history, but only evaluate the configured gesture window. */
  while (samples.size() > 1 && samples.last().time - samples.first().time > time_limit) {
    samples = samples.drop_front(1);
  }
  if (samples.size() < 4) {
    return false;
  }

  const float min_leg = node_shake_min_leg_distance();
  return node_shake_detected_on_axis(samples, 0, min_leg) ||
         node_shake_detected_on_axis(samples, 1, min_leg);
}

static bool node_shake_detector_update(TransCustomDataNode &customdata, TransInfo *t)
{
  if (!customdata.shake_available || customdata.shake_triggered || t->state == TRANS_CANCEL) {
    return false;
  }
  if (t->mode != TFM_TRANSLATION || (t->con.mode & CON_APPLY)) {
    return false;
  }

  const double now = BLI_time_now_seconds();
  const float2 mval = t->mval;
  if (customdata.shake_samples.is_empty() ||
      math::distance(customdata.shake_samples.last().mval, mval) >= 2.0f)
  {
    customdata.shake_samples.append({mval, now});
  }

  constexpr double max_time_limit = 1.0;
  while (customdata.shake_samples.size() > 1 &&
         now - customdata.shake_samples.first().time > max_time_limit)
  {
    customdata.shake_samples.remove(0);
  }
  while (customdata.shake_samples.size() > 48) {
    customdata.shake_samples.remove(0);
  }

  return node_shake_detected(customdata.shake_samples);
}

static void move_child_nodes(bNode &node, const float2 &delta)
{
  for (bNode *child : node.direct_children_in_frame()) {
    child->location[0] += delta.x;
    child->location[1] += delta.y;
    if (child->is_frame()) {
      move_child_nodes(*child, delta);
    }
  }
}

static bool has_selected_parent(const bNode &node)
{
  for (bNode *parent = node.parent; parent; parent = parent->parent) {
    if (parent->flag & NODE_SELECT) {
      return true;
    }
  }
  return false;
}

static void flushTransNodes(TransInfo *t)
{
  const float dpi_fac = UI_SCALE_FAC;
  SpaceNode *snode = static_cast<SpaceNode *>(t->area->spacedata.first);

  TransCustomDataNode *customdata = static_cast<TransCustomDataNode *>(t->custom.type.data);

  if (t->options & CTX_VIEW2D_EDGE_PAN) {
    if (t->state == TRANS_CANCEL) {
      view2d_edge_pan_cancel(t->context, &customdata->edgepan_data);
    }
    else {
      /* Edge panning functions expect window coordinates, mval is relative to region. */
      const int xy[2] = {
          t->region->winrct.xmin + int(t->mval[0]),
          t->region->winrct.ymin + int(t->mval[1]),
      };
      ui::view2d_edge_pan_apply(t->context, &customdata->edgepan_data, xy);
    }
  }

  float offset[2] = {0.0f, 0.0f};
  if (t->state != TRANS_CANCEL) {
    if (t->region &&
        !BLI_rctf_compare(&customdata->viewrect_prev, &t->region->v2d.cur, FLT_EPSILON))
    {
      /* Additional offset due to change in view2D rect. */
      BLI_rctf_transform_pt_v(&t->region->v2d.cur, &customdata->viewrect_prev, offset, offset);
      transformViewUpdate(t);
      customdata->viewrect_prev = t->region->v2d.cur;
    }
  }

  if (t->modifiers & MOD_NODE_FRAME) {
    t->modifiers &= ~MOD_NODE_FRAME;
    Vector<bNode *> nodes_to_detach;
    for (bNode *node : snode->edittree->all_nodes()) {
      if (!(node->flag & NODE_SELECT)) {
        continue;
      }
      if (has_selected_parent(*node)) {
        continue;
      }
      if (!node->parent) {
        continue;
      }
      customdata->old_parent_by_detached_node.add(node, node->parent);
      nodes_to_detach.append(node);
    }
    if (nodes_to_detach.is_empty()) {
      WM_operator_name_call(
          t->context, "NODE_OT_attach", wm::OpCallContext::InvokeDefault, nullptr, nullptr);
    }
    else {
      for (bNode *node : nodes_to_detach) {
        bke::node_detach_node(*snode->edittree, *node);
      }
    }
  }

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    node_snap_grid_apply(t);

    /* Flush to 2d vector from internally used 3d vector. */
    for (int i = 0; i < tc->data_len; i++) {
      TransData *td = &tc->data[i];
      TransData2D *td2d = &tc->data_2d[i];
      bNode *node = static_cast<bNode *>(td->extra);

      float2 loc = float2(td2d->loc) + offset;

      /* Weirdo - but the node system is a mix of free 2d elements and DPI sensitive UI. */
      loc /= dpi_fac;

      if (node->is_frame()) {
        const float2 delta = loc - float2(node->location);
        move_child_nodes(*node, delta);
      }

      node->location[0] = loc.x;
      node->location[1] = loc.y;
    }

    /* Handle intersection with noodles. */
    if (node_shake_detector_update(*customdata, t)) {
      Vector<bNode *> transformed_nodes = node_shake_transformed_nodes_get(t);
      customdata->shake_triggered = space_node::node_shake_preview_create(*snode,
                                                                          transformed_nodes);
      customdata->shake_available = false;
    }
    if (t->region) {
      space_node::node_insert_on_link_flags_set(
          *snode, *t->region, t->modifiers & MOD_NODE_ATTACH, customdata->is_new_node);
      space_node::node_insert_on_frame_flag_set(*snode, *t->region, int2(t->mval));
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Special After Transform Node
 * \{ */

static void special_aftertrans_update__node(bContext *C, TransInfo *t)
{
  Main *bmain = CTX_data_main(C);
  SpaceNode *snode = static_cast<SpaceNode *>(t->area->spacedata.first);
  bNodeTree *ntree = snode->edittree;
  const TransCustomDataNode &customdata = *static_cast<TransCustomDataNode *>(t->custom.type.data);

  const bool canceled = (t->state == TRANS_CANCEL);
  const bool shake_preview_active = space_node::node_shake_preview_is_active(*snode);

  if (canceled) {
    for (auto &&[node, parent] : customdata.old_parent_by_detached_node.items()) {
      bke::node_attach_node(*ntree, *node, *parent);
    }
  }
  if (canceled && t->remove_on_cancel) {
    /* Remove selected nodes on cancel. */
    if (ntree) {
      for (bNode &node : ntree->nodes.items_mutable()) {
        if (node.flag & NODE_SELECT) {
          bke::node_remove_node(bmain, *ntree, node, true);
        }
      }
      BKE_main_ensure_invariants(*bmain, ntree->id);
    }
  }

  if (!canceled) {
    ED_node_post_apply_transform(C, snode->edittree);
    if (shake_preview_active) {
      space_node::node_shake_preview_apply(*bmain, *snode);
    }
    else if (t->modifiers & MOD_NODE_ATTACH) {
      space_node::node_insert_on_link_flags(*bmain, *snode, customdata.is_new_node);
    }
  }

  space_node::node_insert_on_link_flags_clear(*ntree);
  space_node::node_shake_preview_clear(*snode);
  space_node::node_insert_on_frame_flag_clear(*snode);

  wmOperatorType *ot = WM_operatortype_find("NODE_OT_insert_offset", true);
  BLI_assert(ot);
  PointerRNA ptr = WM_operator_properties_create_ptr(ot);
  WM_operator_name_call_ptr(C, ot, wm::OpCallContext::InvokeDefault, &ptr, nullptr);
  WM_operator_properties_free(&ptr);
}

/** \} */

TransConvertTypeInfo TransConvertType_Node = {
    /*flags*/ (T_POINTS | T_2D_EDIT),
    /*create_trans_data*/ createTransNodeData,
    /*recalc_data*/ flushTransNodes,
    /*special_aftertrans_update*/ special_aftertrans_update__node,
};

}  // namespace blender::ed::transform
