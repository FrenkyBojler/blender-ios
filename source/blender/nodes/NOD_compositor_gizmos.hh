/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

struct bContext;
struct wmGizmoGroup;
struct ARegion;
struct wmGizmoGroupType;

namespace blender::nodes::gizmos {

/* -------------------------------------------------------------------- */
/** \name Box Mask
 * \{ */

bool box_mask_show(const SpaceNode &snode);
void WIDGETGROUP_node_box_mask_setup(const bContext *C, wmGizmoGroup *gzgroup);
void WIDGETGROUP_node_mask_refresh(const bContext *C, wmGizmoGroup *gzgroup);
void WIDGETGROUP_bbox_draw_prepare_space_node(const bContext *C, wmGizmoGroup *gzgroup);
void WIDGETGROUP_bbox_draw_prepare_space_image(const bContext *C, wmGizmoGroup *gzgroup);
bool WIDGETGROUP_node_box_mask_poll_space_image(const bContext *C, wmGizmoGroupType *gzgt);
bool WIDGETGROUP_node_box_mask_poll_space_node(const bContext *C, wmGizmoGroupType *gzgt);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Crop
 * \{ */

bool crop_show(const SpaceNode &snode);
void WIDGETGROUP_node_crop_refresh(const bContext *C, wmGizmoGroup *gzgroup);
void WIDGETGROUP_node_crop_setup(const bContext *C, wmGizmoGroup *gzgroup);
bool WIDGETGROUP_node_crop_poll_space_node(const bContext *C, wmGizmoGroupType *gzgt);
void WIDGETGROUP_node_crop_draw_prepare_space_node(const bContext *C, wmGizmoGroup *gzgroup);
bool WIDGETGROUP_node_crop_poll_space_image(const bContext *C, wmGizmoGroupType *gzgt);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glare
 * \{ */

void WIDGETGROUP_node_glare_setup(const bContext *C, wmGizmoGroup *gzgroup);
void WIDGETGROUP_node_glare_refresh(const bContext *C, wmGizmoGroup *gzgroup);
bool WIDGETGROUP_node_glare_poll_space_image(const bContext *C, wmGizmoGroupType *gzgt);
void WIDGETGROUP_node_glare_draw_prepare_space_image(const bContext *C, wmGizmoGroup *gzgroup);
bool WIDGETGROUP_node_glare_poll_space_node(const bContext *C, wmGizmoGroupType *gzgt);
void WIDGETGROUP_node_glare_draw_prepare_space_node(const bContext *C, wmGizmoGroup *gzgroup);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Corner Pin
 * \{ */

void WIDGETGROUP_node_corner_pin_setup(const bContext *C, wmGizmoGroup *gzgroup);
void WIDGETGROUP_node_corner_pin_refresh(const bContext *C, wmGizmoGroup *gzgroup);
bool WIDGETGROUP_node_corner_pin_poll_space_image(const bContext *C, wmGizmoGroupType *gzgt);
bool WIDGETGROUP_node_corner_pin_poll_space_node(const bContext *C, wmGizmoGroupType *gzgt);
void WIDGETGROUP_node_corner_pin_draw_prepare_space_image(const bContext *C,
                                                          wmGizmoGroup *gzgroup);
void WIDGETGROUP_node_corner_pin_draw_prepare_space_node(const bContext *C, wmGizmoGroup *gzgroup);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Ellipse Mask
 * \{ */

void WIDGETGROUP_node_ellipse_mask_setup(const bContext *C, wmGizmoGroup *gzgroup);
bool WIDGETGROUP_node_ellipse_mask_poll_space_image(const bContext *C, wmGizmoGroupType *gzgt);
bool WIDGETGROUP_node_ellipse_mask_poll_space_node(const bContext *C, wmGizmoGroupType *gzgt);

bool show_ellipse_mask(const SpaceNode &snode);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Split
 * \{ */

void WIDGETGROUP_node_split_refresh(const bContext *C, wmGizmoGroup *gzgroup);
void WIDGETGROUP_node_split_setup(const bContext *C, wmGizmoGroup *gzgroup);
bool WIDGETGROUP_node_split_poll_space_node(const bContext *C, wmGizmoGroupType *gzgt);
bool WIDGETGROUP_node_split_poll_space_image(const bContext *C, wmGizmoGroupType *gzgt);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Backdrop Gizmo
 * \{ */

bool WIDGETGROUP_node_transform_poll(const bContext *C, wmGizmoGroupType *gzgt);
void WIDGETGROUP_node_transform_refresh(const bContext *C, wmGizmoGroup *gzgroup);
void WIDGETGROUP_node_transform_setup(const bContext *C, wmGizmoGroup *gzgroup);

/** \} */

}  // namespace blender::nodes::gizmos
