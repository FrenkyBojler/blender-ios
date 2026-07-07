/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * \name Window-Manager XR Drawing
 *
 * Implements Blender specific drawing functionality for use with the Ghost-XR API.
 */

#include <cfloat>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "DNA_userdef_types.h"
#include "DNA_screen_types.h"

#include "BLI_listbase.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_geom.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_math_rotation_c.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_rect.hh"
#include "BLI_string.hh"
#include "BLI_time.hh"

#include "BKE_global.hh"
#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "ED_view3d_offscreen.hh"
#include "ED_screen.hh"
#include "UI_interface_c.hh"
#include "UI_view2d.hh"
#include "DNA_view3d_types.h"

#include "GHOST_Xr-api.hh"

#include "GPU_batch_presets.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_viewport.hh"
#include "GPU_framebuffer.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "MEM_guardedalloc.h"

#include "wm_xr_intern.hh"

namespace blender {
  
extern CLG_LogRef LOG;

#define XR_PANELS_TRACE(...) ((void)0)

struct wmXrPanelHostContextOverride {
  bContext *C;
  wmWindow *prev_win;
  ScrArea *prev_area;
  ARegion *prev_region;

  wmXrPanelHostContextOverride(bContext *context, const wmXrPanel *panel) : C(context)
  {
    prev_win = CTX_wm_window(C);
    prev_area = CTX_wm_area(C);
    prev_region = CTX_wm_region(C);

    CTX_wm_window_set(C, panel->panel_host_win);
    CTX_wm_area_set(C, panel->panel_host_area);
    CTX_wm_region_set(C, panel->panel_host_region);
  }

  ~wmXrPanelHostContextOverride()
  {
    CTX_wm_window_set(C, prev_win);
    CTX_wm_area_set(C, prev_area);
    CTX_wm_region_set(C, prev_region);
  }
};

struct wmXrTempRegionContextOverride {
  bContext *C;
  wmWindow *prev_win;
  ScrArea *prev_area;
  ARegion *prev_region;
  ARegion *prev_region_popup;

  wmXrTempRegionContextOverride(bContext *context, const wmXrPanel *panel, ARegion *popup_region)
      : C(context)
  {
    prev_win = CTX_wm_window(C);
    prev_area = CTX_wm_area(C);
    prev_region = CTX_wm_region(C);
    prev_region_popup = CTX_wm_region_popup(C);

    CTX_wm_window_set(C, panel->panel_host_win);
    CTX_wm_area_set(C, panel->panel_host_area);
    CTX_wm_region_set(C, popup_region);
    CTX_wm_region_popup_set(C, popup_region);
  }

  ~wmXrTempRegionContextOverride()
  {
    CTX_wm_window_set(C, prev_win);
    CTX_wm_area_set(C, prev_area);
    CTX_wm_region_set(C, prev_region);
    CTX_wm_region_popup_set(C, prev_region_popup);
  }
};

static wmXrPanel *wm_xr_panel_find(wmXrSurfaceData *surface_data,
                                   const wmWindow *win,
                                   const ScrArea *area,
                                   const ARegion *region,
                                   const eWMXrPanelMountPoint mount_point)
{
  if (surface_data == nullptr) {
    return nullptr;
  }

  for (wmXrPanel *panel : ListBaseWrapper<wmXrPanel>(surface_data->panels)) {
    if (panel->panel_host_win == win && panel->panel_host_area == area &&
        panel->panel_host_region == region && panel->mount_point == mount_point)
    {
      return panel;
    }
  }
  return nullptr;
}

static wmXrPanel *wm_xr_panel_find_by_host(wmXrSurfaceData *surface_data,
                                           const wmWindow *win,
                                           const ScrArea *area,
                                           const ARegion *region)
{
  if (surface_data == nullptr || win == nullptr || area == nullptr || region == nullptr) {
    return nullptr;
  }

  if (surface_data->active_panel != nullptr && surface_data->active_panel->panel_host_win == win &&
      surface_data->active_panel->panel_host_area == area &&
      surface_data->active_panel->panel_host_region == region)
  {
    return surface_data->active_panel;
  }

  wmXrPanel *match = nullptr;
  for (wmXrPanel *panel : ListBaseWrapper<wmXrPanel>(surface_data->panels)) {
    if (panel->panel_host_win != win || panel->panel_host_area != area ||
        panel->panel_host_region != region)
    {
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = panel;
  }

  return match;
}

static wmXrPanel *wm_xr_panel_find_by_host_region(wmXrSurfaceData *surface_data,
                                                  const wmWindow *win,
                                                  const ARegion *region)
{
  if (surface_data == nullptr || win == nullptr || region == nullptr) {
    return nullptr;
  }

  if (surface_data->active_panel != nullptr && surface_data->active_panel->panel_host_win == win &&
      surface_data->active_panel->panel_host_region == region)
  {
    return surface_data->active_panel;
  }

  wmXrPanel *match = nullptr;
  for (wmXrPanel *panel : ListBaseWrapper<wmXrPanel>(surface_data->panels)) {
    if (panel->panel_host_win != win || panel->panel_host_region != region) {
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = panel;
  }

  return match;
}

static wmXrTempRegion *wm_xr_temp_region_find(wmXrPanel *panel, const ARegion *region)
{
  if (panel == nullptr || region == nullptr) {
    return nullptr;
  }

  for (wmXrTempRegion *temp_region : ListBaseWrapper<wmXrTempRegion>(panel->temporary_regions)) {
    if (temp_region->region == region) {
      return temp_region;
    }
  }

  return nullptr;
}

static wmXrTempRegion *wm_xr_temp_region_find_any(wmXrSurfaceData *surface_data,
                                                  const ARegion *region,
                                                  wmXrPanel **r_panel)
{
  if (r_panel != nullptr) {
    *r_panel = nullptr;
  }
  if (surface_data == nullptr || region == nullptr) {
    return nullptr;
  }

  for (wmXrPanel *panel : ListBaseWrapper<wmXrPanel>(surface_data->panels)) {
    if (wmXrTempRegion *temp_region = wm_xr_temp_region_find(panel, region)) {
      if (r_panel != nullptr) {
        *r_panel = panel;
      }
      return temp_region;
    }
  }

  return nullptr;
}

/* Resolves XR ownership once when a temporary region is created. 
 * The source may be the XR host region itself, another already-registered XR temp region,
 * or the currently active XR panel for operator-driven popup chains.
 */
static wmXrPanel *wm_xr_panel_find_by_source_region(wmXrSurfaceData *surface_data,
                                                    wmWindow *win,
                                                    ScrArea *area,
                                                    ARegion *source_region)
{
  if (surface_data == nullptr || source_region == nullptr) {
    return nullptr;
  }

  if (source_region->regiontype == RGN_TYPE_XR) {
    if (area == nullptr) {
      return wm_xr_panel_find_by_host_region(surface_data, win, source_region);
    }
    return wm_xr_panel_find_by_host(surface_data, win, area, source_region);
  }

  wmXrPanel *panel = nullptr;
  wm_xr_temp_region_find_any(surface_data, source_region, &panel);
  if (panel != nullptr) {
    return panel;
  }

  if (area != nullptr && surface_data->active_panel != nullptr &&
      surface_data->active_panel->panel_host_win == win &&
      surface_data->active_panel->panel_host_area == area)
  {
    return surface_data->active_panel;
  }

  return nullptr;
}

static bool wm_xr_temp_region_rect_update(const wmWindow *win, wmXrTempRegion *temp_region)
{
  if (win == nullptr || temp_region == nullptr || temp_region->region == nullptr) {
    return false;
  }

  rcti clipped_rect = temp_region->region->winrct;
  if (win->runtime != nullptr && win->runtime->is_virtual) {
    if (BLI_rcti_size_x(&clipped_rect) <= 0 || BLI_rcti_size_y(&clipped_rect) <= 0) {
      temp_region->valid = false;
      return false;
    }
    temp_region->region_rect = clipped_rect;
    return true;
  }
  const rcti window_bounds = {
      0, std::max(0, int(win->sizex) - 1), 0, std::max(0, int(win->sizey) - 1)};
  if (!BLI_rcti_isect(&clipped_rect, &window_bounds, &clipped_rect)) {
    temp_region->valid = false;
    return false;
  }

  temp_region->region_rect = clipped_rect;
  return true;
}

static bool wm_xr_temp_region_offscreen_ensure(wmXrTempRegion *temp_region,
                                               const int px_width,
                                               const int px_height)
{
  if (temp_region == nullptr || px_width <= 0 || px_height <= 0) {
    return false;
  }

  if (temp_region->offscreen != nullptr) {
    if (GPU_offscreen_width(temp_region->offscreen) == px_width &&
        GPU_offscreen_height(temp_region->offscreen) == px_height)
    {
      return true;
    }

    GPU_offscreen_free(temp_region->offscreen);
    temp_region->offscreen = nullptr;
  }

  temp_region->offscreen = GPU_offscreen_create(px_width,
                                                px_height,
                                                false,
                                                gpu::TextureFormat::SRGBA_8_8_8_8,
                                                GPU_TEXTURE_USAGE_SHADER_READ,
                                                false,
                                                nullptr);
  return temp_region->offscreen != nullptr;
}

static void wm_xr_temp_region_draw_direct(const bContext *C,
                                          const wmXrPanel *panel,
                                          ARegion *region)
{
  if (C == nullptr || panel == nullptr || region == nullptr || region->runtime == nullptr ||
      region->runtime->type == nullptr || region->runtime->type->draw == nullptr)
  {
    return;
  }

  region->runtime->do_draw |= RGN_DRAWING;
  wmPartialViewport(&region->runtime->drawrct, &region->winrct, &region->runtime->drawrct);
  wmOrtho2_region_pixelspace(region);
  ui::theme::theme_set(panel->panel_host_area ? panel->panel_host_area->spacetype : 0,
                       region->runtime->type->regionid);
  /* Temporary popups depend on their own region draw callback; using the generic region path
   * regressed into uniform background-only output in XR. */
  region->runtime->type->draw(const_cast<bContext *>(C), region);
  ED_region_pixelspace(region);
  region->runtime->drawrct = rcti{};
  region->runtime->do_draw &= ~RGN_DRAWING;
}

static bool wm_xr_temp_region_cache_update(const bContext *C,
                                           wmXrPanel *panel,
                                           wmXrTempRegion *temp_region)
{
  if (C == nullptr || panel == nullptr || temp_region == nullptr || temp_region->region == nullptr ||
      panel->panel_host_win == nullptr)
  {
    return false;
  }

  ARegion *region = temp_region->region;
  if (region->runtime == nullptr || region->runtime->type == nullptr) {
    return false;
  }
  if (!wm_xr_temp_region_rect_update(panel->panel_host_win, temp_region)) {
    return false;
  }

  const int px_width = BLI_rcti_size_x(&temp_region->region_rect) + 1;
  const int px_height = BLI_rcti_size_y(&temp_region->region_rect) + 1;
  if (px_width <= 0 || px_height <= 0) {
    return false;
  }

  bContext *mutable_C = const_cast<bContext *>(C);
  wmXrTempRegionContextOverride context_override(mutable_C, panel, region);
  rcti winrct_prev = region->winrct;
  const int winx_prev = region->winx;
  const int winy_prev = region->winy;
  region->winx = px_width;
  region->winy = px_height;
  region->runtime->visible = true;

  if (region->runtime->type != nullptr && region->runtime->type->layout != nullptr) {
    wmViewport(&region->winrct);
    region->runtime->type->layout(mutable_C, region);
  }

  if (!wm_xr_temp_region_offscreen_ensure(temp_region, px_width, px_height)) {
    region->winrct = winrct_prev;
    region->winx = winx_prev;
    region->winy = winy_prev;
    return false;
  }

  GPU_offscreen_bind(temp_region->offscreen, false);
  gpu::FrameBuffer *framebuffer = nullptr;
  gpu::Texture *color_texture = nullptr;
  gpu::Texture *depth_texture = nullptr;
  GPU_offscreen_viewport_data_get(
      temp_region->offscreen, &framebuffer, &color_texture, &depth_texture);
  if (framebuffer != nullptr) {
    GPU_framebuffer_viewport_reset(framebuffer);
  }
  GPU_clear_color(0.0f, 0.0f, 0.0f, 0.0f);
  GPU_scissor_test(true);
  GPU_scissor(0, 0, px_width, px_height);
  GPU_matrix_push_projection();
  GPU_matrix_push();
  wmOrtho2_region_pixelspace(region);
  ui::blocklist_update_window_matrix(mutable_C, &region->runtime->uiblocks);
  ui::blocklist_update_view_for_buttons(mutable_C, &region->runtime->uiblocks);
  GPU_matrix_pop();
  GPU_matrix_pop_projection();
  wm_xr_temp_region_draw_direct(mutable_C, panel, region);
  GPU_scissor_test(false);
  GPU_offscreen_unbind(temp_region->offscreen, false);
  if (color_texture != nullptr) {
    GPU_texture_mipmap_mode(color_texture, false, false);
  }
  region->winrct = winrct_prev;
  region->winx = winx_prev;
  region->winy = winy_prev;

  temp_region->valid = true;
  temp_region->z_offset = 1.0f;
  return true;
}

static void wm_xr_temp_region_draw_to_world_quad(const float viewmat[4][4],
                                                 const float winmat[4][4],
                                                 const wmXrPanel *panel,
                                                 const wmXrTempRegion *temp_region)
{
  if (panel == nullptr || temp_region == nullptr || !temp_region->valid ||
      temp_region->region == nullptr || panel->panel_host_region == nullptr)
  {
    return;
  }

  gpu::Texture *color_texture = temp_region->offscreen ?
                                    GPU_offscreen_color_texture(temp_region->offscreen) :
                                    nullptr;
  if (color_texture == nullptr) {
    return;
  }

  const int px_width = BLI_rcti_size_x(&temp_region->region_rect) + 1;
  const int px_height = BLI_rcti_size_y(&temp_region->region_rect) + 1;
  const int offset_x = temp_region->region_rect.xmin - panel->panel_host_region->winrct.xmin;
  const int offset_y = temp_region->region_rect.ymin - panel->panel_host_region->winrct.ymin;

  float obmat[4][4];
  copy_m4_m4(obmat, panel->panel_obmat);
  madd_v3_v3fl(obmat[3], panel->panel_obmat[0], float(offset_x));
  madd_v3_v3fl(obmat[3], panel->panel_obmat[1], float(offset_y));
  madd_v3_v3fl(obmat[3], panel->panel_obmat[2], temp_region->z_offset);

  GPU_color_mask(true, true, true, true);
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_face_culling(GPU_CULL_NONE);
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_depth_mask(false);

  GPU_matrix_push_projection();
  GPU_matrix_projection_set(winmat);
  GPU_matrix_push();
  GPU_matrix_set(viewmat);
  GPU_matrix_mul(obmat);

  float q0[3] = {0.0f, 0.0f, 0.0f};
  float q1[3] = {float(px_width), 0.0f, 0.0f};
  float q2[3] = {float(px_width), float(px_height), 0.0f};
  float q3[3] = {0.0f, float(px_height), 0.0f};

  GPUVertFormat *fmt = immVertexFormat();
  uint a_pos = GPU_vertformat_attr_add(fmt, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  uint a_uv = GPU_vertformat_attr_add(fmt, "texCoord", gpu::VertAttrType::SFLOAT_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
  immBindTexture("image", color_texture);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 1.0f);

  immBegin(GPU_PRIM_TRI_FAN, 4);
  immAttr2f(a_uv, 0.0f, 0.0f);
  immVertex3fv(a_pos, q0);
  immAttr2f(a_uv, 1.0f, 0.0f);
  immVertex3fv(a_pos, q1);
  immAttr2f(a_uv, 1.0f, 1.0f);
  immVertex3fv(a_pos, q2);
  immAttr2f(a_uv, 0.0f, 1.0f);
  immVertex3fv(a_pos, q3);
  immEnd();

  immUnbindProgram();
  GPU_matrix_pop();
  GPU_matrix_pop_projection();
}

static void wm_xr_temp_regions_clear(wmXrPanel *panel)
{
  if (panel == nullptr) {
    return;
  }

  while (wmXrTempRegion *temp_region = static_cast<wmXrTempRegion *>(panel->temporary_regions.first))
  {
    BLI_remlink(&panel->temporary_regions, temp_region);
    if (temp_region->offscreen != nullptr) {
      GPU_offscreen_free(temp_region->offscreen);
    }
    MEM_delete(temp_region);
  }
}

static void wm_xr_panel_free(wmXrSurfaceData *surface_data, wmXrPanel *panel)
{
  if (surface_data == nullptr || panel == nullptr) {
    return;
  }
  wm_xr_temp_regions_clear(panel);
  if (panel->panel_offscreen != nullptr) {
    GPU_offscreen_free(panel->panel_offscreen);
  }
  if (surface_data->active_panel == panel) {
    surface_data->active_panel = nullptr;
  }
  BLI_remlink(&surface_data->panels, panel);
  MEM_delete(panel);
}

bool WM_xr_temp_region_register(ARegion *region, wmWindow *win, ScrArea *area, ARegion *xr_region)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  if (surface_data == nullptr || region == nullptr || win == nullptr || xr_region == nullptr)
  {
    return false;
  }

  if (wm_xr_temp_region_find_any(surface_data, region, nullptr) != nullptr) {
    return true;
  }

  wmXrPanel *panel = wm_xr_panel_find_by_source_region(surface_data, win, area, xr_region);
  if (panel == nullptr) {
    return false;
  }

  wmXrTempRegion *temp_region = MEM_new_zeroed<wmXrTempRegion>(__func__);
  temp_region->region = region;
  BLI_addtail(&panel->temporary_regions, temp_region);
  panel->panel_dirty = true;
  ED_region_tag_redraw(region);
  if (region->runtime != nullptr) {
    region->runtime->do_draw |= RGN_REFRESH_UI;
  }

  return true;
}

void WM_xr_temp_region_unregister(ARegion *region)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  if (surface_data == nullptr || region == nullptr) {
    return;
  }

  wmXrPanel *panel = nullptr;
  wmXrTempRegion *temp_region = wm_xr_temp_region_find_any(surface_data, region, &panel);
  if (temp_region == nullptr || panel == nullptr) {
    return;
  }

  BLI_remlink(&panel->temporary_regions, temp_region);
  panel->panel_dirty = true;
  if (temp_region->offscreen != nullptr) {
    GPU_offscreen_free(temp_region->offscreen);
  }
  MEM_delete(temp_region);
}

bool WM_xr_temp_region_is_registered(const ARegion *region)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  if (surface_data == nullptr || region == nullptr) {
    return false;
  }

  return wm_xr_temp_region_find_any(surface_data, region, nullptr) != nullptr;
}

void WM_xr_temp_region_tag_dirty(ARegion *region)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  if (surface_data == nullptr || region == nullptr) {
    return;
  }

