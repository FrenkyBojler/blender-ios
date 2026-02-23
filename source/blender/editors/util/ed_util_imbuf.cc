/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edutil
 */

#include <algorithm>

#include "MEM_guardedalloc.h"

#include "BLF_api.hh"

#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_string_utf8.h"

#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_screen.hh"

#include "ED_image.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "SEQ_render.hh"
#include "SEQ_sequencer.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "sequencer_intern.hh"

/* Own define. */
#include "ED_util_imbuf.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Image Pixel Sample Struct (Operator Custom Data)
 * \{ */

enum class ImageSampleInfoFormat { RGB = 1, HSL = 2, HSV = 3 };

struct ImageSampleInfo {
  ARegionType *art;
  void *draw_handle;
  int x, y;
  int channels;

  int width, height;
  int sample_size;

  uchar col[4];
  float colf[4];
  float linearcol[4];
  int z;
  float zf;

  uchar *colp;
  const float *colfp;
  int *zp;
  float *zfp;

  bool draw;
  bool color_manage;
  int use_default_view;

  ImageSampleInfoFormat format;
  bool show_managed;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image Pixel Sample
 * \{ */

static void image_sample_pixel_color_ubyte(const ImBuf *ibuf,
                                           const int coord[2],
                                           uchar r_col[4],
                                           float r_col_linear[4])
{
  const uchar *cp = ibuf->byte_buffer.data + 4 * (coord[1] * ibuf->x + coord[0]);
  copy_v4_v4_uchar(r_col, cp);
  rgba_uchar_to_float(r_col_linear, r_col);
  IMB_colormanagement_colorspace_to_scene_linear_v4(
      r_col_linear, false, ibuf->byte_buffer.colorspace);
}

static void image_sample_pixel_color_float(ImBuf *ibuf, const int coord[2], float r_col[4])
{
  const float *cp = ibuf->float_buffer.data + (ibuf->channels) * (coord[1] * ibuf->x + coord[0]);
  copy_v4_v4(r_col, cp);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image Pixel Region Sample
 * \{ */

static void image_sample_rect_color_ubyte(const ImBuf *ibuf,
                                          const rcti *rect,
                                          uchar r_col[4],
                                          float r_col_linear[4])
{
  uint col_accum_ub[4] = {0, 0, 0, 0};
  zero_v4(r_col_linear);
  int col_tot = 0;
  int coord[2];
  for (coord[0] = rect->xmin; coord[0] <= rect->xmax; coord[0]++) {
    for (coord[1] = rect->ymin; coord[1] <= rect->ymax; coord[1]++) {
      float col_temp_fl[4];
      uchar col_temp_ub[4];
      image_sample_pixel_color_ubyte(ibuf, coord, col_temp_ub, col_temp_fl);
      add_v4_v4(r_col_linear, col_temp_fl);
      col_accum_ub[0] += uint(col_temp_ub[0]);
      col_accum_ub[1] += uint(col_temp_ub[1]);
      col_accum_ub[2] += uint(col_temp_ub[2]);
      col_accum_ub[3] += uint(col_temp_ub[3]);
      col_tot += 1;
    }
  }
  mul_v4_fl(r_col_linear, 1.0 / float(col_tot));

  r_col[0] = std::min<uchar>(col_accum_ub[0] / col_tot, 255);
  r_col[1] = std::min<uchar>(col_accum_ub[1] / col_tot, 255);
  r_col[2] = std::min<uchar>(col_accum_ub[2] / col_tot, 255);
  r_col[3] = std::min<uchar>(col_accum_ub[3] / col_tot, 255);
}

static void image_sample_rect_color_float(ImBuf *ibuf, const rcti *rect, float r_col[4])
{
  zero_v4(r_col);
  int col_tot = 0;
  int coord[2];
  for (coord[0] = rect->xmin; coord[0] <= rect->xmax; coord[0]++) {
    for (coord[1] = rect->ymin; coord[1] <= rect->ymax; coord[1]++) {
      float col_temp_fl[4];
      image_sample_pixel_color_float(ibuf, coord, col_temp_fl);
      add_v4_v4(r_col, col_temp_fl);
      col_tot += 1;
    }
  }
  mul_v4_fl(r_col, 1.0 / float(col_tot));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image Pixel Sample (Internal Utilities)
 * \{ */

static void image_sample_apply(bContext *C, wmOperator *op, const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  ARegion *region = CTX_wm_region(C);
  Image *image = ED_space_image(sima);

  float uv[2];
  ui::view2d_region_to_view(&region->v2d, event->mval[0], event->mval[1], &uv[0], &uv[1]);
  int tile = BKE_image_get_tile_from_pos(sima->image, uv, uv, nullptr);

  void *lock;
  ImBuf *ibuf = ED_space_image_acquire_buffer(sima, &lock, tile, true);
  ImageSampleInfo *info = static_cast<ImageSampleInfo *>(op->customdata);
  Scene *scene = CTX_data_scene(C);
  CurveMapping *curve_mapping = scene->view_settings.curve_mapping;

  if (ibuf == nullptr) {
    ED_space_image_release_buffer(sima, ibuf, lock);
    info->draw = false;
    return;
  }

  const float2 offset = ibuf->flags & IB_has_display_window ? float2(ibuf->display_offset) :
                                                              float2(0.0f);
  int x = int(uv[0] * ibuf->x), y = int(uv[1] * ibuf->y);

  if (x >= offset[0] && y >= offset[1] && x < (ibuf->x + offset[0]) && y < (ibuf->y + offset[1])) {
    info->width = ibuf->x;
    info->height = ibuf->y;
    info->x = x;
    info->y = y;

    info->draw = true;
    info->channels = ibuf->channels;

    info->colp = nullptr;
    info->colfp = nullptr;
    info->zp = nullptr;
    info->zfp = nullptr;

    info->use_default_view = (image->flag & IMA_VIEW_AS_RENDER) ? false : true;

    rcti sample_rect;
    sample_rect.xmin = max_ii(0, x - offset[0] - info->sample_size / 2);
    sample_rect.ymin = max_ii(0, y - offset[1] - info->sample_size / 2);

    sample_rect.xmax = min_ii(ibuf->x - 1, x - offset[0] + info->sample_size / 2);
    sample_rect.ymax = min_ii(ibuf->y - 1, y - offset[1] + info->sample_size / 2);

    if (ibuf->byte_buffer.data) {
      image_sample_rect_color_ubyte(ibuf, &sample_rect, info->col, info->linearcol);
      rgba_uchar_to_float(info->colf, info->col);

      info->colp = info->col;
      info->colfp = info->colf;
      info->color_manage = true;
    }
    if (ibuf->float_buffer.data) {
      image_sample_rect_color_float(ibuf, &sample_rect, info->colf);

      if (ibuf->channels == 4) {
        /* pass */
      }
      else if (ibuf->channels == 3) {
        info->colf[3] = 1.0f;
      }
      else {
        info->colf[1] = info->colf[0];
        info->colf[2] = info->colf[0];
        info->colf[3] = 1.0f;
      }
      info->colfp = info->colf;

      copy_v4_v4(info->linearcol, info->colf);

      info->color_manage = true;
    }

    if (curve_mapping && ibuf->channels == 4) {
      /* we reuse this callback for set curves point operators */
      if (RNA_struct_find_property(op->ptr, "point")) {
        int point = RNA_enum_get(op->ptr, "point");

        if (point == 1) {
          BKE_curvemapping_set_black_white(curve_mapping, nullptr, info->linearcol);
        }
        else if (point == 0) {
          BKE_curvemapping_set_black_white(curve_mapping, info->linearcol, nullptr);
        }
        WM_event_add_notifier(C, NC_WINDOW, nullptr);
      }
    }

/* XXX node curve integration. */
#if 0
    {
      ScrArea *area, *cur = curarea;

      node_curvemap_sample(fp); /* sends global to node editor */
      for (area = G.curscreen->areabase.first; area; area = area->next) {
        if (area->spacetype == SPACE_NODE) {
          areawinset(area->win);
          scrarea_do_windraw(area);
        }
      }
      node_curvemap_sample(nullptr); /* clears global in node editor */
      curarea = cur;
    }
#endif
  }
  else {
    info->draw = false;
  }

  ED_space_image_release_buffer(sima, ibuf, lock);
  ED_area_tag_redraw(CTX_wm_area(C));
}

static void sequencer_sample_apply(bContext *C, wmOperator *op, const wmEvent *event)
{
  Scene *scene = CTX_data_sequencer_scene(C);
  ARegion *region = CTX_wm_region(C);
  ImBuf *ibuf = ed::vse::sequencer_ibuf_get(C, scene->r.cfra, nullptr);
  ImageSampleInfo *info = static_cast<ImageSampleInfo *>(op->customdata);
  float fx, fy;

  if (ibuf == nullptr) {
    info->draw = false;
    return;
  }

  ui::view2d_region_to_view(&region->v2d, event->mval[0], event->mval[1], &fx, &fy);

  fx /= scene->r.xasp / scene->r.yasp;

  fx += float(scene->r.xsch) / 2.0f;
  fy += float(scene->r.ysch) / 2.0f;
  fx *= float(ibuf->x) / float(scene->r.xsch);
  fy *= float(ibuf->y) / float(scene->r.ysch);

  if (fx >= 0.0f && fy >= 0.0f && fx < ibuf->x && fy < ibuf->y) {
    const float *fp;
    const uchar *cp;
    int x = int(fx), y = int(fy);

    info->x = x;
    info->y = y;
    info->draw = true;
    info->channels = ibuf->channels;

    info->colp = nullptr;
    info->colfp = nullptr;

    if (ibuf->byte_buffer.data) {
      cp = ibuf->byte_buffer.data + 4 * (y * ibuf->x + x);

      info->col[0] = cp[0];
      info->col[1] = cp[1];
      info->col[2] = cp[2];
      info->col[3] = cp[3];
      info->colp = info->col;

      info->colf[0] = float(cp[0]) / 255.0f;
      info->colf[1] = float(cp[1]) / 255.0f;
      info->colf[2] = float(cp[2]) / 255.0f;
      info->colf[3] = float(cp[3]) / 255.0f;
      info->colfp = info->colf;

      copy_v4_v4(info->linearcol, info->colf);
      IMB_colormanagement_colorspace_to_scene_linear_v4(
          info->linearcol, false, ibuf->byte_buffer.colorspace);

      info->color_manage = true;
    }
    if (ibuf->float_buffer.data) {
      fp = (ibuf->float_buffer.data + (ibuf->channels) * (y * ibuf->x + x));

      info->colf[0] = fp[0];
      info->colf[1] = fp[1];
      info->colf[2] = fp[2];
      info->colf[3] = fp[3];
      info->colfp = info->colf;

      /* sequencer's image buffers are in non-linear space, need to make them linear */
      copy_v4_v4(info->linearcol, info->colf);
      seq::render_pixel_from_sequencer_space_v4(scene, info->linearcol);

      info->color_manage = true;
    }
  }
  else {
    info->draw = false;
  }

  IMB_freeImBuf(ibuf);
  ED_area_tag_redraw(CTX_wm_area(C));
}

static void ed_imbuf_sample_apply(bContext *C, wmOperator *op, const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  if (area == nullptr) {
    return;
  }

  switch (area->spacetype) {
    case SPACE_IMAGE: {
      image_sample_apply(C, op, event);
      break;
    }
    case SPACE_SEQ: {
      sequencer_sample_apply(C, op, event);
      break;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image Pixel Sample (Public Operator Callback)
 *
 * Callbacks for the sample operator, used by sequencer and image spaces.
 * \{ */

static void ed_imbuf_sample_rectangle(Scene *scene,
                                      ARegion *region,
                                      ImageSampleInfo *info)
{
  bool color_manage = info->color_manage;
  bool use_default_view = info->use_default_view;
  int channels = info->channels;
  const uchar *cp = info->colp;
  const float *fp = info->colfp;
  const float *linearcol = info->linearcol;

  float col[4];
  float finalcol[4];
  rcti color_rect;
  int dx = 0;

  /* local coordinate visible rect inside region, to accommodate overlapping ui */
  const rcti *rect = ED_region_visible_rect(region);
  const int ymin = rect->ymin;
  const int dy = ymin + 0.3f * UI_UNIT_Y;

  BLI_rcti_init(&color_rect,
                dx,
                dx + (1.5f * UI_UNIT_X),
                ymin + 0.15f * UI_UNIT_Y,
                ymin + 0.85f * UI_UNIT_Y);

  /* color rectangle */
  if (channels == 1) {
    if (fp) {
      col[0] = col[1] = col[2] = fp[0];
    }
    else if (cp) {
      col[0] = col[1] = col[2] = float(cp[0]) / 255.0f;
    }
    else {
      col[0] = col[1] = col[2] = 0.0f;
    }
    col[3] = 1.0f;
  }
  else if (channels == 3) {
    copy_v3_v3(col, linearcol);
    col[3] = 1.0f;
  }
  else if (channels == 4) {
    copy_v4_v4(col, linearcol);
  }
  else {
    BLI_assert(0);
    zero_v4(col);
  }

  if (color_manage && info->show_managed) {
    IMB_colormanagement_pixel_to_display_space_v4(finalcol,
                                                  col,
                                                  (use_default_view) ? nullptr :
                                                                       &scene->view_settings,
                                                  &scene->display_settings);
  }
  else {
    copy_v4_v4(finalcol, col);
  }

  GPU_blend(GPU_BLEND_NONE);
  dx += 0.25f * UI_UNIT_X;

  BLI_rcti_init(&color_rect,
                dx,
                dx + (1.5f * UI_UNIT_X),
                ymin + 0.15f * UI_UNIT_Y,
                ymin + 0.85f * UI_UNIT_Y);

  /* BLF uses immediate mode too, so we must reset our vertex format */
  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  if (channels == 4) {
    rcti color_rect_half;
    int color_quater_x, color_quater_y;

    color_rect_half = color_rect;
    color_rect_half.xmax = BLI_rcti_cent_x(&color_rect);
    /* what color ??? */
    immRectf(pos, color_rect.xmin, color_rect.ymin, color_rect.xmax, color_rect.ymax);

    color_rect_half = color_rect;
    color_rect_half.xmin = BLI_rcti_cent_x(&color_rect);

    color_quater_x = BLI_rcti_cent_x(&color_rect_half);
    color_quater_y = BLI_rcti_cent_y(&color_rect_half);

    immUniformColor3ub(UI_ALPHA_CHECKER_DARK, UI_ALPHA_CHECKER_DARK, UI_ALPHA_CHECKER_DARK);
    immRectf(pos,
             color_rect_half.xmin,
             color_rect_half.ymin,
             color_rect_half.xmax,
             color_rect_half.ymax);

    immUniformColor3ub(UI_ALPHA_CHECKER_LIGHT, UI_ALPHA_CHECKER_LIGHT, UI_ALPHA_CHECKER_LIGHT);
    immRectf(pos, color_quater_x, color_quater_y, color_rect_half.xmax, color_rect_half.ymax);
    immRectf(pos, color_rect_half.xmin, color_rect_half.ymin, color_quater_x, color_quater_y);

    if (fp != nullptr || cp != nullptr) {
      GPU_blend(GPU_BLEND_ALPHA);
      immUniformColor3fvAlpha(finalcol, fp ? fp[3] : (cp[3] / 255.0f));
      immRectf(pos, color_rect.xmin, color_rect.ymin, color_rect.xmax, color_rect.ymax);
      GPU_blend(GPU_BLEND_NONE);
    }
  }
  else {
    immUniformColor3fv(finalcol);
    immRectf(pos, color_rect.xmin, color_rect.ymin, color_rect.xmax, color_rect.ymax);
  }
  immUnbindProgram();

  /* draw outline */
  pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor3ub(128, 128, 128);
  imm_draw_box_wire_2d(pos, color_rect.xmin, color_rect.ymin, color_rect.xmax, color_rect.ymax);
  immUnbindProgram();
}


static void ed_imbuf_sample_draw_info(Scene *scene, ARegion *region, ImageSampleInfo *info)
{
  bool color_manage = info->color_manage;
  bool use_default_view = info->use_default_view;
  int channels = info->channels;
  const uchar *cp = info->colp;
  const float *fp = info->colfp;
  const float *linearcol = info->linearcol;

  char str[256];
  int dx = 45 * UI_SCALE_FAC;
  /* local coordinate visible rect inside region, to accommodate overlapping ui */
  const rcti *rect = ED_region_visible_rect(region);
  const int ymin = rect->ymin;
  const int dy = ymin + 0.3f * UI_UNIT_Y;

/* text colors */
/* XXX colored text not allowed in Blender UI */
#if 0
  uchar red[3] = {255, 50, 50};
  uchar green[3] = {0, 255, 0};
  uchar blue[3] = {100, 100, 255};
#else
  const uchar red[3] = {255, 255, 255};
  const uchar green[3] = {255, 255, 255};
  const uchar blue[3] = {255, 255, 255};
#endif
  float hue = 0, sat = 0, val = 0, lum = 0, u = 0, v = 0;
  float finalcol[4];

  GPU_blend(GPU_BLEND_ALPHA);

  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* noisy, high contrast make impossible to read if lower alpha is used. */
  immUniformColor4ub(0, 0, 0, 190);
  immRectf(pos, 0, ymin, BLI_rcti_size_x(&region->winrct) + 1, ymin + UI_UNIT_Y);

  immUnbindProgram();

  ed_imbuf_sample_rectangle(scene, region, info);

  GPU_blend(GPU_BLEND_NONE);

  BLF_size(blf_mono_font, 11.0f * UI_SCALE_FAC);

  BLF_color3ub(blf_mono_font, 255, 255, 255);

  if (channels == 1 && (cp != nullptr || fp != nullptr)) {
    if (fp != nullptr) {
      SNPRINTF_UTF8(str, " Val:%-.3f |", fp[0]);
    }
    else if (cp != nullptr) {
      SNPRINTF_UTF8(str, " Val:%-.3f |", cp[0] / 255.0f);
    }
    BLF_color3ub(blf_mono_font, 255, 255, 255);
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));
  }

  if (channels >= 3) {
    BLF_color3ubv(blf_mono_font, red);
    ui::icon_draw(dx, dy - (3 * UI_SCALE_FAC), ICON_RGB_RED);
    dx += 20 * UI_SCALE_FAC;
    if (fp) {
      SNPRINTF_UTF8(str, "%-.5f", fp[0]);
    }
    else if (cp) {
      SNPRINTF_UTF8(str, "%-3d", cp[0]);
    }
    else {
      STRNCPY_UTF8(str, "-");
    }
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));
    dx += 10 * UI_SCALE_FAC;

    BLF_color3ubv(blf_mono_font, green);
    ui::icon_draw(dx, dy - (3 * UI_SCALE_FAC), ICON_RGB_GREEN);
    dx += 20 * UI_SCALE_FAC;
    if (fp) {
      SNPRINTF_UTF8(str, "%-.5f", fp[1]);
    }
    else if (cp) {
      SNPRINTF_UTF8(str, "%-3d", cp[1]);
    }
    else {
      STRNCPY_UTF8(str, "-");
    }
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));
    dx += 10 * UI_SCALE_FAC;

    BLF_color3ubv(blf_mono_font, blue);
    ui::icon_draw(dx, dy - (3 * UI_SCALE_FAC), ICON_RGB_BLUE);
    dx += 20 * UI_SCALE_FAC;
    if (fp) {
      SNPRINTF_UTF8(str, "%-.5f", fp[2]);
    }
    else if (cp) {
      SNPRINTF_UTF8(str, "%-3d", cp[2]);
    }
    else {
      STRNCPY_UTF8(str, "-");
    }
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));
    dx += 10 * UI_SCALE_FAC;

    if (channels == 4) {
      BLF_color3ub(blf_mono_font, 255, 255, 255);
      if (fp) {
        SNPRINTF_UTF8(str, "A:%-.4f", fp[3]);
      }
      else if (cp) {
        SNPRINTF_UTF8(str, "A:%-3d", cp[3]);
      }
      else {
        STRNCPY_UTF8(str, "A:-");
      }
      BLF_position(blf_mono_font, dx, dy, 0);
      BLF_draw(blf_mono_font, str, sizeof(str));
      dx += BLF_width(blf_mono_font, str, sizeof(str));
    }

    if (false && color_manage) {
      float rgba[4];

      copy_v3_v3(rgba, linearcol);
      if (channels == 3) {
        rgba[3] = 1.0f;
      }
      else {
        rgba[3] = linearcol[3];
      }

      IMB_colormanagement_pixel_to_display_space_v4(rgba,
                                                    rgba,
                                                    (use_default_view) ? nullptr :
                                                                         &scene->view_settings,
                                                    &scene->display_settings,
                                                    DISPLAY_SPACE_COLOR_INSPECTION);

      SNPRINTF_UTF8(str, "  |  Display  R:%-.4f  G:%-.4f  B:%-.4f", rgba[0], rgba[1], rgba[2]);
      BLF_position(blf_mono_font, dx, dy, 0);
      BLF_draw(blf_mono_font, str, sizeof(str));
      dx += BLF_width(blf_mono_font, str, sizeof(str));
    }
  }

  dx += 1.75f * UI_UNIT_X;

  BLF_color3ub(blf_mono_font, 255, 255, 255);
  if (false && channels == 1) {
    if (fp) {
      rgb_to_hsv(fp[0], fp[0], fp[0], &hue, &sat, &val);
      rgb_to_yuv(fp[0], fp[0], fp[0], &lum, &u, &v, BLI_YUV_ITU_BT709);
    }
    else if (cp) {
      rgb_to_hsv(
          float(cp[0]) / 255.0f, float(cp[0]) / 255.0f, float(cp[0]) / 255.0f, &hue, &sat, &val);
      rgb_to_yuv(float(cp[0]) / 255.0f,
                 float(cp[0]) / 255.0f,
                 float(cp[0]) / 255.0f,
                 &lum,
                 &u,
                 &v,
                 BLI_YUV_ITU_BT709);
    }

    SNPRINTF_UTF8(str, "V:%-.4f", val);
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));

    SNPRINTF_UTF8(str, "   L:%-.4f", lum);
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
  }
  else if (false && channels >= 3) {
    rgb_to_hsv(finalcol[0], finalcol[1], finalcol[2], &hue, &sat, &val);
    rgb_to_yuv(finalcol[0], finalcol[1], finalcol[2], &lum, &u, &v, BLI_YUV_ITU_BT709);

    SNPRINTF_UTF8(str, "H:%-.4f", hue);
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));

    SNPRINTF_UTF8(str, "  S:%-.4f", sat);
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));

    SNPRINTF_UTF8(str, "  V:%-.4f", val);
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));

    SNPRINTF_UTF8(str, "   L:%-.4f", lum);
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
  }
}


