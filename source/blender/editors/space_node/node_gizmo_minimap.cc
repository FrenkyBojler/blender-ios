/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spnode
 */


#include "BLI_bounds.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"

#include "BKE_context.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_tree_zones.hh"
#include "BKE_screen.hh"

#include "DNA_userdef_types.h"
#include "DNA_screen_types.h"

#include "GPU_batch.hh"
#include "GPU_select.hh"
#include "GPU_state.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_resources.hh"


#include "node_intern.hh" /* own include */

namespace blender::ed::space_node {

struct NodeGizmoMinimap {
  wmGizmo gizmo;
};

/* -------------------------------------------------------------------- */

/* Duplicate from node_draw.cc. TODO: Maybe there is a better way to share code with  */
static int node_get_colorid(const bNode &node)
{
  const int nclass = (node.typeinfo->ui_class == nullptr) ? node.typeinfo->nclass :
                                                            node.typeinfo->ui_class(&node);
  switch (nclass) {
    case NODE_CLASS_INPUT:
      return TH_NODE_INPUT;
    case NODE_CLASS_OUTPUT: 
      return TH_NODE_OUTPUT;
    case NODE_CLASS_CONVERTER:
      return TH_NODE_CONVERTER;
    case NODE_CLASS_OP_COLOR:
      return TH_NODE_COLOR;
    case NODE_CLASS_OP_VECTOR:
      return TH_NODE_VECTOR;
    case NODE_CLASS_OP_FILTER:
      return TH_NODE_FILTER;
    case NODE_CLASS_GROUP:
      return TH_NODE_GROUP;
    case NODE_CLASS_INTERFACE:
      return TH_NODE_INTERFACE;
    case NODE_CLASS_MATTE:
      return TH_NODE_MATTE;
    case NODE_CLASS_DISTORT:
      return TH_NODE_DISTORT;
    case NODE_CLASS_TEXTURE:
      return TH_NODE_TEXTURE;
    case NODE_CLASS_SHADER:
      return TH_NODE_SHADER;
    case NODE_CLASS_SCRIPT:
      return TH_NODE_SCRIPT;
    case NODE_CLASS_GEOMETRY:
      return TH_NODE_GEOMETRY;
    case NODE_CLASS_ATTRIBUTE:
      return TH_NODE_ATTRIBUTE;
    case NODE_CLASS_LAYOUT:
      return node.is_frame() ? TH_NODE_FRAME : TH_NODE;
    default:
      return TH_NODE;
  }
}

// For now make this for now
static bool is_minimap_draw_top(const SpaceNode *snode)
{
 return snode->gizmo_flag & SNODE_GIZMO_MINIMAP_MOVE_TO_TOP; 
}

static void minimap_draw_intern(const bContext *C, wmGizmo * /*gz*/, const bool select)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  ARegion *region = CTX_wm_region(C);
  View2D v2d = region->v2d;

  if ((snode->gizmo_flag & SNODE_GIZMO_HIDE) && !(snode->gizmo_flag & SNODE_GIZMO_SHOW_MINIMAP)) {
    return;
  }

  bNodeTree *node_tree = snode->edittree;

  float viewport[4];
  GPU_viewport_size_get_f(viewport);

  float minimap_overlay_scale = snode->minimap_scale;
  float minimap_size = 150.0f * minimap_overlay_scale * UI_SCALE_FAC;
  float minimap_border_radius = BASIS_RAD +  0.5f;
  float padding = 10.0f * UI_SCALE_FAC;
  float inner_padding = 10.0f * UI_SCALE_FAC;

  float min[2], max[2];
  INIT_MINMAX2(min, max);

  for (bNode &node : node_tree->nodes) {
    float pos_min[2] = {node.runtime->draw_bounds.xmin, node.runtime->draw_bounds.ymin};
    float pos_max[2] = {node.runtime->draw_bounds.xmax, node.runtime->draw_bounds.ymax};
    minmax_v2v2_v2(min, max, pos_min);
    minmax_v2v2_v2(min, max, pos_max);
  }

