/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_listbase_iterator.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_string.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_node.hh"

#include "DNA_listBase.h"
#include "DNA_node_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "ED_gizmo_library.hh"

#include "IMB_imbuf_types.hh"

#include "NOD_compositor_gizmos.hh" /* Own include. */

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::nodes::gizmos {

constexpr float2 GIZMO_NODE_DEFAULT_DIMS{64.0f, 64.0f};

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

const SpaceNode *find_node_editor(const bContext *C)
{
  printf("find_node_editor\n");
  wmWindowManager *window_manager = CTX_wm_manager(C);

  for (wmWindow &window : window_manager->windows) {
    bScreen *screen = WM_window_get_active_screen(&window);
    for (ScrArea &area : screen->areabase) {
      SpaceLink *space_link = static_cast<SpaceLink *>(area.spacedata.first);
      if (!space_link || space_link->spacetype != SPACE_NODE) {
        continue;
      }
      const SpaceNode *snode = reinterpret_cast<const SpaceNode *>(space_link);
      if (snode->edittree && snode->edittree->type == NTREE_COMPOSIT) {
        bNodeTreePath *path = static_cast<bNodeTreePath *>(snode->treepath.last);
        if (snode->nodetree->active_viewer_key == path->parent_key) {
          printf("found\n");
          printf("\tsnode: %p\n", snode);
          printf("\tpath: %s\n", path->display_name);
          printf("\tactive viewer key: %i\n", snode->nodetree->active_viewer_key);
          printf("\tpath parent key: %i\n", path->parent_key);
          return snode;
        }
        else {
          printf("not found\n");
          printf("\tsnode: %p\n", snode);
          printf("\tpath: %s\n", path->display_name);
          printf("\tactive viewer key: %i\n", snode->nodetree->active_viewer_key);
          printf("\tpath parent key: %i\n", path->parent_key);
        }
      }
    }
  }

  return nullptr;
}

static float2 node_gizmo_safe_calc_dims(const ImBuf *ibuf, const float2 &fallback_dims)
{
  if (ibuf && ibuf->x > 0 && ibuf->y > 0) {
    return float2{float(ibuf->x), float(ibuf->y)};
  }

  /* We typically want to divide by dims, so avoid returning zero here. */
  BLI_assert(!math::is_any_zero(fallback_dims));
  return fallback_dims;
}

// todo(habib): generalize sima
static void node_gizmo_calc_matrix_space(const SpaceImage *sima,
                                         const ARegion *region,
                                         float matrix_space[4][4])
{
  unit_m4(matrix_space);
  mul_v3_fl(matrix_space[0], sima->zoom);
  mul_v3_fl(matrix_space[1], sima->zoom);
  matrix_space[3][0] = (region->winx / 2) - sima->xof * sima->zoom;
  matrix_space[3][1] = (region->winy / 2) - sima->yof * sima->zoom;
}

void WIDGETGROUP_node_box_mask_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  NodeBBoxWidgetGroup *mask_group = MEM_new<NodeBBoxWidgetGroup>(__func__);
  mask_group->border = WM_gizmo_new("GIZMO_GT_cage_2d", gzgroup, nullptr);

  RNA_enum_set(mask_group->border->ptr,
               "transform",
               ED_GIZMO_CAGE_XFORM_FLAG_TRANSLATE | ED_GIZMO_CAGE_XFORM_FLAG_ROTATE |
                   ED_GIZMO_CAGE_XFORM_FLAG_SCALE);

  RNA_enum_set(mask_group->border->ptr,
               "draw_options",
               ED_GIZMO_CAGE_DRAW_FLAG_XFORM_CENTER_HANDLE |
                   ED_GIZMO_CAGE_DRAW_FLAG_CORNER_HANDLES);

  gzgroup->customdata = mask_group;
  gzgroup->customdata_free = [](void *customdata) {
    MEM_delete(static_cast<NodeBBoxWidgetGroup *>(customdata));
  };
}