  wmXrPanel *panel = nullptr;
  wmXrTempRegion *temp_region = wm_xr_temp_region_find_any(surface_data, region, &panel);
  if (panel == nullptr || temp_region == nullptr) {
    return;
  }

  panel->panel_dirty = true;
  ED_region_tag_redraw(region);
  if (region->runtime != nullptr) {
    region->runtime->do_draw |= RGN_REFRESH_UI;
  }
}

void WM_xr_surface_panel_mount_set(wmXrData *xr, eWMXrPanelMountPoint mount_point)
{
  if (xr == nullptr || xr->runtime == nullptr) {
    return;
  }
  xr->runtime->panel_mount_point = mount_point;
}

static eWMXrPanelMountPoint wm_xr_panel_mount_point_resolve(const wmXrData *xr, const ARegion *region)
{
  if (xr != nullptr && xr->runtime != nullptr && xr->runtime->panel_mount_point != XR_PANEL_MOUNT_NONE) {
    return xr->runtime->panel_mount_point;
  }
  if (region == nullptr || region->runtime == nullptr || region->runtime->type == nullptr) {
    return XR_PANEL_MOUNT_NONE;
  }

  eWMXrPanelMountPoint mount_point = XR_PANEL_MOUNT_NONE;
  for (PanelType *panel_type : ListBaseWrapper<PanelType>(region->runtime->type->paneltypes)) {
    const eWMXrPanelMountPoint panel_mount = eWMXrPanelMountPoint(panel_type->xr_panel_mount_point);
    if (panel_mount == XR_PANEL_MOUNT_NONE) {
      continue;
    }
    if (mount_point != XR_PANEL_MOUNT_NONE && mount_point != panel_mount) {
      return XR_PANEL_MOUNT_NONE;
    }
    mount_point = panel_mount;
  }

  return mount_point;
}

static eWMXrPanelMountPoint wm_xr_panel_type_mount_point_get(const PanelType *panel_type)
{
  for (const PanelType *current = panel_type; current != nullptr; current = current->parent) {
    if (current->xr_panel_mount_point != XR_PANEL_MOUNT_NONE) {
      return eWMXrPanelMountPoint(current->xr_panel_mount_point);
    }
  }
  return XR_PANEL_MOUNT_NONE;
}

static int wm_xr_panel_mount_points_collect(const wmXrData *xr,
                                            const ARegion *region,
                                            eWMXrPanelMountPoint r_mount_points[4])
{
  if (xr != nullptr && xr->runtime != nullptr && xr->runtime->panel_mount_point != XR_PANEL_MOUNT_NONE) {
    r_mount_points[0] = xr->runtime->panel_mount_point;
    return 1;
  }
  if (region == nullptr || region->runtime == nullptr || region->runtime->type == nullptr) {
    return 0;
  }

  int count = 0;
  for (PanelType *panel_type : ListBaseWrapper<PanelType>(region->runtime->type->paneltypes)) {
    const eWMXrPanelMountPoint mount_point = wm_xr_panel_type_mount_point_get(panel_type);
    if (mount_point == XR_PANEL_MOUNT_NONE) {
      continue;
    }
    if (!ELEM(mount_point, XR_PANEL_MOUNT_LEFT_HAND, XR_PANEL_MOUNT_RIGHT_HAND, XR_PANEL_MOUNT_HEAD_FOLLOW, XR_PANEL_MOUNT_WORLD))
    {
      continue;
    }
    bool already_added = false;
    for (int i = 0; i < count; i++) {
      if (r_mount_points[i] == mount_point) {
        already_added = true;
        break;
      }
    }
    if (!already_added && count < 4) {
      r_mount_points[count++] = mount_point;
    }
  }

  return count;
}

class wmXrPanelTypeFilterScope {
  struct PanelTypeLink {
    PanelTypeLink *next, *prev;
    PanelType *panel_type;
  };

  ARegion *region_;
  ListBase original_paneltypes_ = {nullptr, nullptr};

 public:
  wmXrPanelTypeFilterScope(ARegion *region, eWMXrPanelMountPoint mount_point) : region_(region)
  {
    if (region_ == nullptr || region_->runtime == nullptr || region_->runtime->type == nullptr) {
      return;
    }

    ListBase &paneltypes = region_->runtime->type->paneltypes;
    for (PanelType *panel_type : ListBaseWrapper<PanelType>(paneltypes)) {
      PanelTypeLink *link = MEM_new<PanelTypeLink>(__func__);
      link->panel_type = panel_type;
      BLI_addtail(&original_paneltypes_, link);
    }

    BLI_listbase_clear(&paneltypes);
    for (PanelTypeLink *link : ListBaseWrapper<PanelTypeLink>(original_paneltypes_)) {
      if (wm_xr_panel_type_mount_point_get(link->panel_type) == mount_point) {
        BLI_addtail(&paneltypes, link->panel_type);
      }
    }
  }