void ED_imbuf_sample_draw(const bContext *C, ARegion *region, void *arg_info)
{
  ImageSampleInfo *info = static_cast<ImageSampleInfo *>(arg_info);
  if (!info->draw) {
    return;
  }

  Scene *scene = CTX_data_scene(C);
  ed_imbuf_sample_draw_info(scene, region, info);

  if (info->sample_size > 1) {
    ScrArea *area = CTX_wm_area(C);

    if (area && area->spacetype == SPACE_IMAGE) {

      const wmWindow *win = CTX_wm_window(C);
      const wmEvent *event = win->runtime->eventstate;

      SpaceImage *sima = CTX_wm_space_image(C);
      GPUVertFormat *format = immVertexFormat();
      uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);

      const float color[3] = {1, 1, 1};
      immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
      immUniformColor3fv(color);

      /* TODO(@ideasman42): lock to pixels. */
      rctf sample_rect_fl;
      BLI_rctf_init_pt_radius(&sample_rect_fl,
                              float2{float(event->xy[0] - region->winrct.xmin),
                                     float(event->xy[1] - region->winrct.ymin)},
                              float(info->sample_size / 2.0f) * sima->zoom);

      GPU_logic_op_xor_set(true);

      GPU_line_width(1.0f);
      imm_draw_box_wire_2d(
          pos, sample_rect_fl.xmin, sample_rect_fl.ymin, sample_rect_fl.xmax, sample_rect_fl.ymax);

      GPU_logic_op_xor_set(false);

      immUnbindProgram();
    }
  }
}