bool WIDGETGROUP_node_box_mask_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  // todo(habib): handle visibility
  const SpaceNode *snode = nodes::gizmos::find_node_editor(C);
  if (snode == nullptr || snode->edittree == nullptr) {
    return false;
  }

  bNode *node = bke::node_get_active(*snode->edittree);
  if (node == nullptr) {
    return false;
  }

  if (node && node->is_type("CompositorNodeBoxMask")) {
    snode->edittree->ensure_topology_cache();
    for (bNodeSocket &input : node->inputs) {
      if (STR_ELEM(input.name, "Position", "Size", "Rotation") && input.is_directly_linked()) {
        return false;
      }
    }
    return true;
  }

  return false;
}

void WIDGETGROUP_bbox_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  ARegion *region = CTX_wm_region(C);
  wmGizmo *gz = static_cast<wmGizmo *>(gzgroup->gizmos.first);

  // todo(habib): find relevant automaticall? Otherwise keep
  const SpaceNode *snode = find_node_editor(C);
  const SpaceImage *sima = CTX_wm_space_image(C);

  node_gizmo_calc_matrix_space(sima, region, gz->matrix_space);
}

static void gizmo_node_box_mask_prop_matrix_get(const wmGizmo *gz,
                                                wmGizmoProperty *gz_prop,
                                                void *value_p)
{
  float (*matrix)[4] = static_cast<float (*)[4]>(value_p);
  BLI_assert(gz_prop->type->array_length == 16);
  NodeBBoxWidgetGroup *mask_group = static_cast<NodeBBoxWidgetGroup *>(
      gz->parent_gzgroup->customdata);
  const float2 dims = mask_group->state.dims;
  const float2 offset = mask_group->state.offset;
  const bNode *node = static_cast<const bNode *>(gz_prop->custom_func.user_data);
  const float aspect = dims.x / dims.y;

  float loc[3], rot[3][3], size[3];
  mat4_to_loc_rot_size(loc, rot, size, matrix);

  const bNodeSocket *rotation_input = bke::node_find_socket(*node, SOCK_IN, "Rotation");
  const float rotation = rotation_input->default_value_typed<bNodeSocketValueFloat>()->value;
  axis_angle_to_mat3_single(rot, 'Z', rotation);

  const bNodeSocket *position_input = bke::node_find_socket(*node, SOCK_IN, "Position");
  const float2 position = position_input->default_value_typed<bNodeSocketValueVector>()->value;
  loc[0] = (position.x - 0.5) * dims.x + offset.x;
  loc[1] = (position.y - 0.5) * dims.y + offset.y;
  loc[2] = 0;

  const bNodeSocket *size_input = bke::node_find_socket(*node, SOCK_IN, "Size");
  const float2 size_value = size_input->default_value_typed<bNodeSocketValueVector>()->value;
  size[0] = size_value.x;
  size[1] = size_value.y * aspect;
  size[2] = 1;

  loc_rot_size_to_mat4(matrix, loc, rot, size);
}

static void gizmo_node_bbox_update(NodeBBoxWidgetGroup *bbox_group)
{
  RNA_property_update(
      bbox_group->update_data.context, &bbox_group->update_data.ptr, bbox_group->update_data.prop);
}