  ~wmXrPanelTypeFilterScope()
  {
    if (region_ == nullptr || region_->runtime == nullptr || region_->runtime->type == nullptr) {
      return;
    }

    ListBase &paneltypes = region_->runtime->type->paneltypes;
    BLI_listbase_clear(&paneltypes);
    for (PanelTypeLink *link : ListBaseWrapper<PanelTypeLink>(original_paneltypes_)) {
      BLI_addtail(&paneltypes, link->panel_type);
    }
    BLI_freelistN(&original_paneltypes_);
  }
};

static int wm_xr_region_panel_type_count(const ARegion *region)
{
  if (region == nullptr || region->runtime == nullptr || region->runtime->type == nullptr) {
    return -1;
  }
  return BLI_listbase_count(&region->runtime->type->paneltypes);
}

static int wm_xr_region_panel_instance_count(const ARegion *region)
{
  if (region == nullptr) {
    return -1;
  }
  return BLI_listbase_count(&region->panels);
}

static void wm_xr_trace_panel_texture_sample(GPUOffScreen *offscreen, const rcti *panel_rect)
{
  gpu::FrameBuffer *framebuffer;
  gpu::Texture *color_texture;
  gpu::Texture *depth_texture;
  GPU_offscreen_viewport_data_get(offscreen, &framebuffer, &color_texture, &depth_texture);
  if (color_texture == nullptr) {
    XR_PANELS_TRACE("panels_ws: texture sample skipped offscreen=%p color_texture=null", offscreen);
    return;
  }

  const int px_width = BLI_rcti_size_x(panel_rect) + 1;
  const int px_height = BLI_rcti_size_y(panel_rect) + 1;
  float *rgba = static_cast<float *>(GPU_texture_read(color_texture, GPU_DATA_FLOAT, 0));
  if (rgba == nullptr) {
    XR_PANELS_TRACE("panels_ws: texture sample read failed offscreen=%p tex=%p", offscreen, color_texture);
    return;
  }

  const int center_x = std::max(0, px_width / 2);
  const int center_y = std::max(0, px_height / 2);
  const int corner_idx = 0;
  const int center_idx = ((center_y * px_width) + center_x) * 4;
  XR_PANELS_TRACE(
      "panels_ws: texture sample tex=%p size=%dx%d corner_rgba=(%.3f, %.3f, %.3f, %.3f) center_rgba=(%.3f, %.3f, %.3f, %.3f)",
      color_texture,
      px_width,
      px_height,
      rgba[corner_idx + 0],
      rgba[corner_idx + 1],
      rgba[corner_idx + 2],
      rgba[corner_idx + 3],
      rgba[center_idx + 0],
      rgba[center_idx + 1],
      rgba[center_idx + 2],
      rgba[center_idx + 3]);
  MEM_delete(rgba);
}

static void wm_xr_trace_panel_blocks(const ARegion *region)
{
  if (region == nullptr || region->runtime == nullptr) {
    XR_PANELS_TRACE("panels_ws: block trace skipped region=%p", region);
    return;
  }

  const int block_count = BLI_listbase_count(&region->runtime->uiblocks);
  XR_PANELS_TRACE(
      "panels_ws: uiblocks=%d first_block=%p", block_count, region->runtime->uiblocks.first);
}

static void wm_xr_region_ensure_layout_rect(ARegion *region)
{
  if (region == nullptr || region->runtime == nullptr || region->runtime->type == nullptr) {
    return;
  }
  if (BLI_rcti_size_x(&region->winrct) > 0 && BLI_rcti_size_y(&region->winrct) > 0) {
    return;
  }

  const int width = (region->runtime->type->prefsizex > 0) ? region->runtime->type->prefsizex :
                                                          UI_SIDEBAR_PANEL_WIDTH;
  const int height = (region->sizey > 1) ? region->sizey : 2048;

  BLI_rcti_init(&region->winrct, 0, width - 1, 0, height - 1);
  region->sizex = width;
  region->sizey = height;
  ED_region_update_rect(region);
}

static void wm_xr_panel_default_transform_init(wmXrPanel *panel)
{
  const float half_pi = 3.1415f * 0.5f;
  const float screen_to_world_scale = 1.0f / 1536.0f;
  float pos[3] = {0.0f, 0.0f, 2.0f};
  float rot[3] = {half_pi, 0.0f, 0.0f};
  float size[3] = {screen_to_world_scale, screen_to_world_scale, screen_to_world_scale};
  loc_eul_size_to_mat4(panel->panel_obmat, pos, rot, size);
}

static float wm_xr_panel_scale_get(const wmXrPanel *panel)
{
  if (panel != nullptr && panel->mount_point == XR_PANEL_MOUNT_HEAD_FOLLOW) {
    return 1.0f / 500.0f;
  }
  return 1.0f / 1536.0f;
}

static bool wm_xr_controller_pose_find(const wmXrData *xr,
                                       const char *subaction_path,
                                       const wmXrController **r_controller,
                                       bool *r_use_grip_pose)
{
  if (subaction_path == nullptr || r_controller == nullptr || r_use_grip_pose == nullptr) {
    return false;
  }
  *r_controller = nullptr;
  *r_use_grip_pose = false;

  if (xr == nullptr || xr->runtime == nullptr) {
    return false;
  }

  for (const wmXrController *controller :
       ConstListBaseWrapper<wmXrController>(xr->runtime->session_state.controllers))
  {
    if (STREQ(controller->subaction_path, subaction_path)) {
      if (controller->grip_active) {
        *r_controller = controller;
        *r_use_grip_pose = true;
        return true;
      }
      if (controller->aim_active) {
        *r_controller = controller;
        *r_use_grip_pose = false;
        return true;
      }
    }
  }

  return false;
}

static void wm_xr_panel_basis_from_back(const float back_in[3], float r_right[3], float r_up[3], float r_back[3])
{
  float back[3];
  copy_v3_v3(back, back_in);
  if (normalize_v3(back) == 0.0f) {
    back[0] = 0.0f;
    back[1] = -1.0f;
    back[2] = 0.0f;
  }

  const float world_up[3] = {0.0f, 0.0f, 1.0f};
  cross_v3_v3v3(r_right, back, world_up);
  if (normalize_v3(r_right) == 0.0f) {
    r_right[0] = 1.0f;
    r_right[1] = 0.0f;
    r_right[2] = 0.0f;
  }

  cross_v3_v3v3(r_up, r_right, back);
  normalize_v3(r_up);
  copy_v3_v3(r_back, back);
}

static void wm_xr_panel_transform_apply(wmXrPanel *panel, const float pos[3], const float back[3])
{
  if (panel == nullptr) {
    return;
  }

  const float screen_to_world_scale = wm_xr_panel_scale_get(panel);
  float right[3];
  float up[3];
  float basis_back[3];

  wm_xr_panel_basis_from_back(back, right, up, basis_back);

  unit_m4(panel->panel_obmat);
  copy_v3_v3(panel->panel_obmat[0], right);
  copy_v3_v3(panel->panel_obmat[1], up);
  copy_v3_v3(panel->panel_obmat[2], basis_back);
  mul_v3_fl(panel->panel_obmat[0], screen_to_world_scale);
  mul_v3_fl(panel->panel_obmat[1], screen_to_world_scale);
  mul_v3_fl(panel->panel_obmat[2], screen_to_world_scale);
  copy_v3_v3(panel->panel_obmat[3], pos);

  const int panel_px_width = std::max(BLI_rcti_size_x(&panel->panel_rect) + 1, 1);
  const int panel_px_height = std::max(BLI_rcti_size_y(&panel->panel_rect) + 1, 1);
  madd_v3_v3fl(panel->panel_obmat[3], panel->panel_obmat[0], -0.5f * panel_px_width);
  madd_v3_v3fl(panel->panel_obmat[3], panel->panel_obmat[1], -0.5f * panel_px_height);
}

static void wm_xr_panel_center_position_get(const wmXrPanel *panel, float r_center[3])
{
  if (panel == nullptr) {
    zero_v3(r_center);
    return;
  }

  copy_v3_v3(r_center, panel->panel_obmat[3]);
  const int panel_px_width = std::max(BLI_rcti_size_x(&panel->panel_rect) + 1, 1);
  const int panel_px_height = std::max(BLI_rcti_size_y(&panel->panel_rect) + 1, 1);
  madd_v3_v3fl(r_center, panel->panel_obmat[0], 0.5f * panel_px_width);
  madd_v3_v3fl(r_center, panel->panel_obmat[1], 0.5f * panel_px_height);
}

static bool wm_xr_panel_target_from_viewer(const wmXrData *xr, float r_pos[3], float r_back[3])
{
  if (xr == nullptr || xr->runtime == nullptr || !xr->runtime->session_state.is_view_data_set) {
    return false;
  }

  const GHOST_XrPose *viewer_pose = &xr->runtime->session_state.viewer_pose;
  float viewer_mat[4][4];
  wm_xr_pose_to_mat(viewer_pose, viewer_mat);

  float forward[3];
  normalize_v3_v3(forward, viewer_mat[2]);
  negate_v3(forward);

  float forward_xy[2] = {forward[0], forward[1]};
  if (normalize_v2(forward_xy) == 0.0f) {
    forward_xy[0] = 0.0f;
    forward_xy[1] = -1.0f;
  }

  copy_v3_v3(r_pos, viewer_pose->position);
  r_pos[0] += forward_xy[0] * 1.5f;
  r_pos[1] += forward_xy[1] * 1.5f;
  r_back[0] = forward_xy[0];
  r_back[1] = forward_xy[1];
  r_back[2] = 0.0f;
  return true;
}

static bool wm_xr_panel_target_from_controller(const wmXrData *xr,
                                               const char *subaction_path,
                                               float r_pos[3],
                                               float r_back[3])
{
  const wmXrController *controller = nullptr;
  bool use_grip_pose = false;
  if (!wm_xr_controller_pose_find(xr, subaction_path, &controller, &use_grip_pose)) {
    return false;
  }

  float controller_right[3];
  float controller_up[3];
  float controller_forward[3];
  const float(*controller_mat)[4] = use_grip_pose ? controller->grip_mat : controller->aim_mat;
  const GHOST_XrPose *controller_pose = use_grip_pose ? &controller->grip_pose : &controller->aim_pose;
  normalize_v3_v3(controller_right, controller_mat[0]);
  normalize_v3_v3(controller_up, controller_mat[1]);
  normalize_v3_v3(controller_forward, controller_mat[2]);

  const float side_sign = STREQ(subaction_path, "/user/hand/right") ? 1.0f : -1.0f;
  copy_v3_v3(r_pos, controller_pose->position);
  madd_v3_v3fl(r_pos, controller_right, 0.08f * side_sign);
  madd_v3_v3fl(r_pos, controller_up, 0.05f);
  madd_v3_v3fl(r_pos, controller_forward, -0.04f);

  if (xr != nullptr && xr->runtime != nullptr && xr->runtime->session_state.is_view_data_set) {
    const GHOST_XrPose *viewer_pose = &xr->runtime->session_state.viewer_pose;
    r_back[0] = r_pos[0] - viewer_pose->position[0];
    r_back[1] = r_pos[1] - viewer_pose->position[1];
    r_back[2] = 0.0f;
  }
  else {
    r_back[0] = controller_forward[0];
    r_back[1] = controller_forward[1];
    r_back[2] = 0.0f;
  }

  return true;
}