static void ed_imbuf_sample_status(bContext *C, ImageSampleInfo *info)
{
  const std::string header_status = fmt::format("X: {} / {} ({}%), Y: {} / {} ({}%)",
                                                info->x,
                                                info->width,
                                                (info->x * 100) / info->width,
                                                info->y,
                                                info->height,
                                                (info->y * 100) / info->height);

  ED_area_status_text(CTX_wm_area(C), header_status.c_str());

  WorkspaceStatus status(C);
  status.item("Drag to sample", ICON_MOUSE_LMB_DRAG);
  status.item_bool("Toggle Color Managed", info->show_managed, ICON_EVENT_ALT);

  if (info->format == ImageSampleInfoFormat::HSL) {
    status.item("Format (HSL)", ICON_EVENT_CTRL);
  }
  else if (info->format == ImageSampleInfoFormat::HSV) {
    status.item("Format (HSV)", ICON_EVENT_CTRL);
  }
  else {
    status.item("Format (RGB)", ICON_EVENT_CTRL);
  }
}

void ED_imbuf_sample_exit(bContext *C, wmOperator *op)
{
  ImageSampleInfo *info = static_cast<ImageSampleInfo *>(op->customdata);

  ED_area_status_text(CTX_wm_area(C), nullptr);
  ED_workspace_status_text(C, nullptr);

  WM_cursor_set(CTX_wm_window(C), WM_CURSOR_DEFAULT);

  ED_region_draw_cb_exit(info->art, info->draw_handle);
  ED_area_tag_redraw(CTX_wm_area(C));
  MEM_delete(info);
}