  const rcti *rect_visible = ED_region_visible_rect(region);
  const float viewport_height = BLI_rcti_size_y(&v2d.mask);
  float viewport_width = BLI_rcti_size_x(&v2d.mask);
  float tile_height = viewport_height - BLI_rcti_size_y(rect_visible);
  float top_padding = padding;

  float minimap_aspect_ratio = snode->minimap_aspect_ratio;

  float minimap_width = minimap_size * minimap_aspect_ratio;
  float minimap_height = minimap_size;
  if (minimap_aspect_ratio > 1) {
    minimap_width = minimap_size;
    minimap_height = minimap_size / minimap_aspect_ratio;
  }

  if (is_minimap_draw_top(snode)) {
    tile_height = 0;
    viewport_width = BLI_rcti_size_x(rect_visible);
    top_padding = viewport_height - minimap_height - padding;
  }

  float minimap_width_without_padding = minimap_width - inner_padding * 2;
  float minimap_height_without_padding = minimap_height - inner_padding * 2;
  rctf minimap_space;
  BLI_rctf_init(&minimap_space, min[0], max[0], min[1], max[1]);
  float minimap_space_width = BLI_rctf_size_x(&minimap_space);
  float minimap_space_height = BLI_rctf_size_y(&minimap_space);
  float minimap_scale = min_ff(minimap_width_without_padding / minimap_space_width,
                               minimap_height_without_padding / minimap_space_height);

  float offset_x = (minimap_width_without_padding - minimap_space_width * minimap_scale) * 0.5f +
                   inner_padding;
  float offset_y = (minimap_height_without_padding - minimap_space_height * minimap_scale) * 0.5f +
                   inner_padding;

  rctf minimap_rect;
  BLI_rctf_init(&minimap_rect,
                viewport_width - padding - minimap_width,
                viewport_width - padding,
                top_padding + tile_height,
                top_padding + minimap_height + tile_height);
  
  rctf minimap_rect_back;
  BLI_rctf_init(&minimap_rect_back, minimap_rect.xmin, minimap_rect.xmax, minimap_rect.ymin , minimap_rect.ymax);

  /* Draw Backdrop. */
  float backdrop_color[4];
  float backdrop_color_outline[4];
  ui::theme::get_color_shade_alpha_4fv(TH_BACK, 20, 0, backdrop_color_outline);
  ui::theme::get_color_shade_alpha_4fv(TH_BACK, -7, -10, backdrop_color);
  ui::draw_roundbox_corner_set(ui::CNR_ALL);
  ui::draw_roundbox_4fv_ex(&minimap_rect_back,
                          backdrop_color,
                          nullptr,
                          1.0f,
                          backdrop_color_outline,
                          4.0f,
                          minimap_border_radius);
  GPU_blend(GPU_BLEND_NONE);

  /* Draw nodes. */
  float node_border_radius = 3.0f;
  float node_color[4];
  float node_color_frame[4];
  float node_color_outline_selected[4];
  float node_color_outline_active[4];
  float node_color_outline_group_input_output[4];

  ui::theme::get_color_shade_alpha_4fv(TH_BACK, 30, 0, node_color);
  ui::theme::get_color_shade_alpha_4fv(TH_BACK, 30, 0, node_color_outline_group_input_output);
  ui::theme::get_color_shade_alpha_4fv(TH_SELECT, 0, 0, node_color_outline_selected);
  ui::theme::get_color_shade_alpha_4fv(TH_ACTIVE, 0, 0, node_color_outline_active);
  ui::theme::get_color_shade_alpha_4fv(TH_BACK, 10, 0, node_color_frame);