static bool wm_xr_panel_target_from_head_follow(const wmXrData *xr,
                                                const wmXrPanel *panel,
                                                float r_pos[3],
                                                float r_back[3])
{
  float target_pos[3];
  float target_back[3];
  if (!wm_xr_panel_target_from_viewer(xr, target_pos, target_back)) {
    return false;
  }

  if (panel == nullptr || is_zero_m4(panel->panel_obmat)) {
    copy_v3_v3(r_pos, target_pos);
    copy_v3_v3(r_back, target_back);
    return true;
  }

  float current_back[3];
  if (xr == nullptr || xr->runtime == nullptr || !xr->runtime->session_state.is_view_data_set) {
    copy_v3_v3(r_pos, target_pos);
    copy_v3_v3(r_back, target_back);
    return true;
  }

  const GHOST_XrPose *viewer_pose = &xr->runtime->session_state.viewer_pose;
  float current_center[3];
  wm_xr_panel_center_position_get(panel, current_center);
  sub_v3_v3v3(current_back, current_center, viewer_pose->position);
  current_back[2] = 0.0f;
  if (normalize_v3(current_back) == 0.0f) {
    copy_v3_v3(current_back, target_back);
  }

  const float fixed_distance = 1.0f;
  const float follow_angle_threshold = DEG2RADF(25.0f);
  const float follow_factor = 0.01f;
  const float angle = angle_normalized_v3v3(current_back, target_back);

  if (angle < follow_angle_threshold) {
    madd_v3_v3v3fl(r_pos, viewer_pose->position, current_back, fixed_distance);
    copy_v3_v3(r_back, current_back);
    return true;
  }

  interp_v3_v3v3(r_back, current_back, target_back, follow_factor);
  normalize_v3(r_back);
  madd_v3_v3v3fl(r_pos, viewer_pose->position, r_back, fixed_distance);
  return true;
}

static void wm_xr_panel_mount_update(wmXrPanel *panel, const wmXrData *xr)
{
  if (panel == nullptr) {
    return;
  }

  float pos[3];
  float back[3];
  bool has_target = false;

  switch (panel->mount_point) {
    case XR_PANEL_MOUNT_NONE:
      return;
    case XR_PANEL_MOUNT_LEFT_HAND:
      has_target = wm_xr_panel_target_from_controller(xr, "/user/hand/left", pos, back);
      if (!has_target) {
        has_target = wm_xr_panel_target_from_viewer(xr, pos, back);
      }
      break;
    case XR_PANEL_MOUNT_RIGHT_HAND:
      has_target = wm_xr_panel_target_from_controller(xr, "/user/hand/right", pos, back);
      if (!has_target) {
        has_target = wm_xr_panel_target_from_viewer(xr, pos, back);
      }
      break;
    case XR_PANEL_MOUNT_HEAD_FOLLOW:
      has_target = wm_xr_panel_target_from_head_follow(xr, panel, pos, back);
      break;
    case XR_PANEL_MOUNT_WORLD:
      if (is_zero_m4(panel->panel_obmat)) {
        has_target = wm_xr_panel_target_from_viewer(xr, pos, back);
      }
      else {
        return;
      }
      break;
  }

  if (!has_target) {
    wm_xr_panel_default_transform_init(panel);
    return;
  }

  wm_xr_panel_transform_apply(panel, pos, back);
}

static wmXrPanel *wm_xr_panel_register(wmXrSurfaceData *surface_data,
                                       wmWindow *win,
                                       ScrArea *area,
                                       ARegion *region,
                                       eWMXrPanelMountPoint mount_point,
                                       const wmXrData *xr)
{
  if (mount_point == XR_PANEL_MOUNT_NONE) {
    return nullptr;
  }

  wmXrPanel *panel = wm_xr_panel_find(surface_data, win, area, region, mount_point);
  if (panel != nullptr) {
    panel->mount_point = mount_point;
    panel->panel_host_region = region;
    XR_PANELS_TRACE("panels_ws: reuse host panel area=%p region=%p panel_types=%d panel_instances=%d",
                    area,
                    region,
                    wm_xr_region_panel_type_count(region),
                    wm_xr_region_panel_instance_count(region));
    return panel;
  }

  panel = MEM_new_zeroed<wmXrPanel>(__func__);
  panel->mount_point = mount_point;
  panel->panel_host_win = win;
  panel->panel_host_area = area;
  panel->panel_host_region = region;
  wm_xr_panel_mount_update(panel, xr);
  panel->panel_frame_tag = surface_data->panels_frame_tag;
  BLI_addtail(&surface_data->panels, panel);
  XR_PANELS_TRACE("panels_ws: created panel transform panel=%p loc=(%.3f, %.3f, %.3f)",
                  panel,
                  panel->panel_obmat[3][0],
                  panel->panel_obmat[3][1],
                  panel->panel_obmat[3][2]);
  XR_PANELS_TRACE("panels_ws: register host panel area=%p region=%p panel=%p panel_types=%d total_hosts=%d",
                  area,
                  region,
                  panel,
                  wm_xr_region_panel_type_count(region),
                  BLI_listbase_count(&surface_data->panels));
  return panel;
}

static void wm_xr_panel_pointer_clear(wmXrPanel *panel)
{
  panel->panel_hovered = false;
  panel->panel_hover_region = nullptr;
  panel->panel_cursor_visible = false;
  zero_v2_int(panel->panel_window_xy);
  panel->panel_pointer.pressed = false;
  panel->panel_pointer.subaction_path[0] = '\0';
  panel->panel_pointer.action_idname[0] = '\0';
  panel->panel_pointer.region = nullptr;
}

static void wm_xr_ui_overlay_winmat_create(const float src_winmat[4][4], float r_winmat[4][4])
{
  const float m00 = src_winmat[0][0];
  const float m11 = src_winmat[1][1];
  if (m00 == 0.0f || m11 == 0.0f) {
    copy_m4_m4(r_winmat, src_winmat);
    return;
  }

  const float near_clip = 0.001f;
  const float far_clip = 10000.0f;
  const float right_minus_left = (2.0f * near_clip) / m00;
  const float top_minus_bottom = (2.0f * near_clip) / m11;
  const float right_plus_left = src_winmat[2][0] * right_minus_left;
  const float top_plus_bottom = src_winmat[2][1] * top_minus_bottom;
  const float left = 0.5f * (right_plus_left - right_minus_left);
  const float right = 0.5f * (right_plus_left + right_minus_left);
  const float bottom = 0.5f * (top_plus_bottom - top_minus_bottom);
  const float top = 0.5f * (top_plus_bottom + top_minus_bottom);

  perspective_m4(r_winmat, left, right, bottom, top, near_clip, far_clip);
}

static void wm_xr_panel_cursor_draw_overlay(const float viewmat[4][4],
                                            const float winmat[4][4],
                                            const wmXrSurfaceData *surface_data)
{
  if (surface_data == nullptr) {
    return;
  }

  const float cursor_color[4] = {1.0f, 1.0f, 1.0f, 0.9f};
  gpu::Batch *sphere = GPU_batch_preset_sphere(3);
  GPU_batch_program_set_builtin(sphere, GPU_SHADER_3D_UNIFORM_COLOR);
  GPU_batch_uniform_4fv(sphere, "color", cursor_color);
  float ui_winmat[4][4];
  wm_xr_ui_overlay_winmat_create(winmat, ui_winmat);
  GPU_matrix_push_projection();
  GPU_matrix_projection_set(ui_winmat);
  GPU_matrix_push();
  GPU_matrix_set(viewmat);
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_blend(GPU_BLEND_ALPHA);

  for (const wmXrPanel *panel : ConstListBaseWrapper<wmXrPanel>(surface_data->panels)) {
    if (!panel->panel_cursor_visible) {
      continue;
    }
    GPU_matrix_push();
    GPU_matrix_translate_3fv(panel->panel_cursor_world);
    GPU_matrix_scale_1f(0.0075f);
    GPU_batch_draw(sphere);
    GPU_matrix_pop();
  }

  GPU_matrix_pop();
  GPU_matrix_pop_projection();
}

static void wm_xr_draw_cached_panel_overlay(const float viewmat[4][4],
                                            const float winmat[4][4],
                                            const wmXrSurfaceData *surface_data)
{
  if (surface_data == nullptr) {
    return;
  }

  for (const bool draw_controller_panels : {false, true}) {
    for (const wmXrPanel *panel : ConstListBaseWrapper<wmXrPanel>(surface_data->panels)) {
      if (!panel->panel_valid || panel->panel_offscreen == nullptr) {
        continue;
      }
      const bool is_controller_panel = ELEM(
          panel->mount_point, XR_PANEL_MOUNT_LEFT_HAND, XR_PANEL_MOUNT_RIGHT_HAND);
      if (is_controller_panel != draw_controller_panels) {
        continue;
      }

      RegionView3D rv_tmp = {};
      wm_xr_ui_overlay_winmat_create(winmat, rv_tmp.winmat);
      copy_m4_m4(rv_tmp.viewmat, viewmat);
      ED_region_panels_draw_to_world_quad(
          &rv_tmp, panel->panel_obmat, &panel->panel_rect, panel->panel_offscreen);

      for (const wmXrTempRegion *temp_region :
           ConstListBaseWrapper<wmXrTempRegion>(panel->temporary_regions))
      {
        wm_xr_temp_region_draw_to_world_quad(viewmat, winmat, panel, temp_region);
      }
    }
  }
}

static const wmXrController *wm_xr_surface_interaction_controller_find(const wmXrSessionState *state,
                                                                       char *r_subaction_path)
{
  const wmXrController *fallback = nullptr;
  for (const wmXrController &controller : state->controllers) {
    if (!controller.aim_active) {
      continue;
    }
    if (strstr(controller.subaction_path, "right") != nullptr) {
      BLI_strncpy(r_subaction_path, controller.subaction_path, XR_MAX_USER_PATH_LENGTH);
      return &controller;
    }
    if (fallback == nullptr) {
      fallback = &controller;
    }
  }
  if (fallback != nullptr) {
    BLI_strncpy(r_subaction_path, fallback->subaction_path, XR_MAX_USER_PATH_LENGTH);
  }
  return fallback;
}

static bool wm_xr_surface_action_is_teleport(const wmXrAction *action)
{
  return action != nullptr && action->ot != nullptr && action->ot->idname != nullptr &&
         BLI_strcasestr(action->ot->idname, "xr_navigation_teleport") != nullptr;
}

static bool wm_xr_surface_action_is_panel_click_compatible(const wmXrAction *action)
{
  if (action == nullptr || action->name == nullptr || action->ot == nullptr ||
      action->ot->idname == nullptr)
  {
    return false;
  }
  if (action->type == XR_VECTOR2F_INPUT) {
    return false;
  }
  if (wm_xr_surface_action_is_teleport(action) ||
      BLI_strcasestr(action->name, "teleport") != nullptr)
  {
    return ELEM(action->type, XR_BOOLEAN_INPUT, XR_FLOAT_INPUT);
  }
  if (BLI_strcasestr(action->ot->idname, "xr_navigation_") != nullptr) {
    return false;
  }
  if (BLI_strcasestr(action->name, "trigger") == nullptr &&
      BLI_strcasestr(action->name, "select") == nullptr &&
      BLI_strcasestr(action->name, "click") == nullptr)
  {
    return false;
  }
  return ELEM(action->type, XR_BOOLEAN_INPUT, XR_FLOAT_INPUT);
}

static bool wm_xr_surface_controller_teleport_active(const wmXrData *xr, const char *subaction_path)
{
  if (xr == nullptr || xr->runtime == nullptr || subaction_path == nullptr) {
    return false;
  }

  const wmXrActionSet *active_action_set = xr->runtime->session_state.active_action_set;
  if (active_action_set == nullptr) {
    return false;
  }

  for (LinkData &ld : active_action_set->active_modal_actions) {
    const wmXrAction *action = static_cast<const wmXrAction *>(ld.data);
    if (wm_xr_surface_action_is_teleport(action) && action->active_modal_path != nullptr &&
        STREQ(action->active_modal_path, subaction_path))
    {
      return true;
    }
  }

  return false;
}