wmOperatorStatus ED_imbuf_sample_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  ScrArea *area = CTX_wm_area(C);
  if (area) {
    switch (area->spacetype) {
      case SPACE_IMAGE: {
        SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
        if (region->regiontype == RGN_TYPE_WINDOW) {
          if (ED_space_image_show_cache_and_mval_over(sima, region, event->mval)) {
            return OPERATOR_PASS_THROUGH;
          }
        }
        if (!ED_space_image_has_buffer(sima)) {
          return OPERATOR_CANCELLED;
        }
        break;
      }
      case SPACE_SEQ: {
        /* Sequencer checks could be added. */
        break;
      }
    }
  }

  ImageSampleInfo *info = MEM_new_zeroed<ImageSampleInfo>("ImageSampleInfo");

  info->art = region->runtime->type;
  info->draw_handle = ED_region_draw_cb_activate(
      region->runtime->type, ED_imbuf_sample_draw, info, REGION_DRAW_POST_PIXEL);
  info->sample_size = RNA_int_get(op->ptr, "size");
  op->customdata = info;

  ed_imbuf_sample_apply(C, op, event);

  WM_event_add_modal_handler(C, op);

  ed_imbuf_sample_status(C, info);

  WM_cursor_set(CTX_wm_window(C), WM_CURSOR_CROSS);

  return OPERATOR_RUNNING_MODAL;
}