  /* Draw node frames. */
  for (bNode &node : node_tree->nodes) {
    if (!(node.type_legacy == NODE_FRAME)) {
        continue;
    }
    float pos[2], size[2];
    pos[0] = (node.runtime->draw_bounds.xmin - minimap_space.xmin) * minimap_scale + minimap_rect.xmin +
              offset_x;
    pos[1] = (node.runtime->draw_bounds.ymin - minimap_space.ymin) * minimap_scale + minimap_rect.ymin +
              offset_y;
    size[0] = BLI_rctf_size_x(&node.runtime->draw_bounds) * minimap_scale;
    size[1] = BLI_rctf_size_y(&node.runtime->draw_bounds) * minimap_scale;

    if (snode->gizmo_flag & SNODE_GIZMO_MINIMAP_USE_FRAME_COLORS) {
      const float alpha = node_color_frame[3];
      if (node.flag & NODE_CUSTOM_COLOR) {
        rgba_float_args_set(
            node_color_frame, node.color[0], node.color[1], node.color[2], alpha);
      }
      else {
        ui::theme::get_color_shade_alpha_4fv(TH_BACK, 10, 0, node_color_frame);
      }
    }

    rctf node_rect;
    BLI_rctf_init(&node_rect, pos[0], pos[0] + size[0], pos[1], pos[1] + size[1]);
    ui::draw_roundbox_4fv(&node_rect, true, node_border_radius, node_color_frame);
  }

  /* Draw zones. */
  const bke::bNodeTreeZones *zones = node_tree->zones();
  const int zones_num = zones ? zones->zones.size() : 0;

  Array<Vector<float2>> bounds_by_zone(zones_num);

  for (const int zone_i : IndexRange(zones_num)) {
    const bke::bNodeTreeZone &zone = *zones->zones[zone_i];

    find_bounds_by_zone_recursive(*snode, zone, zones->zones, bounds_by_zone);
    const Span<float2> boundary_positions = bounds_by_zone[zone_i];
    const int boundary_positions_num = boundary_positions.size();
    if (boundary_positions_num < 3) {
      /* Can happen when drawing zone errors. */
      continue;
    }

    const blender::Bounds<float2> bounding_box = *blender::bounds::min_max(boundary_positions);
    const float bounding_box_width = bounding_box.max.x - bounding_box.min.x;
    const float bounding_box_height= bounding_box.max.y - bounding_box.min.y;


    float pos[2], size[2];
    pos[0] = (bounding_box.min.x - minimap_space.xmin) * minimap_scale + minimap_rect.xmin +
              offset_x;
    pos[1] = (bounding_box.min.y - minimap_space.ymin) * minimap_scale + minimap_rect.ymin +
              offset_y;
    size[0] = bounding_box_width * minimap_scale;
    size[1] = bounding_box_height * minimap_scale;
    
    const auto get_theme_id = [&](const int zone_i) {
    const bNode *node = zones->zones[zone_i]->output_node();
    if (!node) {
      return TH_REDALERT;
    }
    return ThemeColorID(bke::zone_type_by_node_type(node->type_legacy)->theme_id);
    };

    ui::theme::get_color_shade_alpha_4fv(get_theme_id(zone_i), 10, 0, node_color_frame);
    
    rctf node_rect;
    BLI_rctf_init(&node_rect, pos[0], pos[0] + size[0], pos[1], pos[1] + size[1]);
    ui::draw_roundbox_4fv(&node_rect, true, node_border_radius, node_color_frame);
  }