static void wm_xr_surface_interaction_ray_from_pose(const GHOST_XrPose *aim_pose,
                                                    float r_origin[3],
                                                    float r_direction[3])
{
  float aim_mat[4][4];
  wm_xr_pose_to_mat(aim_pose, aim_mat);
  copy_v3_v3(r_origin, aim_pose->position);
  normalize_v3_v3(r_direction, aim_mat[2]);
  negate_v3(r_direction);
}

static bool wm_xr_surface_interaction_raycast_rect(const wmXrPanel *panel,
                                                   const float ray_origin[3],
                                                   const float ray_direction[3],
                                                   const rcti &local_rect,
                                                   const float plane_z,
                                                   const bool allow_outside_bounds,
                                                   int r_win_xy[2],
                                                   float r_hit_world[3],
                                                   float *r_lambda)
{
  if (panel == nullptr || !panel->panel_valid || panel->panel_offscreen == nullptr ||
      panel->panel_host_region == nullptr)
  {
    return false;
  }

  float obimat[4][4];
  if (!invert_m4_m4(obimat, panel->panel_obmat)) {
    return false;
  }

  float ray_end[3];
  madd_v3_v3v3fl(ray_end, ray_origin, ray_direction, 100.0f);

  float origin_local[3], end_local[3], dir_local[3];
  copy_v3_v3(origin_local, ray_origin);
  copy_v3_v3(end_local, ray_end);
  mul_m4_v3(obimat, origin_local);
  mul_m4_v3(obimat, end_local);
  sub_v3_v3v3(dir_local, end_local, origin_local);

  if (fabsf(dir_local[2]) < 1e-6f) {
    return false;
  }

  const float lambda = (plane_z - origin_local[2]) / dir_local[2];
  if (lambda < 0.0f) {
    return false;
  }

  float hit_local[3];
  madd_v3_v3v3fl(hit_local, origin_local, dir_local, lambda);

  const float rect_xmin = float(local_rect.xmin);
  const float rect_xmax = float(local_rect.xmax + 1);
  const float rect_ymin = float(local_rect.ymin);
  const float rect_ymax = float(local_rect.ymax + 1);
  if (!allow_outside_bounds &&
      (hit_local[0] < rect_xmin || hit_local[1] < rect_ymin || hit_local[0] > rect_xmax ||
       hit_local[1] > rect_ymax))
  {
    return false;
  }

  r_win_xy[0] = panel->panel_host_region->winrct.xmin + round_fl_to_int(hit_local[0]);
  r_win_xy[1] = panel->panel_host_region->winrct.ymin + round_fl_to_int(hit_local[1]);
  copy_v3_v3(r_hit_world, hit_local);
  r_hit_world[2] = plane_z;
  mul_m4_v3(panel->panel_obmat, r_hit_world);
  if (r_lambda != nullptr) {
    *r_lambda = lambda;
  }
  return true;
}

static bool wm_xr_surface_interaction_raycast_target(const wmXrPanel *panel,
                                                     const wmXrTempRegion *temp_region,
                                                     const float ray_origin[3],
                                                     const float ray_direction[3],
                                                     const bool allow_outside_bounds,
                                                     ARegion **r_region,
                                                     int r_win_xy[2],
                                                     float r_hit_world[3],
                                                     float *r_lambda)
{
  if (panel == nullptr || panel->panel_host_region == nullptr) {
    return false;
  }

  rcti local_rect = panel->panel_rect;
  float plane_z = 0.0f;
  ARegion *target_region = panel->panel_host_region;
  if (temp_region != nullptr) {
    if (!temp_region->valid || temp_region->region == nullptr) {
      return false;
    }
    target_region = temp_region->region;
    local_rect.xmin = temp_region->region_rect.xmin - panel->panel_host_region->winrct.xmin;
    local_rect.xmax = temp_region->region_rect.xmax - panel->panel_host_region->winrct.xmin;
    local_rect.ymin = temp_region->region_rect.ymin - panel->panel_host_region->winrct.ymin;
    local_rect.ymax = temp_region->region_rect.ymax - panel->panel_host_region->winrct.ymin;
    plane_z = temp_region->z_offset;
  }

  if (!wm_xr_surface_interaction_raycast_rect(
          panel, ray_origin, ray_direction, local_rect, plane_z, allow_outside_bounds, r_win_xy,
          r_hit_world, r_lambda))
  {
    return false;
  }

  if (r_region != nullptr) {
    *r_region = target_region;
  }
  return true;
}

static void wm_xr_surface_interaction_event_add(const bContext * /*C*/,
                                                wmWindow *win,
                                                ScrArea *area,
                                                ARegion *region,
                                                short type,
                                                short val,
                                                const int xy[2])
{
  wmEvent event{};
  wm_event_init_from_window(win, &event);
  copy_v2_v2_int(event.xy, xy);
  copy_v2_v2_int(event.prev_xy, win->runtime->eventstate->xy);
  event.type = static_cast<wmEventType>(type);
  event.val = val;

  XR_PANELS_TRACE("panels_ws_input: enqueue type=%d val=%d xy=(%d,%d) prev_xy=(%d,%d) win=%p",
                  int(type),
                  int(val),
                  event.xy[0],
                  event.xy[1],
                  event.prev_xy[0],
                  event.prev_xy[1],
                  win);
  WM_event_add_simulate_region(win, area, region, &event);
}

static bool wm_xr_panel_cache_update(const bContext *C, wmXrPanel *panel)
{
  if (panel != nullptr && panel->panel_valid && panel->panel_offscreen != nullptr && !panel->panel_dirty &&
      panel->panel_last_rebuild_tag == panel->panel_frame_tag)
  {
    return true;
  }

  ScrArea *area = panel ? panel->panel_host_area : nullptr;
  ARegion *xr_region = panel ? panel->panel_host_region : nullptr;
  if (panel == nullptr || area == nullptr || xr_region == nullptr || xr_region->runtime == nullptr ||
      xr_region->runtime->type == nullptr)
  {
    XR_PANELS_TRACE(
        "panels_ws: cache update skipped panel=%p area=%p xr_region=%p", panel, area, xr_region);
    return false;
  }

  XR_PANELS_TRACE("panels_ws: cache update begin panel=%p area=%p xr_region=%p panel_types=%d panel_instances_before=%d",
                  panel,
                  area,
                  xr_region,
                  wm_xr_region_panel_type_count(xr_region),
                  wm_xr_region_panel_instance_count(xr_region));

  bContext *mutable_C = const_cast<bContext *>(C);
  blender::eRegion_Alignment prev_alignment = xr_region->alignment;
  ARegion *prev_region = CTX_wm_region(mutable_C);
  const bool prev_visible = xr_region->runtime->visible;
  rcti panel_rect = panel->panel_rect;
  int w = 0;
  int h = 0;
  const bool needs_layout = !panel->panel_valid ||
                            (panel->panel_last_rebuild_tag != panel->panel_frame_tag) ||
                            (xr_region->runtime->do_draw & RGN_REFRESH_UI) ||
                            BLI_listbase_is_empty(&xr_region->runtime->uiblocks) ||
                            (CTX_wm_manager(C)->xr.runtime->offscreen_area_mount_point != panel->mount_point);
  if (needs_layout) {
    wm_xr_region_ensure_layout_rect(xr_region);
  }
  xr_region->runtime->visible = true;
  if (!CTX_wm_manager(C)->xr.runtime->offscreen_area_initialized) {
    if (xr_region->runtime->type->init != nullptr) {
      xr_region->runtime->type->init(CTX_wm_manager(C), xr_region);
    }
    xr_region->flag |= RGN_FLAG_INDICATE_OVERFLOW;
    CTX_wm_manager(C)->xr.runtime->offscreen_area_initialized = true;
    XR_PANELS_TRACE("panels_ws: initialized xr panel region region=%p", xr_region);
  }
  XR_PANELS_TRACE("panels_ws: ensure layout rect xr_region=%p winrct=(%d,%d)-(%d,%d) size=%dx%d",
                  xr_region,
                  xr_region->winrct.xmin,
                  xr_region->winrct.ymin,
                  xr_region->winrct.xmax,
                  xr_region->winrct.ymax,
                  xr_region->winx,
                  xr_region->winy);
  XR_PANELS_TRACE("panels_ws: xr region visible=%d alignment=%d",
                  int(xr_region->runtime->visible),
                  int(xr_region->alignment));
  CTX_wm_region_set(mutable_C, xr_region);
  xr_region->alignment = RGN_ALIGN_FLOAT;
  {
    wmXrPanelTypeFilterScope panel_type_filter(xr_region, panel->mount_point);
    if (needs_layout) {
      ED_region_panels_exit_active_state(mutable_C, xr_region);
      ui::blocklist_free(mutable_C, xr_region);
      /* Keep the panel list alive across XR relayouts so runtime state like collapse and
       * drag-reorder order persists, just like it does for regular screen regions. */
      ED_region_panels_layout(mutable_C, xr_region);

      int content_width = std::max(1, int(std::ceil(BLI_rctf_size_x(&xr_region->v2d.tot))));
      int content_height = std::max(1, int(std::ceil(BLI_rctf_size_y(&xr_region->v2d.tot))));
      XR_PANELS_TRACE("panels_ws: panel content size=%dx%d v2d_tot=(%.3f, %.3f)-(%.3f, %.3f)",
                      content_width,
                      content_height,
                      xr_region->v2d.tot.xmin,
                      xr_region->v2d.tot.ymin,
                      xr_region->v2d.tot.xmax,
                      xr_region->v2d.tot.ymax);

      if (content_width != xr_region->winx || content_height != xr_region->winy) {
        BLI_rcti_init(&xr_region->winrct, 0, content_width - 1, 0, content_height - 1);
        xr_region->sizex = content_width;
        xr_region->sizey = content_height;
        ED_region_update_rect(xr_region);
        view2d_region_reinit(
            &xr_region->v2d, ui::V2D_COMMONVIEW_PANELS_UI, xr_region->winx, xr_region->winy);
        XR_PANELS_TRACE("panels_ws: resized xr panel region winrct=(%d,%d)-(%d,%d) size=%dx%d",
                        xr_region->winrct.xmin,
                        xr_region->winrct.ymin,
                        xr_region->winrct.xmax,
                        xr_region->winrct.ymax,
                        xr_region->winx,
                        xr_region->winy);
        ED_region_panels_exit_active_state(mutable_C, xr_region);
        ui::blocklist_free(mutable_C, xr_region);
        ED_region_panels_layout(mutable_C, xr_region);
        content_width = std::max(1, int(std::ceil(BLI_rctf_size_x(&xr_region->v2d.tot))));
        content_height = std::max(1, int(std::ceil(BLI_rctf_size_y(&xr_region->v2d.tot))));
      }
      BLI_rcti_init(&panel_rect, 0, content_width - 1, 0, content_height - 1);
      XR_PANELS_TRACE("panels_ws: layout done xr_region=%p rect=(%d,%d)-(%d,%d) panel_instances_after_layout=%d",
                      xr_region,
                      panel_rect.xmin,
                      panel_rect.ymin,
                      panel_rect.xmax,
                      panel_rect.ymax,
                      wm_xr_region_panel_instance_count(xr_region));
      wm_xr_trace_panel_blocks(xr_region);
    }
    else {
      XR_PANELS_TRACE("panels_ws: redraw existing panel rect=(%d,%d)-(%d,%d) panel_instances=%d",
                      panel_rect.xmin,
                      panel_rect.ymin,
                      panel_rect.xmax,
                      panel_rect.ymax,
                      wm_xr_region_panel_instance_count(xr_region));
    }

    w = BLI_rcti_size_x(&panel_rect) + 1;
    h = BLI_rcti_size_y(&panel_rect) + 1;
    XR_PANELS_TRACE("panels_ws: offscreen target size=%dx%d existing=%p", w, h, panel->panel_offscreen);
    bool create_new = true;
    if (panel->panel_offscreen) {
      if (GPU_offscreen_width(panel->panel_offscreen) == w &&
          GPU_offscreen_height(panel->panel_offscreen) == h)
      {
        create_new = false;
      }
      else {
        GPU_offscreen_free(panel->panel_offscreen);
        panel->panel_offscreen = nullptr;
      }
    }
    if (create_new) {
      panel->panel_offscreen = GPU_offscreen_create(
          w, h, false, gpu::TextureFormat::SRGBA_8_8_8_8, GPU_TEXTURE_USAGE_SHADER_READ, false, nullptr);
    }
    if (!panel->panel_offscreen) {
      XR_PANELS_TRACE("panels_ws: offscreen create failed size=%dx%d", w, h);
      CLOG_ERROR(&LOG, "panels_ws: offscreen create failed");
      xr_region->runtime->visible = prev_visible;
      ED_region_panels_world_layout_end(mutable_C, xr_region, prev_alignment, prev_region);
      return false;
    }

    XR_PANELS_TRACE("panels_ws: draw offscreen begin offscreen=%p", panel->panel_offscreen);
    ED_region_panels_draw_offscreen(C, xr_region, &panel_rect, panel->panel_offscreen);
    XR_PANELS_TRACE("panels_ws: draw offscreen end offscreen=%p", panel->panel_offscreen);
    wm_xr_trace_panel_texture_sample(panel->panel_offscreen, &panel_rect);
  }
  panel->panel_rect = panel_rect;
  panel->panel_valid = true;
  panel->panel_dirty = false;
  panel->panel_last_rebuild_tag = panel->panel_frame_tag;
  panel->panel_host_win = CTX_wm_window(C);
  panel->panel_host_area = area;
  panel->panel_host_region = xr_region;
  CTX_wm_manager(C)->xr.runtime->offscreen_area_mount_point = panel->mount_point;
  xr_region->runtime->do_draw &= ~RGN_REFRESH_UI;

  xr_region->runtime->visible = prev_visible;
  XR_PANELS_TRACE("panels_ws: cache update end panel=%p rect=(%d,%d)-(%d,%d) size=%dx%d panel_instances_after=%d",
                  panel,
                  panel_rect.xmin,
                  panel_rect.ymin,
                  panel_rect.xmax,
                  panel_rect.ymax,
                  w,
                  h,
                  wm_xr_region_panel_instance_count(xr_region));

  ED_region_panels_world_layout_end(mutable_C, xr_region, prev_alignment, prev_region);
  return true;
}