wmOperatorStatus ED_imbuf_sample_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  ImageSampleInfo *info = static_cast<ImageSampleInfo *>(op->customdata);
  switch (event->type) {
    case LEFTMOUSE:
    case RIGHTMOUSE: /* XXX hardcoded */
      if (event->val == KM_RELEASE) {
        ED_imbuf_sample_exit(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    case MOUSEMOVE:
      ed_imbuf_sample_apply(C, op, event);
      break;
    case EVT_LEFTALTKEY:
    case EVT_RIGHTALTKEY:
      if (event->val == KM_PRESS) {
        info->show_managed = !info->show_managed;
      }
      break;
    case EVT_LEFTCTRLKEY:
    case EVT_RIGHTCTRLKEY:
      if (event->val == KM_PRESS) {
        /* cycle through all the ImageSampleInfoFormat types */
        int v = static_cast<int>(info->format);
        /* Enum values are explicitly 1..3 (RGB=1, HSL=2, HSV=3). Increment and wrap. */
        v += 1;
        if (v > static_cast<int>(ImageSampleInfoFormat::HSV)) {
          v = static_cast<int>(ImageSampleInfoFormat::RGB);
        }
        info->format = static_cast<ImageSampleInfoFormat>(v);
      }
      break;
    default: {
      break;
    }
  }

  ed_imbuf_sample_status(C, info);

  return OPERATOR_RUNNING_MODAL;
}

void ED_imbuf_sample_cancel(bContext *C, wmOperator *op)
{
  ED_imbuf_sample_exit(C, op);
}

bool ED_imbuf_sample_poll(bContext *C)
{
  ScrArea *area = CTX_wm_area(C);
  if (area == nullptr) {
    return false;
  }

  switch (area->spacetype) {
    case SPACE_IMAGE: {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      Object *obedit = CTX_data_edit_object(C);
      if (obedit) {
        /* Disable when UV editing so it doesn't swallow all click events
         * (use for setting cursor). */
        if (ED_space_image_show_uvedit(sima, obedit)) {
          return false;
        }
      }
      else if (sima->mode != SI_MODE_VIEW) {
        return false;
      }
      return true;
    }
    case SPACE_SEQ: {
      SpaceSeq *sseq = static_cast<SpaceSeq *>(area->spacedata.first);

      if (sseq->mainb != SEQ_DRAW_IMG_IMBUF) {
        return false;
      }
      if (seq::editing_get(CTX_data_sequencer_scene(C)) == nullptr) {
        return false;
      }
      ARegion *region = CTX_wm_region(C);
      if (!(region && (region->regiontype == RGN_TYPE_PREVIEW))) {
        return false;
      }
      return true;
    }
  }

  return false;
}

/** \} */

}  // namespace blender