static void gizmo_node_box_mask_prop_matrix_set(const wmGizmo *gz,
                                                wmGizmoProperty *gz_prop,
                                                const void *value_p)
{
  const float (*matrix)[4] = static_cast<const float (*)[4]>(value_p);
  BLI_assert(gz_prop->type->array_length == 16);
  NodeBBoxWidgetGroup *mask_group = static_cast<NodeBBoxWidgetGroup *>(
      gz->parent_gzgroup->customdata);
  const float2 dims = mask_group->state.dims;
  const float2 offset = mask_group->state.offset;
  bNode *node = static_cast<bNode *>(gz_prop->custom_func.user_data);

  bNodeSocket *position_input = bke::node_find_socket(*node, SOCK_IN, "Position");
  const float2 position = position_input->default_value_typed<bNodeSocketValueVector>()->value;

  bNodeSocket *size_input = bke::node_find_socket(*node, SOCK_IN, "Size");
  const float2 size_value = size_input->default_value_typed<bNodeSocketValueVector>()->value;

  const float aspect = dims.x / dims.y;
  rctf rct;
  rct.xmin = position.x - size_value.x / 2;
  rct.xmax = position.x + size_value.x / 2;
  rct.ymin = position.y - size_value.y / 2;
  rct.ymax = position.y + size_value.y / 2;

  float loc[3];
  float rot[3][3];
  float size[3];
  mat4_to_loc_rot_size(loc, rot, size, matrix);

  float eul[3];

  /* Rotation can't be extracted from matrix when the gizmo width or height is zero. */
  if (size[0] != 0 and size[1] != 0) {
    mat4_to_eul(eul, matrix);
    bNodeSocket *rotation_input = bke::node_find_socket(*node, SOCK_IN, "Rotation");
    rotation_input->default_value_typed<bNodeSocketValueFloat>()->value = eul[2];
  }

  BLI_rctf_resize(&rct, fabsf(size[0]), fabsf(size[1]) / aspect);
  BLI_rctf_recenter(
      &rct, ((loc[0] - offset.x) / dims.x) + 0.5, ((loc[1] - offset.y) / dims.y) + 0.5);

  size_input->default_value_typed<bNodeSocketValueVector>()->value[0] = size[0];
  size_input->default_value_typed<bNodeSocketValueVector>()->value[1] = size[1] / aspect;
  position_input->default_value_typed<bNodeSocketValueVector>()->value[0] = rct.xmin + size[0] / 2;
  position_input->default_value_typed<bNodeSocketValueVector>()->value[1] = rct.ymin +
                                                                            size[1] / aspect / 2;

  gizmo_node_bbox_update(mask_group);
}

void WIDGETGROUP_node_mask_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  Main *bmain = CTX_data_main(C);
  NodeBBoxWidgetGroup *mask_group = static_cast<NodeBBoxWidgetGroup *>(gzgroup->customdata);
  wmGizmo *gz = mask_group->border;

  void *lock;
  Image *ima = BKE_image_ensure_viewer(bmain, IMA_TYPE_COMPOSITE, "Render Result");
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, nullptr, &lock);

  if (UNLIKELY(ibuf == nullptr)) {
    WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, true);
    BKE_image_release_ibuf(ima, ibuf, lock);
    return;
  }

  mask_group->state.dims = node_gizmo_safe_calc_dims(ibuf, GIZMO_NODE_DEFAULT_DIMS);
  mask_group->state.offset = ibuf->flags & IB_has_display_window ? float2(ibuf->display_offset) :
                                                                   float2(0.0f);

  RNA_float_set_array(gz->ptr, "dimensions", mask_group->state.dims);
  WM_gizmo_set_flag(gz, WM_GIZMO_HIDDEN, false);

  const SpaceNode *snode = find_node_editor(C);
  if (snode == nullptr) {
    // BLI_assert();
    printf("snode is nullptr\n");
    return;
  }
  bNode *node = bke::node_get_active(*snode->edittree);

  mask_group->update_data.context = const_cast<bContext *>(C);
  bNodeSocket *source_input = bke::node_find_socket(*node, SOCK_IN, "Mask");
  mask_group->update_data.ptr = RNA_pointer_create_discrete(
      reinterpret_cast<ID *>(snode->edittree), RNA_NodeSocket, source_input);
  mask_group->update_data.prop = RNA_struct_find_property(&mask_group->update_data.ptr, "enabled");
  BLI_assert(mask_group->update_data.prop != nullptr);

  wmGizmoPropertyFnParams params{};
  params.value_get_fn = gizmo_node_box_mask_prop_matrix_get;
  params.value_set_fn = gizmo_node_box_mask_prop_matrix_set;
  params.range_get_fn = nullptr;
  params.user_data = node;
  WM_gizmo_target_property_def_func(gz, "matrix", &params);

  BKE_image_release_ibuf(ima, ibuf, lock);
}

}  // namespace blender::nodes::gizmos