static void wm_xr_panel_cache_refresh_host(const bContext *C, wmXrPanel *panel)
{
  if (C == nullptr || panel == nullptr || panel->panel_host_win == nullptr ||
      panel->panel_host_area == nullptr || panel->panel_host_region == nullptr)
  {
    return;
  }

  bContext *mutable_C = const_cast<bContext *>(C);
  wmXrPanelHostContextOverride context_override(mutable_C, panel);
  wm_xr_panel_cache_update(mutable_C, panel);
  ED_region_tag_redraw(panel->panel_host_region);
}

void WM_xr_surface_panels_register(const bContext *C)
{
  if (C == nullptr) {
    return;
  }

  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  wmWindow *win = CTX_wm_window(C);
  const wmWindowManager *wm = CTX_wm_manager(C);
  const wmXrData *xr = wm ? &wm->xr : nullptr;
  if (surface_data == nullptr || win == nullptr || xr == nullptr || xr->runtime == nullptr) {
    XR_PANELS_TRACE("panels_ws: register skipped surface=%p win=%p xr=%p",
                    surface_data,
                    win,
                    xr);
    return;
  }

  ScrArea *area = CTX_wm_area(C);
  ARegion *xr_region = area ? BKE_area_find_region_type(area, RGN_TYPE_XR) : nullptr;
  eWMXrPanelMountPoint mount_points[4];
  const int mount_count = wm_xr_panel_mount_points_collect(xr, xr_region, mount_points);
  if (area == nullptr || xr_region == nullptr || mount_count == 0) {
    return;
  }
  for (int i = 0; i < mount_count; i++) {
    wm_xr_panel_register(surface_data, win, area, xr_region, mount_points[i], xr);
  }
}

void WM_xr_surface_panels_update(const bContext *C, const wmXrData *xr)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  wmWindow *win = C ? CTX_wm_window(C) : nullptr;
  if (surface_data == nullptr || xr == nullptr || xr->runtime == nullptr || win == nullptr) {
    return;
  }

  ScrArea *area = C ? CTX_wm_area(C) : nullptr;
  ARegion *xr_region = area ? BKE_area_find_region_type(area, RGN_TYPE_XR) : nullptr;
  eWMXrPanelMountPoint mount_points[4];
  const int mount_count = wm_xr_panel_mount_points_collect(xr, xr_region, mount_points);
  if (area != nullptr && xr_region != nullptr) {
    for (int i = 0; i < mount_count; i++) {
      wm_xr_panel_register(surface_data, win, area, xr_region, mount_points[i], xr);
    }
  }

  wmXrPanel *panel = static_cast<wmXrPanel *>(surface_data->panels.first);
  while (panel != nullptr) {
    wmXrPanel *panel_next = panel->next;
    bool mount_found = false;
    for (int i = 0; i < mount_count; i++) {
      if (panel->mount_point == mount_points[i]) {
        mount_found = true;
        break;
      }
    }
    if (panel->panel_host_win != win || panel->panel_host_area != area || panel->panel_host_region != xr_region ||
        !mount_found)
    {
      wm_xr_panel_free(surface_data, panel);
      panel = panel_next;
      continue;
    }
    wm_xr_panel_mount_update(panel, xr);
    panel = panel_next;
  }
}

void wm_xr_surface_interaction_update(const bContext *C, wmXrData *xr)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  if (C == nullptr || xr == nullptr || surface_data == nullptr)
  {
    return;
  }

  char subaction_path[64] = "";
  const wmXrController *controller = wm_xr_surface_interaction_controller_find(
      &xr->runtime->session_state, subaction_path);
  if (controller == nullptr) {
    if (surface_data->active_panel != nullptr && !surface_data->active_panel->panel_pointer.pressed) {
      wm_xr_panel_pointer_clear(surface_data->active_panel);
      surface_data->active_panel = nullptr;
    }
    return;
  }

  const bool is_captured_panel_drag = surface_data->active_panel != nullptr &&
                                      surface_data->active_panel->panel_pointer.pressed &&
                                      STREQ(surface_data->active_panel->panel_pointer.subaction_path,
                                            subaction_path);
  if (!is_captured_panel_drag && wm_xr_surface_controller_teleport_active(xr, subaction_path)) {
    if (surface_data->active_panel != nullptr && !surface_data->active_panel->panel_pointer.pressed) {
      wm_xr_panel_pointer_clear(surface_data->active_panel);
      surface_data->active_panel = nullptr;
    }
    return;
  }

  float ray_origin[3], ray_direction[3];
  wm_xr_surface_interaction_ray_from_pose(&controller->aim_pose, ray_origin, ray_direction);
  wmXrPanel *hit_panel = nullptr;
  ARegion *hit_region = nullptr;
  int hit_win_xy[2] = {0, 0};
  float hit_world[3] = {0.0f, 0.0f, 0.0f};
  float hit_lambda = FLT_MAX;

  if (is_captured_panel_drag) {
    hit_panel = surface_data->active_panel;
    ARegion *capture_region = hit_panel->panel_pointer.region ? hit_panel->panel_pointer.region :
                                                             hit_panel->panel_host_region;
    wmXrTempRegion *capture_temp_region = (capture_region == hit_panel->panel_host_region) ?
                                              nullptr :
                                              wm_xr_temp_region_find(hit_panel, capture_region);
    if (!wm_xr_surface_interaction_raycast_target(hit_panel,
                                                  capture_temp_region,
                                                  ray_origin,
                                                  ray_direction,
                                                  true,
                                                  &hit_region,
                                                  hit_win_xy,
                                                  hit_world,
                                                  &hit_lambda))
    {
      return;
    }
  }
  else {
    for (wmXrPanel *panel : ListBaseWrapper<wmXrPanel>(surface_data->panels)) {
      for (wmXrTempRegion *temp_region : ListBaseWrapper<wmXrTempRegion>(panel->temporary_regions)) {
        ARegion *region = nullptr;
        int win_xy[2];
        float panel_hit_world[3];
        float lambda;
        if (!wm_xr_surface_interaction_raycast_target(panel,
                                                      temp_region,
                                                      ray_origin,
                                                      ray_direction,
                                                      false,
                                                      &region,
                                                      win_xy,
                                                      panel_hit_world,
                                                      &lambda))
        {
          continue;
        }
        if (lambda < hit_lambda) {
          hit_panel = panel;
          hit_region = region;
          hit_lambda = lambda;
          copy_v2_v2_int(hit_win_xy, win_xy);
          copy_v3_v3(hit_world, panel_hit_world);
        }
      }

      ARegion *region = nullptr;
      int win_xy[2];
      float panel_hit_world[3];
      float lambda;
      if (wm_xr_surface_interaction_raycast_target(
              panel, nullptr, ray_origin, ray_direction, false, &region, win_xy, panel_hit_world, &lambda))
      {
        if (lambda < hit_lambda) {
          hit_panel = panel;
          hit_region = region;
          hit_lambda = lambda;
          copy_v2_v2_int(hit_win_xy, win_xy);
          copy_v3_v3(hit_world, panel_hit_world);
        }
      }
    }
  }

  if (hit_panel == nullptr || hit_region == nullptr) {
    if (surface_data->active_panel != nullptr && !surface_data->active_panel->panel_pointer.pressed) {
      wm_xr_panel_pointer_clear(surface_data->active_panel);
      surface_data->active_panel = nullptr;
    }
    return;
  }

  hit_panel->panel_cursor_visible = true;
  copy_v3_v3(hit_panel->panel_cursor_world, hit_world);

  if (surface_data->active_panel != nullptr && surface_data->active_panel != hit_panel &&
      !surface_data->active_panel->panel_pointer.pressed)
  {
    wm_xr_panel_pointer_clear(surface_data->active_panel);
  }
  surface_data->active_panel = hit_panel;

  if (!hit_panel->panel_hovered || hit_panel->panel_hover_region != hit_region ||
      hit_panel->panel_window_xy[0] != hit_win_xy[0] || hit_panel->panel_window_xy[1] != hit_win_xy[1] ||
      !STREQ(hit_panel->panel_pointer.subaction_path, subaction_path))
  {
    wm_xr_panel_cache_refresh_host(C, hit_panel);
    wm_xr_surface_interaction_event_add(C,
                                        hit_panel->panel_host_win,
                                        hit_panel->panel_host_area,
                                        hit_region,
                                        MOUSEMOVE,
                                        KM_NOTHING,
                                        hit_win_xy);
    ED_region_tag_redraw(hit_region);
    hit_panel->panel_dirty = true;
  }

  hit_panel->panel_hover_region = hit_region;
  copy_v2_v2_int(hit_panel->panel_window_xy, hit_win_xy);
  hit_panel->panel_region_xy[0] = hit_win_xy[0] - hit_region->winrct.xmin;
  hit_panel->panel_region_xy[1] = hit_win_xy[1] - hit_region->winrct.ymin;
  hit_panel->panel_hovered = true;
  BLI_strncpy(hit_panel->panel_pointer.subaction_path, subaction_path, XR_MAX_USER_PATH_LENGTH);
}

