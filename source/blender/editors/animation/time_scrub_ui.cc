/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edanimation
 */

#include "BKE_context.hh"
#include "BKE_scene.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "ED_time_scrub_ui.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "DNA_scene_types.h"

#include "BLI_math_base.h"
#include "BLI_rect.h"
#include "BLI_string_utf8.h"
#include "BLI_timecode.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#define LINE_WIDTH (3 * UI_SCALE_FAC + 1)

void ED_time_scrub_region_rect_get(const ARegion *region, rcti *r_rect)
{
  r_rect->xmin = 0;
  r_rect->xmax = region->winx;
  r_rect->ymax = region->winy;
  r_rect->ymin = r_rect->ymax - UI_TIME_SCRUB_MARGIN_Y;
}

static int get_centered_text_y(const rcti *rect)
{
  return BLI_rcti_cent_y(rect) - UI_SCALE_FAC * 4;
}

static void draw_background(const rcti *rect)
{
  uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  immUniformThemeColor(TH_TIME_SCRUB_BACKGROUND);

  GPU_blend(GPU_BLEND_ALPHA);

  immRectf(pos, rect->xmin, rect->ymin, rect->xmax, rect->ymax);

  GPU_blend(GPU_BLEND_NONE);

  immUnbindProgram();
}

static void get_current_time_str(
    const Scene *scene, bool display_seconds, const float frame, char *r_str, uint str_maxncpy)
{
  if (display_seconds) {
    BLI_timecode_string_from_time(r_str,
                                  str_maxncpy,
                                  -1,
                                  FRA2TIME(int(frame)),
                                  scene->frames_per_second(),
                                  U.timecode_style);
  }
  else if (scene->r.flag & SCER_SHOW_SUBFRAME) {
    BLI_snprintf_utf8(r_str, str_maxncpy, "%.02f", frame);
  }
  else {
    BLI_snprintf_utf8(r_str, str_maxncpy, "%d", int(frame));
  }
}

static void draw_frame_line(const float subframe_x,
                            const float region_height,
                            const float *fg_color,
                            const float *bg_color)
{
  const float line_width = LINE_WIDTH;
  rctf line_rect{};
  line_rect.xmin = floor(subframe_x - line_width / 2);
  line_rect.xmax = ceil(subframe_x + line_width / 2);
  line_rect.ymin = -UI_SCALE_FAC;
  line_rect.ymax = ceil(region_height);
  UI_draw_roundbox_4fv_ex(&line_rect, fg_color, nullptr, 1.0f, bg_color, UI_SCALE_FAC, 0.0f);
}

