/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spnode
 */

#include <cmath>

#include "BLI_listbase.h"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_node_runtime.hh"

#include "ED_gizmo_library.hh"
#include "ED_screen.hh"

#include "IMB_imbuf_types.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "NOD_compositor_gizmos.hh"

#include "WM_types.hh"

#include "node_intern.hh"

namespace blender::ed::space_node {

/* -------------------------------------------------------------------- */
/** \name Local Utilities
 * \{ */

// todo (habib): move to NOD_compositor_gizmos
static const float2 GIZMO_NODE_DEFAULT_DIMS{64.0f, 64.0f};
static float2 node_gizmo_safe_calc_dims(const ImBuf *ibuf, const float2 &fallback_dims)
{
  if (ibuf && ibuf->x > 0 && ibuf->y > 0) {
    return float2{float(ibuf->x), float(ibuf->y)};
  }

  /* We typically want to divide by dims, so avoid returning zero here. */
  BLI_assert(!math::is_any_zero(fallback_dims));
  return fallback_dims;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Backdrop Gizmo
 * \{ */

static void gizmo_node_backdrop_prop_matrix_get(const wmGizmo * /*gz*/,
                                                wmGizmoProperty *gz_prop,
                                                void *value_p)
{
  float (*matrix)[4] = static_cast<float (*)[4]>(value_p);
  BLI_assert(gz_prop->type->array_length == 16);
  const SpaceNode *snode = static_cast<const SpaceNode *>(gz_prop->custom_func.user_data);
  matrix[0][0] = snode->zoom;
  matrix[1][1] = snode->zoom;
  matrix[3][0] = snode->xof;
  matrix[3][1] = snode->yof;
}

static void gizmo_node_backdrop_prop_matrix_set(const wmGizmo * /*gz*/,
                                                wmGizmoProperty *gz_prop,
                                                const void *value_p)
{
  const float (*matrix)[4] = static_cast<const float (*)[4]>(value_p);
  BLI_assert(gz_prop->type->array_length == 16);
  SpaceNode *snode = static_cast<SpaceNode *>(gz_prop->custom_func.user_data);
  snode->zoom = matrix[0][0];
  snode->xof = matrix[3][0];
  snode->yof = matrix[3][1];
}

static bool WIDGETGROUP_node_transform_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  if (snode == nullptr) {
    return false;
  }
  if (!nodes::gizmos::node_gizmo_is_set_visible(*snode)) {
    return false;
  }

  bNode *node = bke::node_get_active(*snode->edittree);

  if (node && node->is_type("CompositorNodeViewer")) {
    return true;
  }

  return false;
}

static void WIDGETGROUP_node_transform_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  wmGizmoWrapper *wwrapper = MEM_new_uninitialized<wmGizmoWrapper>(__func__);

  wwrapper->gizmo = WM_gizmo_new("GIZMO_GT_cage_2d", gzgroup, nullptr);

  RNA_enum_set(wwrapper->gizmo->ptr,
               "transform",
               ED_GIZMO_CAGE_XFORM_FLAG_TRANSLATE | ED_GIZMO_CAGE_XFORM_FLAG_SCALE_UNIFORM);

  gzgroup->customdata = wwrapper;
}

static void WIDGETGROUP_node_transform_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  Main *bmain = CTX_data_main(C);
  wmGizmo *cage = (static_cast<wmGizmoWrapper *>(gzgroup->customdata))->gizmo;
  const ARegion *region = CTX_wm_region(C);
  /* center is always at the origin */
  const float origin[3] = {float(region->winx / 2), float(region->winy / 2), 0.0f};

  void *lock;
  Image *ima = BKE_image_ensure_viewer(bmain, IMA_TYPE_COMPOSITE, "Viewer Node");
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, nullptr, &lock);

  if (UNLIKELY(ibuf == nullptr)) {
    WM_gizmo_set_flag(cage, WM_GIZMO_HIDDEN, true);
    BKE_image_release_ibuf(ima, ibuf, lock);
    return;
  }

  const float2 dims = node_gizmo_safe_calc_dims(ibuf, GIZMO_NODE_DEFAULT_DIMS);

  RNA_float_set_array(cage->ptr, "dimensions", dims);
  WM_gizmo_set_matrix_location(cage, origin);
  WM_gizmo_set_flag(cage, WM_GIZMO_HIDDEN, false);

  /* Need to set property here for undo. TODO: would prefer to do this in _init. */
  SpaceNode *snode = CTX_wm_space_node(C);