bool wm_xr_surface_interaction_apply_action(const bContext *C,
                                            wmXrData *xr,
                                            const wmXrAction *action,
                                            const char *subaction_path,
                                            short event_val)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  wmXrPanel *panel = surface_data ? surface_data->active_panel : nullptr;
  if (C == nullptr || xr == nullptr || action == nullptr || subaction_path == nullptr ||
      surface_data == nullptr || panel == nullptr || panel->panel_host_win == nullptr)
  {
    return false;
  }

  if (!ELEM(event_val, KM_PRESS, KM_RELEASE)) {
    return false;
  }

  /* A press that begins over the panel may start capture. After that, only the matching release
   * for that captured action/subaction is rerouted to the panel. */
  if (event_val == KM_PRESS) {
    const bool action_is_panel_click = wm_xr_surface_action_is_panel_click_compatible(action);
    XR_PANELS_TRACE(
        "panels_ws_input: action press hovered=%d pressed=%d subaction_match=%d "
        "action_name=%s action_type=%d action_op=%s panel_click=%d "
        "host_area=%p host_region=%p host_region_type=%d offscreen_area=%p",
        int(panel->panel_hovered),
        int(panel->panel_pointer.pressed),
        int(STREQ(panel->panel_pointer.subaction_path, subaction_path)),
        action->name ? action->name : "<null>",
        int(action->type),
        (action->ot && action->ot->idname) ? action->ot->idname : "<null>",
        int(action_is_panel_click),
        panel->panel_host_area,
        panel->panel_host_region,
        panel->panel_host_region ? int(panel->panel_host_region->regiontype) : -1,
        xr->runtime ? xr->runtime->offscreen_area : nullptr);
    if (panel->panel_pointer.pressed && action->ot != nullptr &&
        STREQ(panel->panel_pointer.subaction_path, subaction_path) &&
        STREQ(panel->panel_pointer.action_idname, action->ot->idname))
    {
      /* Keep consuming held press events for an active panel drag so the panel interaction keeps
       * ownership of this controller until release. */
      return true;
    }
    if (!action_is_panel_click) {
      return false;
    }
    if (!panel->panel_hovered || panel->panel_hover_region == nullptr) {
      return false;
    }
    ARegion *target_region = panel->panel_hover_region;
    wm_xr_panel_cache_refresh_host(C, panel);
    wm_xr_surface_interaction_event_add(C,
                                        panel->panel_host_win,
                                        panel->panel_host_area,
                                        target_region,
                                        LEFTMOUSE,
                                        KM_PRESS,
                                        panel->panel_window_xy);
    panel->panel_pointer.pressed = true;
    panel->panel_pointer.region = target_region;
    BLI_strncpy(
        panel->panel_pointer.subaction_path, subaction_path, XR_MAX_USER_PATH_LENGTH);
    BLI_strncpy(
        panel->panel_pointer.action_idname,
        action->ot->idname,
        sizeof(panel->panel_pointer.action_idname));
    ED_region_tag_redraw(panel->panel_host_region);
    panel->panel_dirty = true;
    return true;
  }

  if (event_val == KM_RELEASE && panel->panel_pointer.pressed &&
      STREQ(panel->panel_pointer.subaction_path, subaction_path) && action->ot != nullptr &&
      STREQ(panel->panel_pointer.action_idname, action->ot->idname))
  {
    ARegion *target_region = panel->panel_pointer.region ? panel->panel_pointer.region :
                                                           panel->panel_hover_region;
    if (target_region == nullptr) {
      return false;
    }
    XR_PANELS_TRACE(
        "panels_ws_input: action release action_name=%s action_type=%d action_op=%s "
        "host_area=%p host_region=%p host_region_type=%d offscreen_area=%p",
        action->name ? action->name : "<null>",
        int(action->type),
        (action->ot && action->ot->idname) ? action->ot->idname : "<null>",
        panel->panel_host_area,
        target_region,
        int(target_region->regiontype),
        xr->runtime ? xr->runtime->offscreen_area : nullptr);
    wm_xr_panel_cache_refresh_host(C, panel);
    wm_xr_surface_interaction_event_add(C,
                                        panel->panel_host_win,
                                        panel->panel_host_area,
                                        target_region,
                                        LEFTMOUSE,
                                        KM_RELEASE,
                                        panel->panel_window_xy);
    wm_xr_panel_pointer_clear(panel);
    /* Keep the active XR panel alive through release handling so any popup opened by the
     * dispatched mouse-release event can still inherit XR ownership. Hover updates clear it
     * naturally once the controller leaves the panel. */
    ED_region_tag_redraw(panel->panel_host_region);
    panel->panel_frame_tag++;
    panel->panel_dirty = true;
    return true;
  }

  return false;
}

void wm_xr_pose_to_mat(const GHOST_XrPose *pose, float r_mat[4][4])
{
  quat_to_mat4(r_mat, pose->orientation_quat);
  copy_v3_v3(r_mat[3], pose->position);
}

void wm_xr_pose_scale_to_mat(const GHOST_XrPose *pose, float scale, float r_mat[4][4])
{
  wm_xr_pose_to_mat(pose, r_mat);

  BLI_assert(scale > 0.0f);
  mul_v3_fl(r_mat[0], scale);
  mul_v3_fl(r_mat[1], scale);
  mul_v3_fl(r_mat[2], scale);
}

void wm_xr_pose_to_imat(const GHOST_XrPose *pose, float r_imat[4][4])
{
  float iquat[4];
  invert_qt_qt_normalized(iquat, pose->orientation_quat);
  quat_to_mat4(r_imat, iquat);
  translate_m4(r_imat, -pose->position[0], -pose->position[1], -pose->position[2]);
}

void wm_xr_pose_scale_to_imat(const GHOST_XrPose *pose, float scale, float r_imat[4][4])
{
  float iquat[4];
  invert_qt_qt_normalized(iquat, pose->orientation_quat);
  quat_to_mat4(r_imat, iquat);

  BLI_assert(scale > 0.0f);
  scale = 1.0f / scale;
  mul_v3_fl(r_imat[0], scale);
  mul_v3_fl(r_imat[1], scale);
  mul_v3_fl(r_imat[2], scale);

  translate_m4(r_imat, -pose->position[0], -pose->position[1], -pose->position[2]);
}

static void wm_xr_draw_matrices_create(const wmXrDrawData *draw_data,
                                       const GHOST_XrDrawViewInfo *draw_view,
                                       const XrSessionSettings *session_settings,
                                       const wmXrSessionState *session_state,
                                       float r_viewmat[4][4],
                                       float r_projmat[4][4])
{
  GHOST_XrPose eye_pose;
  float eye_inv[4][4], base_inv[4][4], nav_inv[4][4], m[4][4];

  /* Calculate inverse eye matrix. */
  copy_qt_qt(eye_pose.orientation_quat, draw_view->eye_pose.orientation_quat);
  copy_v3_v3(eye_pose.position, draw_view->eye_pose.position);
  if ((session_settings->flag & XR_SESSION_USE_POSITION_TRACKING) == 0) {
    sub_v3_v3(eye_pose.position, draw_view->local_pose.position);
  }
  if ((session_settings->flag & XR_SESSION_USE_ABSOLUTE_TRACKING) == 0) {
    sub_v3_v3(eye_pose.position, draw_data->eye_position_ofs);
  }

  wm_xr_pose_to_imat(&eye_pose, eye_inv);

  /* Apply base pose and navigation. */
  wm_xr_pose_scale_to_imat(&draw_data->base_pose, draw_data->base_scale, base_inv);
  wm_xr_pose_scale_to_imat(&session_state->nav_pose_last_actions_sync,
                           session_state->viewer_scale_last_actions_sync,
                           nav_inv);
  mul_m4_m4m4(m, eye_inv, base_inv);
  mul_m4_m4m4(r_viewmat, m, nav_inv);

  perspective_m4_fov(r_projmat,
                     draw_view->fov.angle_left,
                     draw_view->fov.angle_right,
                     draw_view->fov.angle_up,
                     draw_view->fov.angle_down,
                     session_settings->clip_start,
                     session_settings->clip_end);
}

static void wm_xr_draw_viewport_buffers_to_active_framebuffer(
    const wmXrRuntimeData *runtime_data,
    const wmXrSurfaceData *surface_data,
    const GHOST_XrDrawViewInfo *draw_view)
{
  const wmXrViewportPair *vp = static_cast<const wmXrViewportPair *>(
      BLI_findlink(&surface_data->viewports, draw_view->view_idx));
  BLI_assert(vp && vp->viewport);

  const bool is_upside_down = GHOST_XrSessionNeedsUpsideDownDrawing(runtime_data->ghost_context);
  rcti rect{};
  rect.xmin = 0;
  rect.ymin = 0;
  rect.xmax = draw_view->width - 1;
  rect.ymax = draw_view->height - 1;

  wmViewport(&rect);

  /* For upside down contexts, draw with inverted y-values. */
  if (is_upside_down) {
    std::swap(rect.ymin, rect.ymax);
  }
  GPU_viewport_draw_to_screen_ex(vp->viewport, 0, &rect, draw_view->expects_srgb_buffer, true);
}

void wm_xr_draw_view(const GHOST_XrDrawViewInfo *draw_view, void *customdata)
{
  wmXrDrawData *draw_data = static_cast<wmXrDrawData *>(customdata);
  wmXrData *xr_data = draw_data->xr_data;
  wmXrSurfaceData *surface_data = draw_data->surface_data;
  wmXrSessionState *session_state = &xr_data->runtime->session_state;
  XrSessionSettings *settings = &xr_data->session_settings;

  const int display_flags = V3D_OFSDRAW_OVERRIDE_SCENE_SETTINGS | settings->draw_flags;

  float viewmat[4][4], winmat[4][4];

  BLI_assert(WM_xr_session_is_ready(xr_data));

  wm_xr_session_draw_data_update(session_state, settings, draw_view, draw_data);
  wm_xr_draw_matrices_create(draw_data, draw_view, settings, session_state, viewmat, winmat);
  wm_xr_session_state_update(settings, draw_data, draw_view, session_state);

  if (!wm_xr_session_surface_offscreen_ensure(surface_data, draw_view)) {
    return;
  }

  const wmXrViewportPair *vp = static_cast<const wmXrViewportPair *>(
      BLI_findlink(&surface_data->viewports, draw_view->view_idx));
  BLI_assert(vp && vp->offscreen && vp->viewport);

  /* In case a framebuffer is still bound from drawing the last eye. */
  GPU_framebuffer_restore();
  /* Some systems have drawing glitches without this. */
  GPU_clear_depth(1.0f);

  /* XR context is ensured before each draw in #wm_xr_session_surface_draw. */
  bContext *xr_context = WM_xr_session_context_get(xr_data);
  Scene *scene = CTX_data_scene(xr_context);

  /* The XR context depsgraph is separately evaluated outside of drawing within the XR surface
   * #do_depsgraph callback. Thus, obtain the depsgraph directly without evaluating it. */
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(xr_context);

  if (draw_view->view_idx == 0) {
    /* Only render location scouting viewfinder on first eye draw. */
    wm_xr_viewfinder_render_view(xr_data);
  }

  /* Draws the view into the surface_data->viewport's frame-buffers. */
  ED_view3d_draw_offscreen_simple(depsgraph,
                                  scene,
                                  &settings->shading,
                                  xr_context,
                                  eDrawType(settings->shading.type),
                                  settings->object_type_exclude_viewport,
                                  settings->object_type_exclude_select,
                                  draw_view->width,
                                  draw_view->height,
                                  display_flags,
                                  viewmat,
                                  winmat,
                                  settings->clip_start,
                                  settings->clip_end,
                                  session_state->vignette_aperture,
                                  true,
                                  false,
                                  true,
                                  nullptr,
                                  false,
                                  nullptr,
                                  vp->offscreen,
                                  vp->viewport);

  /* The draw-manager uses both GPUOffscreen and GPUViewport to manage frame and texture buffers. A
   * call to GPU_viewport_draw_to_screen() is still needed to get the final result from the
   * viewport buffers composited together and potentially color managed for display on screen.
   * It needs a bound frame-buffer to draw into, for which we simply reuse the GPUOffscreen one.
   *
   * In a next step, Ghost-XR will use the currently bound frame-buffer to retrieve the image
   * to be submitted to the OpenXR swap-chain. So do not un-bind the off-screen yet! */

  GPU_offscreen_bind(vp->offscreen, false);

  wm_xr_draw_viewport_buffers_to_active_framebuffer(xr_data->runtime, surface_data, draw_view);

  if ((settings->draw_flags & V3D_OFSDRAW_XR_SHOW_CUSTOM_OVERLAYS) != 0) {
    wm_xr_draw_cached_panel_overlay(viewmat, winmat, surface_data);
    wm_xr_panel_cursor_draw_overlay(viewmat, winmat, surface_data);
  }

  GPU_matrix_push_projection();
  GPU_matrix_projection_set(winmat);
  GPU_matrix_push();
  GPU_matrix_set(viewmat);
  wm_xr_draw_controllers(nullptr, nullptr, xr_data);
  GPU_matrix_pop();
  GPU_matrix_pop_projection();
}