static void draw_current_frame(const Scene *scene,
                               bool display_seconds,
                               const View2D *v2d,
                               const rcti *scrub_region_rect,
                               const bool display_stalk,
                               const bool draw_line)
{
  const uiFontStyle *fstyle = UI_FSTYLE_WIDGET;
  const float current_frame = BKE_scene_ctime_get(scene);
  const float subframe_x = blender::ui::view2d_view_to_region_x(v2d, current_frame);

  constexpr int max_frame_string_len = 64;
  char frame_str[max_frame_string_len];
  get_current_time_str(scene, display_seconds, current_frame, frame_str, max_frame_string_len);

  const float text_width = blender::ui::fontstyle_string_width(fstyle, frame_str);
  const float text_padding = 4.0f * UI_SCALE_FAC;

  const float box_min_width = 24.0f * UI_SCALE_FAC;
  const float box_width = std::max(text_width + (2.0f * text_padding), box_min_width);
  const float box_margin = 2.0f * UI_SCALE_FAC;

  const float shadow_width = UI_SCALE_FAC;

  /* The stalk is a trapezoid which is thicker at the top and narrows down to the line width. */
  const float stalk_top = ceil(scrub_region_rect->ymin + 4.0 * UI_SCALE_FAC);
  const float stalk_bottom = scrub_region_rect->ymin;
  const float stalk_half_width_top = 6.0f * UI_SCALE_FAC;
  const float stalk_half_width_bottom = LINE_WIDTH / 2.0f;

  rctf box_rect{};
  box_rect.xmin = subframe_x - (box_width / 2.0f);
  box_rect.xmax = subframe_x + (box_width / 2.0f) + 1.0f;
  box_rect.ymin = floor(stalk_top - 1.0f * UI_SCALE_FAC);
  box_rect.ymax = ceil(scrub_region_rect->ymax - box_margin + shadow_width);

  uint pos;

  float fg_color[4];
  blender::ui::theme::get_color_4fv(TH_CFRAME, fg_color);
  float bg_color[4];
  blender::ui::theme::get_color_shade_4fv(TH_BACK, -20, bg_color);

  if (display_stalk) {
    /* Shadow for triangle below frame box. */
    GPUVertFormat *format = immVertexFormat();
    pos = GPU_vertformat_attr_add(format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
    GPU_blend(GPU_BLEND_ALPHA);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    GPU_polygon_smooth(true);
    immUniformColor4fv(bg_color);
    immBegin(GPU_PRIM_TRI_STRIP, 4);
    /* This constant ensures that the outline has the correct thickness despite the angle of the
     * stalk. This could use trigonometry but since it's unlikely to change often it's easier to
     * just set. */
    const float diag_offset = 0.4f * UI_SCALE_FAC;

    /* Only draw the stalk outline within the scrub area. Otherwise we are running into layering
     * issues here, where keys > line > stalk > keys. This is solved by carefully drawing to the
     * exact place, instead of relying on layering. */
    immVertex2f(pos, floor(subframe_x - stalk_half_width_top - diag_offset), stalk_top);
    immVertex2f(pos, ceil(subframe_x + stalk_half_width_top + diag_offset), stalk_top);
    immVertex2f(pos, floor(subframe_x - stalk_half_width_bottom) - diag_offset, stalk_bottom);
    immVertex2f(pos, ceil(subframe_x + stalk_half_width_bottom + diag_offset), stalk_bottom);

    GPU_polygon_smooth(false);
    immEnd();
    immUnbindProgram();

    /* Vertical line. */
    if (draw_line) {
      draw_frame_line(subframe_x, scrub_region_rect->ymax, fg_color, bg_color);
    }
  }

  /* Box. */
  draw_roundbox_corner_set(blender::ui::CNR_ALL);
  const float box_corner_radius = 4.0f * UI_SCALE_FAC;

  blender::ui::draw_roundbox_4fv_ex(
      &box_rect, fg_color, nullptr, 1.0f, bg_color, shadow_width, box_corner_radius);

  /* Frame number text. */
  uchar text_color[4];
  blender::ui::theme::get_color_4ubv(TH_HEADER_TEXT_HI, text_color);
  const int y = BLI_rcti_cent_y(scrub_region_rect) - int(fstyle->points * UI_SCALE_FAC * 0.38f);
  blender::ui::fontstyle_draw_simple(
      fstyle, subframe_x - (text_width / 2.0f), y, frame_str, text_color);

  if (display_stalk) {
    /* Trapezoid base under frame number. */
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    GPU_polygon_smooth(true);
    immBegin(GPU_PRIM_TRI_STRIP, 4);
    immUniformColor4fv(fg_color);

    immVertex2f(pos, floor(subframe_x - stalk_half_width_top + shadow_width), stalk_top);
    immVertex2f(pos, ceil(subframe_x + stalk_half_width_top - shadow_width), stalk_top);
    immVertex2f(pos, floor(subframe_x - stalk_half_width_bottom + shadow_width), stalk_bottom);
    immVertex2f(pos, ceil(subframe_x + stalk_half_width_bottom - shadow_width), stalk_bottom);

    immEnd();
    immUnbindProgram();
    GPU_polygon_smooth(false);
    GPU_blend(GPU_BLEND_NONE);
  }
}

void ED_time_scrub_draw_current_frame(const ARegion *region,
                                      const Scene *scene,
                                      bool display_seconds,
                                      const bool display_stalk,
                                      const bool draw_line)
{
  const View2D *v2d = &region->v2d;
  GPU_matrix_push_projection();
  wmOrtho2_region_pixelspace(region);

  rcti scrub_region_rect;
  ED_time_scrub_region_rect_get(region, &scrub_region_rect);

  draw_current_frame(scene, display_seconds, v2d, &scrub_region_rect, display_stalk, draw_line);
  GPU_matrix_pop_projection();
}

void ED_time_scrub_draw_current_frame_line(const ARegion *region, const Scene *scene)
{
  GPU_matrix_push_projection();
  wmOrtho2_region_pixelspace(region);

  rcti scrub_region_rect;
  ED_time_scrub_region_rect_get(region, &scrub_region_rect);

  float fg_color[4];
  UI_GetThemeColor4fv(TH_CFRAME, fg_color);
  float bg_color[4];
  UI_GetThemeColorShade4fv(TH_BACK, -20, bg_color);

  const float subframe_x = UI_view2d_view_to_region_x(&region->v2d, BKE_scene_ctime_get(scene));
  draw_frame_line(subframe_x, scrub_region_rect.ymax, fg_color, bg_color);

  GPU_matrix_pop_projection();
}

void ED_time_scrub_draw(const ARegion *region,
                        const Scene *scene,
                        bool display_seconds,
                        bool discrete_frames,
                        const int base)
{
  const View2D *v2d = &region->v2d;

  GPU_matrix_push_projection();
  wmOrtho2_region_pixelspace(region);

  rcti scrub_region_rect;
  ED_time_scrub_region_rect_get(region, &scrub_region_rect);

  draw_background(&scrub_region_rect);

  rcti numbers_rect = scrub_region_rect;
  numbers_rect.ymin = get_centered_text_y(&scrub_region_rect) - 4 * UI_SCALE_FAC;
  if (discrete_frames) {
    blender::ui::view2d_draw_scale_x__discrete_frames_or_seconds(
        region, v2d, &numbers_rect, scene, display_seconds, TH_TIME_SCRUB_TEXT, base);
  }
  else {
    blender::ui::view2d_draw_scale_x__frames_or_seconds(
        region, v2d, &numbers_rect, scene, display_seconds, TH_TIME_SCRUB_TEXT, base);
  }

  GPU_matrix_pop_projection();
}

rcti ED_time_scrub_clamp_scroller_mask(const rcti &scroller_mask)
{
  rcti clamped_mask = scroller_mask;
  clamped_mask.ymax -= UI_TIME_SCRUB_MARGIN_Y;
  return clamped_mask;
}

bool ED_time_scrub_event_in_region(const ARegion *region, const wmEvent *event)
{
  rcti rect = region->winrct;
  rect.ymin = rect.ymax - UI_TIME_SCRUB_MARGIN_Y;
  return BLI_rcti_isect_pt_v(&rect, event->xy);
}

bool ED_time_scrub_event_in_region_poll(const wmWindow * /*win*/,
                                        const ScrArea * /*area*/,
                                        const ARegion *region,
                                        const wmEvent *event)
{
  return ED_time_scrub_event_in_region(region, event);
}

void ED_time_scrub_channel_search_draw(const bContext *C, ARegion *region, bDopeSheet *dopesheet)
{
  GPU_matrix_push_projection();
  wmOrtho2_region_pixelspace(region);

  rcti rect;
  rect.xmin = 0;
  rect.xmax = region->winx;
  rect.ymin = region->winy - UI_TIME_SCRUB_MARGIN_Y;
  rect.ymax = region->winy;

  uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformThemeColor(TH_BACK);
  immRectf(pos, rect.xmin, rect.ymin, rect.xmax, rect.ymax);
  immUnbindProgram();

  PointerRNA ptr = RNA_pointer_create_discrete(&CTX_wm_screen(C)->id, &RNA_DopeSheet, dopesheet);

  const uiStyle *style = blender::ui::style_get_dpi();
  const float padding_x = 2 * UI_SCALE_FAC;
  const float padding_y = UI_SCALE_FAC;

  blender::ui::Block *block = block_begin(C, region, __func__, blender::ui::EmbossType::Emboss);
  blender::ui::Layout &layout = blender::ui::block_layout(block,
                                                          blender::ui::LayoutDirection::Vertical,
                                                          blender::ui::LayoutType::Header,
                                                          rect.xmin + padding_x,
                                                          rect.ymin + UI_UNIT_Y + padding_y,
                                                          BLI_rcti_size_x(&rect) - 2 * padding_x,
                                                          1,
                                                          0,
                                                          style);
  layout.scale_y_set((UI_UNIT_Y - padding_y) / UI_UNIT_Y);
  blender::ui::block_layout_set_current(block, &layout);
  block_align_begin(block);
  layout.prop(&ptr, "filter_text", UI_ITEM_NONE, "", ICON_NONE);
  layout.prop(&ptr, "use_filter_invert", UI_ITEM_NONE, "", ICON_ARROW_LEFTRIGHT);
  block_align_end(block);
  blender::ui::block_layout_resolve(block);

  /* Make sure the events are consumed from the search and don't reach other UI blocks since this
   * is drawn on top of animation-channels. */
  block_flag_enable(block, blender::ui::BLOCK_CLIP_EVENTS);
  block_bounds_set_normal(block, 0);
  block_end(C, block);
  block_draw(C, block);

  GPU_matrix_pop_projection();
}
