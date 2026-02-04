/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

// todo(habib): doxygen doc

struct bContext;
struct wmGizmoGroup;
struct ARegion;
struct wmGizmoGroupType;

namespace blender::nodes::gizmos {

SpaceNode *find_node_editor(const bContext *C);

void node_gizmo_calc_matrix_space(const ARegion *region,
                                  const float zoom,
                                  const float2 offset,
                                  float matrix_space[4][4]);
void node_gizmo_calc_matrix_space_with_image_dims(const ARegion *region,
                                                  const float zoom,
                                                  const float2 space_offset,
                                                  const float2 &image_dims,
                                                  const float2 &image_offset,
                                                  float matrix_space[4][4]);

/* -------------------------------------------------------------------- */
/** \name Box Mask
 * \{ */

bool box_mask_show(const SpaceNode &snode);
void WIDGETGROUP_node_box_mask_setup(const bContext *C, wmGizmoGroup *gzgroup);
void WIDGETGROUP_node_mask_refresh(const bContext *C, wmGizmoGroup *gzgroup);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Crop
 * \{ */

bool crop_show(const SpaceNode &snode);
void WIDGETGROUP_node_crop_refresh(const bContext *C, wmGizmoGroup *gzgroup);
void WIDGETGROUP_node_crop_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glare
 * \{ */

struct NodeGlareWidgetGroup {
  wmGizmo *gizmo;

  struct {
    float2 dims;
    float2 offset;
  } state;
};

void WIDGETGROUP_node_glare_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup);
void WIDGETGROUP_node_glare_refresh(const bContext *C, wmGizmoGroup *gzgroup);
bool show_glare(const SpaceNode &snode);

/** \} */

}  // namespace blender::nodes::gizmos