bool wm_xr_passthrough_enabled(void *customdata)
{
  wmXrDrawData *draw_data = static_cast<wmXrDrawData *>(customdata);
  wmXrData *xr_data = draw_data->xr_data;
  XrSessionSettings *settings = &xr_data->session_settings;

  return (settings->draw_flags & V3D_OFSDRAW_XR_SHOW_PASSTHROUGH) != 0;
}

void wm_xr_disable_passthrough(void *customdata)
{
  wmXrDrawData *draw_data = static_cast<wmXrDrawData *>(customdata);
  wmXrData *xr_data = draw_data->xr_data;
  XrSessionSettings *settings = &xr_data->session_settings;

  settings->draw_flags &= ~V3D_OFSDRAW_XR_SHOW_PASSTHROUGH;
  WM_global_report(RPT_INFO, "Passthrough not available");
}

static gpu::Batch *wm_xr_controller_model_batch_create(GHOST_IXrContext *xr_context,
                                                       const char *subaction_path)
{
  GHOST_XrControllerModelData model_data;

  if (!GHOST_XrGetControllerModelData(xr_context, subaction_path, &model_data) ||
      model_data.count_vertices < 1)
  {
    return nullptr;
  }

  GPUVertFormat format = {0};
  GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  GPU_vertformat_attr_add(&format, "nor", gpu::VertAttrType::SFLOAT_32_32_32);

  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, model_data.count_vertices);
  vbo->data<GHOST_XrControllerModelVertex>().copy_from(
      {model_data.vertices, model_data.count_vertices});

  gpu::IndexBuf *ibo = nullptr;
  if (model_data.count_indices > 0 && ((model_data.count_indices % 3) == 0)) {
    GPUIndexBufBuilder ibo_builder;
    const uint prim_len = model_data.count_indices / 3;
    GPU_indexbuf_init(&ibo_builder, GPU_PRIM_TRIS, prim_len, model_data.count_vertices);
    for (uint i = 0; i < prim_len; ++i) {
      const uint32_t *idx = &model_data.indices[i * 3];
      GPU_indexbuf_add_tri_verts(&ibo_builder, idx[0], idx[1], idx[2]);
    }
    ibo = GPU_indexbuf_build(&ibo_builder);
  }

  return GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, ibo, GPU_BATCH_OWNS_VBO | GPU_BATCH_OWNS_INDEX);
}

static void wm_xr_controller_model_draw(const XrSessionSettings *settings,
                                        GHOST_IXrContext *xr_context,
                                        wmXrSessionState *state)
{
  GHOST_XrControllerModelData model_data;

  float color[4];
  switch (settings->controller_draw_style) {
    case XR_CONTROLLER_DRAW_DARK:
    case XR_CONTROLLER_DRAW_DARK_RAY:
      color[0] = color[1] = color[2] = 0.0f;
      color[3] = 0.4f;
      break;
    case XR_CONTROLLER_DRAW_LIGHT:
    case XR_CONTROLLER_DRAW_LIGHT_RAY:
      color[0] = 0.422f;
      color[1] = 0.438f;
      color[2] = 0.446f;
      color[3] = 0.4f;
      break;
  }

  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_blend(GPU_BLEND_ALPHA);

  for (wmXrController &controller : state->controllers) {
    if (!controller.grip_active) {
      continue;
    }

    gpu::Batch *model = controller.model;
    if (!model) {
      model = controller.model = wm_xr_controller_model_batch_create(xr_context,
                                                                     controller.subaction_path);
    }

    if (model &&
        GHOST_XrGetControllerModelData(xr_context, controller.subaction_path, &model_data) &&
        model_data.count_components > 0)
    {
      GPU_batch_program_set_builtin(model, GPU_SHADER_3D_UNIFORM_COLOR);
      GPU_batch_uniform_4fv(model, "color", color);

      GPU_matrix_push();
      GPU_matrix_mul(controller.grip_mat);
      for (uint component_idx = 0; component_idx < model_data.count_components; ++component_idx) {
        const GHOST_XrControllerModelComponent *component = &model_data.components[component_idx];
        GPU_matrix_push();
        GPU_matrix_mul(component->transform);
        GPU_batch_draw_range(model,
                             model->elem ? component->index_offset : component->vertex_offset,
                             model->elem ? component->index_count : component->vertex_count);
        GPU_matrix_pop();
      }
      GPU_matrix_pop();
    }
    else {
      /* Fallback. */
      const float scale = 0.05f;
      gpu::Batch *sphere = GPU_batch_preset_sphere(2);
      GPU_batch_program_set_builtin(sphere, GPU_SHADER_3D_UNIFORM_COLOR);
      GPU_batch_uniform_4fv(sphere, "color", color);

      GPU_matrix_push();
      GPU_matrix_mul(controller.grip_mat);
      GPU_matrix_scale_1f(scale);
      GPU_batch_draw(sphere);
      GPU_matrix_pop();
    }
  }
}

static void wm_xr_controller_aim_draw(const XrSessionSettings *settings, wmXrSessionState *state)
{
  const bool draw_ray = ELEM(
      settings->controller_draw_style, XR_CONTROLLER_DRAW_DARK_RAY, XR_CONTROLLER_DRAW_LIGHT_RAY);

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  uint col = GPU_vertformat_attr_add(format, "color", gpu::VertAttrType::SFLOAT_32_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_POLYLINE_FLAT_COLOR);

  float viewport[4];
  GPU_viewport_size_get_f(viewport);
  immUniform2fv("viewportSize", &viewport[2]);

  immUniform1f("lineWidth", 3.0f * U.pixelsize);

  if (draw_ray) {
    const float color[4] = {0.33f, 0.33f, 1.0f, 0.5f};
    const float scale = settings->clip_end;
    float ray[3];

    GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
    GPU_blend(GPU_BLEND_ALPHA);

    for (wmXrController &controller : state->controllers) {
      if (!controller.grip_active) {
        continue;
      }

      immBegin(GPU_PRIM_LINES, 2);

      const float (*mat)[4] = controller.aim_mat;
      madd_v3_v3v3fl(ray, mat[3], mat[2], -scale);

      immAttrSkip(col);
      immVertex3fv(pos, mat[3]);
      immAttr4fv(col, color);
      immVertex3fv(pos, ray);

      immEnd();
    }
  }
  else {
    const float r[4] = {255 / 255.0f, 51 / 255.0f, 82 / 255.0f, 255 / 255.0f};
    const float g[4] = {139 / 255.0f, 220 / 255.0f, 0 / 255.0f, 255 / 255.0f};
    const float b[4] = {40 / 255.0f, 144 / 255.0f, 255 / 255.0f, 255 / 255.0f};
    const float scale = 0.01f;
    float x_axis[3], y_axis[3], z_axis[3];

    GPU_depth_test(GPU_DEPTH_NONE);
    GPU_blend(GPU_BLEND_NONE);

    for (wmXrController &controller : state->controllers) {
      if (!controller.grip_active) {
        continue;
      }

      immBegin(GPU_PRIM_LINES, 6);

      const float (*mat)[4] = controller.aim_mat;
      madd_v3_v3v3fl(x_axis, mat[3], mat[0], scale);
      madd_v3_v3v3fl(y_axis, mat[3], mat[1], scale);
      madd_v3_v3v3fl(z_axis, mat[3], mat[2], scale);

      immAttrSkip(col);
      immVertex3fv(pos, mat[3]);
      immAttr4fv(col, r);
      immVertex3fv(pos, x_axis);

      immAttrSkip(col);
      immVertex3fv(pos, mat[3]);
      immAttr4fv(col, g);
      immVertex3fv(pos, y_axis);

      immAttrSkip(col);
      immVertex3fv(pos, mat[3]);
      immAttr4fv(col, b);
      immVertex3fv(pos, z_axis);

      immEnd();
    }
  }

  immUnbindProgram();
}

void wm_xr_draw_controllers(const bContext *C, ARegion * /*region*/, void *customdata)
{
  wmXrData *xr = static_cast<wmXrData *>(customdata);
  const XrSessionSettings *settings = &xr->session_settings;
  GHOST_IXrContext *xr_context = xr->runtime->ghost_context;
  wmXrSessionState *state = &xr->runtime->session_state;

  wm_xr_controller_model_draw(settings, xr_context, state);
  wm_xr_controller_aim_draw(settings, state);
  wm_xr_viewfinder_draw(C, settings, state);
}

static CLG_LogRef LOG = {"xr"};

void wm_xr_draw_panels_world_space(const bContext *C, ARegion * /*region*/, void *customdata)
{
  if (C == nullptr) {
    return;
  }
  BLI_assert(customdata != nullptr);

  wmXrData *xr = static_cast<wmXrData *>(customdata);
  const XrSessionSettings *settings = &xr->session_settings;
  if ((settings->draw_flags & V3D_OFSDRAW_XR_SHOW_CUSTOM_OVERLAYS) == 0) {
    return;
  }

  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  if (!surface_data) {
    return;
  }

  ScrArea *area = CTX_wm_area(C);
  ARegion *xr_region = area ? BKE_area_find_region_type(area, RGN_TYPE_XR) : nullptr;
  if (area == nullptr || xr_region == nullptr) {
    XR_PANELS_TRACE("panels_ws: draw skipped area=%p xr_region=%p", area, xr_region);
    return;
  }

  XR_PANELS_TRACE("panels_ws: draw host area=%p xr_region=%p panel_types=%d panel_instances=%d registered_hosts=%d",
                  area,
                  xr_region,
                  wm_xr_region_panel_type_count(xr_region),
                  wm_xr_region_panel_instance_count(xr_region),
                  BLI_listbase_count(&surface_data->panels));
  bool found_host = false;
  for (wmXrPanel *panel : ListBaseWrapper<wmXrPanel>(surface_data->panels)) {
    if (panel->panel_host_win != CTX_wm_window(C) || panel->panel_host_area != area ||
        panel->panel_host_region != xr_region)
    {
      continue;
    }
    found_host = true;
    panel->panel_host_region = xr_region;
    wm_xr_panel_mount_update(panel, xr);
    wm_xr_panel_cache_update(C, panel);

    if (!BLI_listbase_is_empty(&panel->temporary_regions)) {
      for (wmXrTempRegion *temp_region :
           ListBaseWrapper<wmXrTempRegion>(panel->temporary_regions))
      {
        wm_xr_temp_region_cache_update(C, panel, temp_region);
      }
    }
  }
  if (!found_host) {
    CLOG_ERROR(&LOG, "panels_ws: XR panel host not registered");
  }
}

}  // namespace blender