#if 0
  PointerRNA nodeptr = RNA_pointer_create_discrete(snode->id, RNA_SpaceNodeEditor, snode);
  WM_gizmo_target_property_def_rna(cage, "offset", &nodeptr, "backdrop_offset", -1);
  WM_gizmo_target_property_def_rna(cage, "scale", &nodeptr, "backdrop_zoom", -1);
#endif

  wmGizmoPropertyFnParams params{};
  params.value_get_fn = gizmo_node_backdrop_prop_matrix_get;
  params.value_set_fn = gizmo_node_backdrop_prop_matrix_set;
  params.range_get_fn = nullptr;
  params.user_data = snode;
  WM_gizmo_target_property_def_func(cage, "matrix", &params);

  BKE_image_release_ibuf(ima, ibuf, lock);
}

void NODE_GGT_backdrop_transform(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Backdrop Transform Widget";
  gzgt->idname = "NODE_GGT_backdrop_transform";

  gzgt->flag |= WM_GIZMOGROUPTYPE_PERSISTENT;

  gzgt->poll = WIDGETGROUP_node_transform_poll;
  gzgt->setup = WIDGETGROUP_node_transform_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = WIDGETGROUP_node_transform_refresh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Crop Gizmo
 * \{ */

struct NodeBBoxWidgetGroup {
  wmGizmo *border;

  struct {
    float2 dims;
    float2 offset;
  } state;

  struct {
    PointerRNA ptr;
    PropertyRNA *prop;
    bContext *context;
  } update_data;
};

static bool WIDGETGROUP_node_crop_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  if (snode == nullptr) {
    return false;
  }
  if (!nodes::gizmos::node_gizmo_is_set_visible(*snode)) {
    return false;
  }

  return nodes::gizmos::crop_show(*snode);
}

static void WIDGETGROUP_node_crop_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  ARegion *region = CTX_wm_region(C);
  wmGizmo *gz = static_cast<wmGizmo *>(gzgroup->gizmos.first);

  SpaceNode *snode = CTX_wm_space_node(C);

  nodes::gizmos::node_gizmo_calc_matrix_space(
      region, snode->zoom, {-snode->xof, -snode->yof}, gz->matrix_space);
}