  /* Draw nodes. */
  for (bNode &node : node_tree->nodes) {
    /* Skip zone nodes */
    if (ELEM(node.type_legacy, 
             GEO_NODE_SIMULATION_INPUT,
             GEO_NODE_SIMULATION_OUTPUT,
             GEO_NODE_REPEAT_INPUT,
             GEO_NODE_REPEAT_OUTPUT,
             GEO_NODE_FOREACH_GEOMETRY_ELEMENT_OUTPUT,
             GEO_NODE_FOREACH_GEOMETRY_ELEMENT_INPUT,
             NODE_CLOSURE_INPUT,
             NODE_CLOSURE_OUTPUT,
             NODE_FRAME)) {
      continue;
    }

    if (node.parent && !(snode->gizmo_flag & SNODE_GIZMO_MINIMAP_SHOW_NODES_IN_FRAME)) {
      continue;
    }

    float pos[2], size[2];

    pos[0] = (node.runtime->draw_bounds.xmin - minimap_space.xmin) * minimap_scale + minimap_rect.xmin +
              offset_x;
    pos[1] = (node.runtime->draw_bounds.ymin - minimap_space.ymin) * minimap_scale + minimap_rect.ymin +
              offset_y;
    size[0] = BLI_rctf_size_x(&node.runtime->draw_bounds) * minimap_scale;
    size[1] = BLI_rctf_size_y(&node.runtime->draw_bounds) * minimap_scale;

    if (snode->gizmo_flag & SNODE_GIZMO_MINIMAP_USE_NODE_COLORS) {
      int color_id = node_get_colorid(node);
      ui::theme::get_color_3fv(color_id, node_color);
    }

    rctf node_rect;
    BLI_rctf_init(&node_rect, pos[0], pos[0] + size[0], pos[1], pos[1] + size[1]);
    if (node.flag & NODE_ACTIVE) {
      ui::draw_roundbox_4fv_ex(&node_rect,
                              node_color,
                              nullptr,
                              1.0f,
                              node_color_outline_active,
                              3.0f,
                              node_border_radius);
    }
    else if (node.flag & NODE_SELECT) {
      ui::draw_roundbox_4fv_ex(&node_rect,
                              node_color,
                              nullptr,
                              1.0f,
                              node_color_outline_selected,
                              3.0f,
                              node_border_radius);
    }
    else if (ELEM(node.type_legacy, NODE_GROUP_INPUT, NODE_GROUP_OUTPUT)) {
      ui::draw_roundbox_4fv_ex(&node_rect,
                              node_color,
                              nullptr,
                              1.0f,
                              node_color_outline_group_input_output,
                              3.0f,
                              node_border_radius);
    }
    else {
      ui::draw_roundbox_4fv(&node_rect, true, node_border_radius, node_color);
    }
    
  }

  /* Draw view-rect. */
  float pos[2], size[2];
  pos[0] = (v2d.cur.xmin - minimap_space.xmin) * minimap_scale + minimap_rect.xmin + offset_x;
  pos[1] = (v2d.cur.ymin - minimap_space.ymin) * minimap_scale + minimap_rect.ymin + offset_y;
  size[0] = BLI_rctf_size_x(&v2d.cur) * minimap_scale;
  size[1] = BLI_rctf_size_y(&v2d.cur) * minimap_scale;
  rctf viewport_rect;
  BLI_rctf_init(&viewport_rect,
                clamp_f(pos[0], minimap_rect.xmin, minimap_rect.xmax),
                clamp_f(pos[0] + size[0], minimap_rect.xmin, minimap_rect.xmax),
                clamp_f(pos[1], minimap_rect.ymin, minimap_rect.ymax),
                clamp_f(pos[1] + size[1], minimap_rect.ymin, minimap_rect.ymax));
  float viewport_rect_outline[4];
  ui::theme::get_color_shade_alpha_4fv(TH_BACK, 100, 0, viewport_rect_outline);
  ui::draw_roundbox_corner_set(ui::CNR_ALL);
  if (select) {
    ui::draw_roundbox_4fv_ex(&viewport_rect,
                            viewport_rect_outline,
                            nullptr,
                            1.0f,
                            node_color_outline_selected,
                            2.0f,
                            2.0f);
  }
  else {
    ui::draw_roundbox_4fv(&viewport_rect, false, 2.0f, viewport_rect_outline);
  }

  /* Draw outline of the minimap and another outline in background color to hide mask corners. */
  float space_node_background_color[4];
  ui::theme::get_color_shade_alpha_4fv(TH_BACK, 0, 0, space_node_background_color);
  rctf minimap_outer_rect;
  float minimap_outer_rect_offset = 6.0f;
  BLI_rctf_init(&minimap_outer_rect,
                minimap_rect.xmin - minimap_outer_rect_offset,
                minimap_rect.xmax + minimap_outer_rect_offset,
                minimap_rect.ymin - minimap_outer_rect_offset,
                minimap_rect.ymax + minimap_outer_rect_offset);
  ui::draw_roundbox_4fv_ex(&minimap_outer_rect,
                          nullptr,
                          nullptr,
                          1.0f,
                          space_node_background_color,
                          minimap_outer_rect_offset,
                          minimap_border_radius + minimap_outer_rect_offset);

  ui::draw_roundbox_4fv(&minimap_rect_back, false, minimap_border_radius, backdrop_color_outline);
}