void NODE_GGT_backdrop_crop(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Backdrop Crop Widget";
  gzgt->idname = "NODE_GGT_backdrop_crop";

  gzgt->flag |= WM_GIZMOGROUPTYPE_PERSISTENT;

  gzgt->poll = WIDGETGROUP_node_crop_poll;
  gzgt->setup = nodes::gizmos::WIDGETGROUP_node_crop_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->draw_prepare = WIDGETGROUP_node_crop_draw_prepare;
  gzgt->refresh = nodes::gizmos::WIDGETGROUP_node_crop_refresh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Box Mask
 * \{ */

void NODE_GGT_backdrop_box_mask(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Backdrop Box Mask Widget";
  gzgt->idname = "NODE_GGT_backdrop_box_mask";

  gzgt->flag |= WM_GIZMOGROUPTYPE_PERSISTENT;

  gzgt->poll = nodes::gizmos::WIDGETGROUP_node_box_mask_poll_space_node;
  gzgt->setup = nodes::gizmos::WIDGETGROUP_node_box_mask_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->draw_prepare = nodes::gizmos::WIDGETGROUP_bbox_draw_prepare_space_node;
  gzgt->refresh = nodes::gizmos::WIDGETGROUP_node_mask_refresh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Ellipse Mask
 * \{ */

static bool WIDGETGROUP_node_ellipse_mask_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  if (snode == nullptr) {
    return false;
  }
  if (!nodes::gizmos::node_gizmo_is_set_visible(*snode)) {
    return false;
  }

  return nodes::gizmos::show_ellipse_mask(*snode);
}

void NODE_GGT_backdrop_ellipse_mask(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Backdrop Ellipse Mask Widget";
  gzgt->idname = "NODE_GGT_backdrop_ellipse_mask";

  gzgt->flag |= WM_GIZMOGROUPTYPE_PERSISTENT;

  gzgt->poll = WIDGETGROUP_node_ellipse_mask_poll;
  gzgt->setup = nodes::gizmos::WIDGETGROUP_node_ellipse_mask_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->draw_prepare = nodes::gizmos::WIDGETGROUP_bbox_draw_prepare_space_node;
  gzgt->refresh = nodes::gizmos::WIDGETGROUP_node_mask_refresh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glare
 * \{ */

static bool WIDGETGROUP_node_glare_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  if (snode == nullptr) {
    return false;
  }
  if (!nodes::gizmos::node_gizmo_is_set_visible(*snode)) {
    return false;
  }

  return nodes::gizmos::show_glare(*snode);
}

static void WIDGETGROUP_node_glare_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  using namespace nodes::gizmos;

  NodeGlareWidgetGroup *glare_group = static_cast<NodeGlareWidgetGroup *>(gzgroup->customdata);
  ARegion *region = CTX_wm_region(C);
  wmGizmo *gz = static_cast<wmGizmo *>(gzgroup->gizmos.first);

  SpaceNode *snode = CTX_wm_space_node(C);

  nodes::gizmos::node_gizmo_calc_matrix_space_with_image_dims(region,
                                                              snode->zoom,
                                                              {-snode->xof, -snode->yof},
                                                              glare_group->state.dims,
                                                              glare_group->state.offset,
                                                              gz->matrix_space);
}

void NODE_GGT_backdrop_glare(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Glare Widget";
  gzgt->idname = "NODE_GGT_glare";

  gzgt->flag |= WM_GIZMOGROUPTYPE_PERSISTENT;

  gzgt->poll = WIDGETGROUP_node_glare_poll;
  gzgt->setup = nodes::gizmos::WIDGETGROUP_node_glare_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->draw_prepare = WIDGETGROUP_node_glare_draw_prepare;
  gzgt->refresh = nodes::gizmos::WIDGETGROUP_node_glare_refresh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Corner Pin
 * \{ */

static bool WIDGETGROUP_node_corner_pin_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  if (snode == nullptr) {
    return false;
  }
  if (!nodes::gizmos::node_gizmo_is_set_visible(*snode)) {
    return false;
  }

  return nodes::gizmos::show_corner_pin(*snode);
}

static void WIDGETGROUP_node_corner_pin_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  using namespace nodes::gizmos;

  NodeCornerPinWidgetGroup *cpin_group = static_cast<NodeCornerPinWidgetGroup *>(
      gzgroup->customdata);
  ARegion *region = CTX_wm_region(C);

  SpaceNode *snode = CTX_wm_space_node(C);

  float matrix_space[4][4];
  nodes::gizmos::node_gizmo_calc_matrix_space_with_image_dims(region,
                                                              snode->zoom,
                                                              {-snode->xof, -snode->yof},
                                                              cpin_group->state.dims,
                                                              cpin_group->state.offset,
                                                              matrix_space);

  for (int i = 0; i < 4; i++) {
    wmGizmo *gz = cpin_group->gizmos[i];
    copy_m4_m4(gz->matrix_space, matrix_space);
  }
}

void NODE_GGT_backdrop_corner_pin(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Corner Pin Widget";
  gzgt->idname = "NODE_GGT_backdrop_corner_pin";

  gzgt->flag |= WM_GIZMOGROUPTYPE_PERSISTENT;

  gzgt->poll = WIDGETGROUP_node_corner_pin_poll;
  gzgt->setup = nodes::gizmos::WIDGETGROUP_node_corner_pin_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->draw_prepare = WIDGETGROUP_node_corner_pin_draw_prepare;
  gzgt->refresh = nodes::gizmos::WIDGETGROUP_node_corner_pin_refresh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Split
 * \{ */

static bool WIDGETGROUP_node_split_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  if (snode == nullptr) {
    return false;
  }
  if (!nodes::gizmos::node_gizmo_is_set_visible(*snode)) {
    return false;
  }

  return nodes::gizmos::show_split(*snode);
}

void NODE_GGT_backdrop_split(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Split Widget";
  gzgt->idname = "NODE_GGT_backdrop_split";

  gzgt->flag |= WM_GIZMOGROUPTYPE_PERSISTENT;

  gzgt->poll = WIDGETGROUP_node_split_poll;
  gzgt->setup = nodes::gizmos::WIDGETGROUP_node_split_setup;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->draw_prepare = nodes::gizmos::WIDGETGROUP_bbox_draw_prepare_space_node;
  gzgt->refresh = nodes::gizmos::WIDGETGROUP_node_split_refresh;
}

/** \} */

}  // namespace blender::ed::space_node