static void gizmo_minimap_draw_select(const bContext *C, wmGizmo *gz, int /*select_id*/)
{
  minimap_draw_intern(C, gz, true);
}

static void gizmo_minimap_draw(const bContext *C, wmGizmo *gz)
{
  minimap_draw_intern(C, gz, false);
}

static int gizmo_minimap_test_select(bContext *C, wmGizmo * /*gz*/, const int mval[2])
{
  const float mval_fl[2] = {
    static_cast<float>(mval[0]),
    static_cast<float>(mval[1]),
  };
  SpaceNode *snode = CTX_wm_space_node(C);
  
  ARegion *region = CTX_wm_region(C);
  View2D v2d = region->v2d;


  float minimap_overlay_scale = snode->minimap_scale;
  float minimap_size = 150.0f * minimap_overlay_scale * UI_SCALE_FAC;
  float padding = 10.0f * UI_SCALE_FAC;
  

  float minimap_aspect_ratio = snode->minimap_aspect_ratio;

  float minimap_width = minimap_size * minimap_aspect_ratio;
  float minimap_height = minimap_size;
  if (minimap_aspect_ratio > 1) {
    minimap_width = minimap_size;
    minimap_height = minimap_size / minimap_aspect_ratio;
  }

  const rcti *rect_visible = ED_region_visible_rect(region);
  const float viewport_height = BLI_rcti_size_y(&v2d.mask);
  float viewport_width = BLI_rcti_size_x(&v2d.mask);
  float tile_height = viewport_height - BLI_rcti_size_y(rect_visible);
  float padding_top = padding;
  if (is_minimap_draw_top(snode)) {
    viewport_width = BLI_rcti_size_x(rect_visible);
    tile_height = 0;
    padding_top = viewport_height - minimap_height - padding;
  }

  rctf minimap_rect;
  BLI_rctf_init(&minimap_rect,
                viewport_width - padding - minimap_width,
                viewport_width - padding,
                padding_top + tile_height,
                padding_top + minimap_height + tile_height);
  if (BLI_rctf_isect_pt_v(&minimap_rect, mval_fl)) {
    return 1;
  }

  return -1;
}

static int gizmo_minimap_cursor_get(wmGizmo * /*gz*/)
{
  return WM_CURSOR_HAND;
}

static wmOperatorStatus gizmo_minimap_modal(bContext *C,
                                            wmGizmo *gz,
                                            const wmEvent *event,
                                            eWM_GizmoFlagTweak /*tweak_flag*/)
{
  if (event->type != MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }

  ARegion *region = CTX_wm_region(C);

  float mval[2] = {
    static_cast<float>(event->mval[0]),
    static_cast<float>(event->mval[1])
  };
  float mval_last[2];
  RNA_float_get_array(gz->ptr, "drag_last_pos", mval_last);
  RNA_float_set_array(gz->ptr, "drag_last_pos", mval);

  float delta_factor = RNA_float_get(gz->ptr, "delta_factor");
  float delta_x = (mval[0] - mval_last[0]) * delta_factor;
  float delta_y = (mval[1] - mval_last[1]) * delta_factor;

  PointerRNA op_ptr = WM_operator_properties_create("VIEW2D_OT_pan");
  RNA_int_set(&op_ptr, "deltax", delta_x);
  RNA_int_set(&op_ptr, "deltay", delta_y);

  WM_operator_name_call(C, "VIEW2D_OT_pan", wm::OpCallContext::ExecDefault, &op_ptr, nullptr);
  WM_operator_properties_free(&op_ptr);

  ED_region_tag_redraw(region);

  /* tag the region for redraw */
  ED_region_tag_redraw_editor_overlays(region);
  WM_event_add_mousemove(CTX_wm_window(C));

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus gizmo_minimap_invoke(bContext *C, wmGizmo *gz, const wmEvent *event)
{
  float mval[2] = {
    static_cast<float>(event->mval[0]),
    static_cast<float>(event->mval[1])
  };
  RNA_float_set_array(gz->ptr, "drag_start_pos", mval);
  RNA_float_set_array(gz->ptr, "drag_last_pos", mval);

  ARegion *region = CTX_wm_region(C);
  View2D v2d = region->v2d;
  SpaceNode *snode = CTX_wm_space_node(C);


  if ((snode->gizmo_flag & SNODE_GIZMO_HIDE) && !(snode->gizmo_flag & SNODE_GIZMO_SHOW_MINIMAP)) {
    return OPERATOR_CANCELLED;
  }
  const rcti *rect_visible = ED_region_visible_rect(region);
  const float viewport_height = BLI_rcti_size_y(&v2d.mask);
  float tile_height = 0;
  if (!is_minimap_draw_top(snode)) {
    tile_height = viewport_height - BLI_rcti_size_y(rect_visible);
  }

  const float zoom = (float)(BLI_rcti_size_x(&v2d.mask) + 1) / BLI_rctf_size_x(&v2d.cur);
  float min[2], max[2];
  INIT_MINMAX2(min, max);
  for (bNode &node : snode->edittree->nodes) {
    float pos_min[2] = {node.runtime->draw_bounds.xmin, node.runtime->draw_bounds.ymin};
    float pos_max[2] = {node.runtime->draw_bounds.xmax, node.runtime->draw_bounds.ymax};
    minmax_v2v2_v2(min, max, pos_min);
    minmax_v2v2_v2(min, max, pos_max);
  }
  float minimap_aspect_ratio = snode->minimap_aspect_ratio;
  float minimap_overlay_scale = snode->minimap_scale;

  float minimap_size = 150 * minimap_overlay_scale * UI_SCALE_FAC;
  float inner_padding = 10.0f * UI_SCALE_FAC;

  float minimap_width = minimap_size * minimap_aspect_ratio;
  float minimap_height = minimap_size;
  if (minimap_aspect_ratio > 1) {
    minimap_width = minimap_size;
    minimap_height = minimap_size / minimap_aspect_ratio;
  }

  float minimap_width_without_padding = minimap_width - inner_padding * 2;
  float minimap_height_without_padding = minimap_height - inner_padding * 2;
  rctf minimap_space;
  BLI_rctf_init(&minimap_space, min[0], max[0], min[1], max[1] + tile_height);
  float minimap_space_width = BLI_rctf_size_x(&minimap_space);
  float minimap_space_height = BLI_rctf_size_y(&minimap_space);
  float minimap_scale = min_ff(minimap_width_without_padding / minimap_space_width,
                               minimap_height_without_padding / minimap_space_height);

  float delta_factor = 1 / minimap_scale * zoom;
  RNA_float_set(gz->ptr, "delta_factor", delta_factor);

  return OPERATOR_RUNNING_MODAL;
}

/** \} */

void NODE_GT_minimap(wmGizmoType *gzt)
{
  /* identifiers */
  gzt->idname = "NODE_GT_minimap";

  /* api callbacks */
  gzt->draw = gizmo_minimap_draw;
  gzt->draw_select = gizmo_minimap_draw_select;
  gzt->test_select = gizmo_minimap_test_select;
  gzt->cursor_get = gizmo_minimap_cursor_get;

  gzt->modal = gizmo_minimap_modal;
  gzt->invoke = gizmo_minimap_invoke;

  gzt->struct_size = sizeof(NodeGizmoMinimap);

  /* rna */
  RNA_def_float_vector(gzt->srna,
                       "drag_start_pos",
                       2,
                       nullptr,
                       -FLT_MAX,
                       FLT_MAX,
                       "Drag Start Pos",
                       "",
                       -FLT_MAX,
                       FLT_MAX);
  RNA_def_float_vector(gzt->srna,
                       "drag_last_pos",
                       2,
                       nullptr,
                       -FLT_MAX,
                       FLT_MAX,
                       "Drag Last Pos",
                       "",
                       -FLT_MAX,
                       FLT_MAX);
  RNA_def_float_vector(gzt->srna,
                       "delta_factor",
                       1,
                       nullptr,
                       0,
                       FLT_MAX,
                       "Delta factor depending on node space and zoom",
                       "",
                       0,
                       FLT_MAX);
}

}  // namespace blender::ed::space_node