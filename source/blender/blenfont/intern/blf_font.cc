/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blf
 *
 * Deals with drawing text to OpenGL or bitmap buffers.
 *
 * Also low level functions for managing \a FontBLF.
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ft2build.h>

#include FT_FREETYPE_H
#include FT_CACHE_H /* FreeType Cache. */
#include FT_GLYPH_H
#include FT_MULTIPLE_MASTERS_H /* Variable font support. */
#include FT_TRUETYPE_IDS_H     /* Code-point coverage constants. */
#include FT_TRUETYPE_TABLES_H  /* For TT_OS2 */

#ifdef WITH_HARFBUZZ
#  include <harfbuzz/hb-ft.h>
#  include <harfbuzz/hb-ot.h>
#  include <harfbuzz/hb.h>
#endif

#include "MEM_guardedalloc.h"

#include "DNA_userdef_types.h"
#include "DNA_vec_types.h"

#include "BLI_math_bits.h"
#include "BLI_math_color_blend.h"
#include "BLI_math_matrix.h"
#include "BLI_mutex.hh"
#include "BLI_path_utils.hh"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_cursor_utf8.h"
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"

#include "BLF_api.hh"

#include "BLT_lang.hh"

#include "GPU_batch.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "blf_internal.hh"
#include "blf_internal_types.hh"

#include "BLI_strict_flags.h" /* IWYU pragma: keep. Keep last. */

namespace blender {

#ifdef WIN32
#  define FT_New_Face FT_New_Face__win32_compat
#endif

/* Batching buffer for drawing. */

BatchBLF g_batch;

/* `freetype2` handle ONLY for this file! */
static FT_Library ft_lib = nullptr;
static FTC_Manager ftc_manager = nullptr;
static FTC_CMapCache ftc_charmap_cache = nullptr;

/* Mutex around face creation and deletion. */
static Mutex ft_face_load_mutex;

/* Lock around places that query free type caching system, and use the
 * calculated ft_size result. `FTC_Manager_LookupSize` can remove
 * ft_size of a completely different font instance, when the cache
 * is full! */
static Mutex ft_cache_size_mutex;

/* May be set to #widgetbase_draw_cache_flush. */
static void (*blf_draw_cache_flush)() = nullptr;

static ft_pix blf_font_height_max_ft_pix(FontBLF *font);
static ft_pix blf_font_width_max_ft_pix(FontBLF *font);

/* -------------------------------------------------------------------- */

/** \name FreeType Caching
 * \{ */

static bool blf_setup_face(FontBLF *font);

/**
 * Called when a face is removed by the cache. FreeType will call #FT_Done_Face.
 */
static void blf_face_finalizer(void *object)
{
  FT_Face face = static_cast<FT_Face>(object);
  FontBLF *font = static_cast<FontBLF *>(face->generic.data);
  font->face = nullptr;
}

/**
 * Called in response to #FTC_Manager_LookupFace. Now add a face to our font.
 *
 * \note Unused arguments are kept to match #FTC_Face_Requester function signature.
 */
static FT_Error blf_cache_face_requester(FTC_FaceID faceID,
                                         FT_Library lib,
                                         FT_Pointer /*req_data*/,
                                         FT_Face *face)
{
  FontBLF *font = static_cast<FontBLF *>(faceID);
  int err = FT_Err_Cannot_Open_Resource;

  std::scoped_lock lock(ft_face_load_mutex);
  if (font->filepath) {
    err = FT_New_Face(lib, font->filepath, 0, face);
  }
  else if (font->mem) {
    err = FT_New_Memory_Face(
        lib, static_cast<const FT_Byte *>(font->mem), FT_Long(font->mem_size), 0, face);
  }

  if (err == FT_Err_Ok) {
    font->face = *face;
    font->face->generic.data = font;
    font->face->generic.finalizer = blf_face_finalizer;

    /* More FontBLF setup now that we have a face. */
    if (!blf_setup_face(font)) {
      err = FT_Err_Cannot_Open_Resource;
    }
  }
  else {
    /* Clear this on error to avoid exception in FTC_Manager_LookupFace. */
    *face = nullptr;
  }

  return err;
}

/**
 * Called when the FreeType cache is removing a font size.
 */
static void blf_size_finalizer(void *object)
{
  FT_Size size = static_cast<FT_Size>(object);
  FontBLF *font = static_cast<FontBLF *>(size->generic.data);
  font->ft_size = nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name FreeType Utilities (Internal)
 * \{ */

uint blf_get_char_index(FontBLF *font, const uint charcode)
{
  return FTC_CMapCache_Lookup(ftc_charmap_cache, font, -1, charcode);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glyph Batching
 * \{ */

/**
 * Draw-calls are precious! make them count!
 * Since most of the Text elements are not covered by other UI elements, we can
 * group some strings together and render them in one draw-call. This behavior
 * is on demand only, between #BLF_batch_draw_begin() and #BLF_batch_draw_end().
 */
static void blf_batch_draw_init()
{
  g_batch.glyph_buf = GPU_storagebuf_create_ex(
      sizeof(g_batch.glyph_data), nullptr, GPU_USAGE_STREAM, __func__);
  g_batch.glyph_len = 0;
  /* We render a quad as a triangle strip and instance it for each glyph. */
  g_batch.batch = GPU_batch_create_procedural(GPU_PRIM_TRI_STRIP, 4);
}

static void blf_batch_draw_exit()
{
  GPU_BATCH_DISCARD_SAFE(g_batch.batch);
  if (g_batch.glyph_buf) {
    GPU_storagebuf_free(g_batch.glyph_buf);
  }
}

void blf_batch_draw_begin(FontBLF *font)
{
  if (g_batch.batch == nullptr) {
    blf_batch_draw_init();
  }

  const bool font_changed = (g_batch.font != font);
  const bool simple_shader = ((font->flags & (BLF_ROTATION | BLF_ASPECT)) == 0);
  const bool shader_changed = (simple_shader != g_batch.simple_shader);

  g_batch.active = g_batch.enabled && simple_shader;

  if (simple_shader) {
    /* Offset is applied to each glyph. */
    g_batch.ofs[0] = font->pos[0];
    g_batch.ofs[1] = font->pos[1];
  }
  else {
    /* Offset is baked in model-view matrix. */
    zero_v2_int(g_batch.ofs);
  }

  if (g_batch.active) {
    float gpumat[4][4];
    GPU_matrix_model_view_get(gpumat);

    bool mat_changed = equals_m4m4(gpumat, g_batch.mat) == false;

    if (mat_changed) {
      /* Model view matrix is no longer the same.
       * Flush cache but with the previous matrix. */
      GPU_matrix_push();
      GPU_matrix_set(g_batch.mat);
    }

    /* Flush cache if configuration is not the same. */
    if (mat_changed || font_changed || shader_changed) {
      blf_batch_draw();
      g_batch.simple_shader = simple_shader;
      g_batch.font = font;
    }
    else {
      /* Nothing changed continue batching. */
      return;
    }

    if (mat_changed) {
      GPU_matrix_pop();
      /* Save for next `memcmp`. */
      memcpy(g_batch.mat, gpumat, sizeof(g_batch.mat));
    }
  }
  else {
    /* Flush cache. */
    blf_batch_draw();
    g_batch.font = font;
    g_batch.simple_shader = simple_shader;
  }
}

static gpu::Texture *blf_batch_cache_texture_load()
{
  GlyphCacheBLF *gc = g_batch.glyph_cache;
  BLI_assert(gc);
  BLI_assert(gc->bitmap_len > 0);

  if (gc->bitmap_len > gc->bitmap_len_landed) {
    const int tex_width = GPU_texture_width(gc->texture);

    int bitmap_len_landed = gc->bitmap_len_landed;
    int remain = gc->bitmap_len - bitmap_len_landed;
    int offset_x = bitmap_len_landed % tex_width;
    int offset_y = bitmap_len_landed / tex_width;

    /* TODO(@germano): Update more than one row in a single call. */
    while (remain) {
      int remain_row = tex_width - offset_x;
      int width = remain > remain_row ? remain_row : remain;
      GPU_texture_update_sub(gc->texture,
                             GPU_DATA_UBYTE,
                             &gc->bitmap_result[bitmap_len_landed],
                             offset_x,
                             offset_y,
                             0,
                             width,
                             1,
                             0);

      bitmap_len_landed += width;
      remain -= width;
      offset_x = 0;
      offset_y += 1;
    }

    gc->bitmap_len_landed = bitmap_len_landed;
  }

  return gc->texture;
}

void blf_batch_draw()
{
  if (g_batch.glyph_len == 0) {
    return;
  }

  GPU_blend(GPU_BLEND_ALPHA);

  /* We need to flush widget base first to ensure correct ordering. */
  if (blf_draw_cache_flush != nullptr) {
    blf_draw_cache_flush();
  }

  gpu::Texture *texture = blf_batch_cache_texture_load();
  GPU_storagebuf_usage_size_set(g_batch.glyph_buf, size_t(g_batch.glyph_len) * sizeof(GlyphQuad));
  GPU_storagebuf_update(g_batch.glyph_buf, g_batch.glyph_data);
  GPU_storagebuf_bind(g_batch.glyph_buf, 0);

  GPU_batch_program_set_builtin(g_batch.batch, GPU_SHADER_TEXT);
  GPU_batch_texture_bind(g_batch.batch, "glyph", texture);
  /* Setup texture width mask and shift, so that shader can avoid costly divisions. */
  int tex_width = GPU_texture_width(texture);
  BLI_assert_msg(is_power_of_2_i(tex_width), "Font texture width must be power of two");
  int width_shift = 31 - bitscan_reverse_i(tex_width);
  GPU_batch_uniform_1i(g_batch.batch, "glyph_tex_width_mask", tex_width - 1);
  GPU_batch_uniform_1i(g_batch.batch, "glyph_tex_width_shift", width_shift);
  GPU_batch_draw_advanced(g_batch.batch, 0, 4, 0, g_batch.glyph_len);

  GPU_blend(GPU_BLEND_NONE);

  GPU_texture_unbind(texture);
  g_batch.glyph_len = 0;
}

static void blf_batch_draw_end()
{
  if (!g_batch.active) {
    blf_batch_draw();
  }
}

void BLF_batch_discard()
{
  if (g_batch.glyph_buf) {
    GPU_storagebuf_free(g_batch.glyph_buf);
    g_batch.glyph_buf = GPU_storagebuf_create_ex(
        sizeof(g_batch.glyph_data), nullptr, GPU_USAGE_STREAM, __func__);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Drawing: GPU
 * \{ */

struct Glyph {
  FontBLF *font = nullptr;
  GlyphCacheBLF *gc = nullptr;
  GlyphBLF *g = nullptr;
  /* Differs from GlyphBLF bounds in that each is from common origin
   * and includes the contextual positional offsets. In ft_pix. */
  rcti bounds = {};
  /* Index into the UTF-8 version of the original unshaped string. */
  size_t index_utf8 = 0;
  rcti integer_bounds() const
  {
    rcti r;
    r.xmin = ft_pix_to_int_floor(bounds.xmin);
    r.xmax = ft_pix_to_int_floor(bounds.xmax);
    r.ymin = ft_pix_to_int_floor(bounds.ymin);
    r.ymax = ft_pix_to_int_ceil(bounds.ymax);
    return r;
  }
};

struct ShapingData {
  blender::Vector<Glyph> glyphs = {};
  ft_pix width = 0;
  ft_pix height = 0;
  ShapingData(FontBLF *font, GlyphCacheBLF *gc, const char *str, size_t len);
};

ShapingData::ShapingData(FontBLF *font, GlyphCacheBLF *gc, const char *str, size_t len)
{
  if (!str || !str[0] || !len) {
    return;
  }
  size_t segment_start = 0;
  size_t segment_len = 0;
  FontBLF *segment_font = font;
  hb_script_t script = HB_SCRIPT_UNKNOWN;
  hb_script_t last_script = HB_SCRIPT_UNKNOWN;
  hb_buffer_t *hb_buf = hb_buffer_create();
  /* Include space for null terminator. */
  size_t char_count = BLI_strnlen_utf8(str, len) + 1;
  std::u32string str32(char_count, 0);
  /* Convert entire input string into array of 32-bit code points. */
  BLI_str_utf8_as_utf32(str32.data(), str, char_count);
  /* Harfbuzz gets the entire string but we process it by segment,
   * portions with the same language, direction, style, etc. */
#if 0
  printf("\n%s\n", str);
#endif
  while ((segment_start + segment_len) < char_count) {
    segment_start += segment_len;
    segment_len = 0;
    size_t i;
    for (i = segment_start; i < char_count && str32[i]; i++) {
      script = hb_unicode_script(hb_unicode_funcs_get_default(), str32[i]);
      if (script != last_script && script != HB_SCRIPT_INHERITED && script != HB_SCRIPT_COMMON) {
        last_script = script;
        if (i > 0) {
          break;
        }
      }
    }
    segment_len = (i - segment_start);
    if (segment_len == 0) {
      break;
    }
    hb_buffer_clear_contents(hb_buf);
    hb_buffer_add_utf32(
        hb_buf, (uint32_t *)str32.data(), int(char_count), uint(segment_start), int(segment_len));
    hb_buffer_guess_segment_properties(hb_buf);
    script = hb_buffer_get_script(hb_buf);
    if (script == HB_SCRIPT_HAN) {
      hb_buffer_set_language(hb_buf, hb_language_from_string(BLT_lang_get(), -1));
    }
    /* Is the current font ideal for this script? */
    segment_font = font;
    if (!ELEM(script, HB_SCRIPT_COMMON, HB_SCRIPT_INHERITED, HB_SCRIPT_UNKNOWN, HB_SCRIPT_LATIN)) {
      segment_font = blf_font_script_ensure(font, str32[segment_start]);
    }
    if (!segment_font->hb_font) {
      segment_font->hb_font = hb_ft_font_create_referenced(segment_font->face);
      hb_ot_font_set_funcs(segment_font->hb_font);
    }
    hb_font_set_scale(
        segment_font->hb_font, ft_pix_from_float(font->size), ft_pix_from_float(font->size));
    hb_buffer_set_cluster_level(hb_buf, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
    hb_shape_full(segment_font->hb_font,
                  hb_buf,
                  font->features.data(),
                  uint(font->features.size()),
                  nullptr);
    /* Unlikely. Drawing monospaced but changed mid-string to a proportional font. */
    bool set_mono = segment_font != font && font->flags & BLF_MONOSPACED &&
                    !(segment_font->flags & BLF_MONOSPACED);
    if (set_mono) {
      segment_font->flags |= BLF_MONOSPACED;
    }
    int pen_x = this->width;
    int max_width = 0;
    int max_height = this->height;
    int cwidth = std::max(gc->fixed_width, 1);
    uint glyph_count;
    hb_glyph_info_t *hb_glyph_info = hb_buffer_get_glyph_infos(hb_buf, &glyph_count);
    hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(hb_buf, nullptr);
#if 0
    char *diag_str = MEM_malloc_arrayN<char>(glyph_count * 50, "diag_str");
    hb_buffer_serialize_glyphs(hb_buf,
                               0,
                               glyph_count,
                               diag_str,
                               glyph_count * 50,
                               nullptr,
                               segment_font->hb_font,
                               HB_BUFFER_SERIALIZE_FORMAT_TEXT,
                               HB_BUFFER_SERIALIZE_FLAG_DEFAULT);
    printf("%s\n", diag_str);
    MEM_freeN(diag_str);
#endif
    GlyphCacheBLF *segment_gc = (!gc || segment_font != font) ?
                                    blf_glyph_cache_acquire(segment_font) :
                                    gc;
    size_t str8_offset = 0;
    for (i = 0; i < glyph_count; i++) {
      uint32_t glyph_id = hb_glyph_info[i].codepoint;
      char32_t codepoint = str32[hb_glyph_info[i].cluster];
      GlyphBLF *g = blf_glyph_ensure(segment_font, segment_gc, codepoint, glyph_id);
      if (UNLIKELY(g == nullptr)) {
        /* Skip missing glyphs. */
        continue;
      }
      const int advance = ((font->flags & BLF_MONOSPACED) ?
                               ft_pix_from_int(cwidth) * BLI_wcwidth_safe(codepoint) :
                               glyph_pos[i].x_advance);
      if (g->box_xmin == g->box_xmax) {
        g->box_xmax = g->box_xmin + advance;
      }
      g = blf_glyph_ensure_subpixel(segment_font, segment_gc, g, pen_x);
      rcti bounds = {pen_x + glyph_pos[i].x_offset,
                     pen_x + g->box_xmax + glyph_pos[i].x_offset,
                     glyph_pos[i].y_offset,
                     g->box_ymax + glyph_pos[i].y_offset};
      this->glyphs.append({segment_font, segment_gc, g, bounds, str8_offset});
      str8_offset += BLI_str_utf8_from_unicode_len(codepoint);
      pen_x += advance;
      max_width = pen_x;
      max_height = std::max(g->box_ymax - g->box_ymin, max_height);
    }
    this->width = max_width;
    this->height = max_height;
    if (set_mono) {
      segment_font->flags &= ~BLF_MONOSPACED;
    }
    if (!gc || segment_font != font) {
      blf_glyph_cache_release(segment_font);
    }
  }
  if (hb_buf) {
    hb_buffer_destroy(hb_buf);
  }
}

bool blf_font_otf_feature_supported(FontBLF *font, const char tag[4])
{
  if (!font) {
    return false;
  }
  blf_ensure_face(font);
  hb_face_t *hb_face = hb_ft_face_create_cached(font->face);
  hb_tag_t tag_value = HB_TAG(tag[0], tag[1], tag[2], tag[3]);
  if (hb_ot_layout_language_find_feature(
          hb_face, HB_OT_TAG_GSUB, 0, HB_OT_LAYOUT_DEFAULT_LANGUAGE_INDEX, tag_value, nullptr))
  {
    return true;
  }
  return (hb_ot_layout_language_find_feature(
      hb_face, HB_OT_TAG_GPOS, 0, HB_OT_LAYOUT_DEFAULT_LANGUAGE_INDEX, tag_value, nullptr));
}

void blf_font_otf_feature_set(FontBLF *font, const char tag[4], int value)
{
  if (!font) {
    return;
  }
  hb_tag_t tag_value = HB_TAG(tag[0], tag[1], tag[2], tag[3]);
  for (hb_feature_t &feature : font->features) {
    if (feature.tag == tag_value) {
      feature.value = hb_tag_t(value);
      return;
    }
  }
  font->features.append(
      {tag_value, hb_tag_t(value), HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END});
}

static void blf_font_draw_ex(FontBLF *font,
                             GlyphCacheBLF *gc,
                             const char *str,
                             const size_t str_len,
                             ResultBLF *r_info,
                             const ft_pix pen_y)
{
  if (str_len == 0) {
    /* Early exit, don't do any immediate-mode GPU operations. */
    return;
  }

  blf_batch_draw_begin(font);

  ShapingData text(font, gc, str, str_len);
  for (const Glyph &glyph : text.glyphs) {
    blf_glyph_draw(glyph.font,
                   glyph.gc,
                   glyph.g,
                   ft_pix_to_int_floor(glyph.bounds.xmin),
                   ft_pix_to_int_floor(pen_y + glyph.bounds.ymin));
  }

  blf_batch_draw_end();

  if (r_info) {
    r_info->lines = 1;
    r_info->width = ft_pix_to_int(text.width);
  }
}
void blf_font_draw(FontBLF *font, const char *str, const size_t str_len, ResultBLF *r_info)
{
  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  blf_font_draw_ex(font, gc, str, str_len, r_info, 0);
  blf_glyph_cache_release(font);
}

int blf_font_draw_mono(FontBLF *font,
                       const char *str,
                       const size_t str_len,
                       const int /*cwidth*/,
                       const int tab_columns)
{
  if (str_len == 0 || !str || !str[0]) {
    /* Early exit, don't do any immediate-mode GPU operations. */
    return 0;
  }

  int columns = 0;
  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  blf_batch_draw_begin(font);

  ShapingData text(font, gc, str, str_len);
  for (const Glyph &glyph : text.glyphs) {
    blf_glyph_draw(glyph.font,
                   glyph.gc,
                   glyph.g,
                   ft_pix_to_int_floor(glyph.bounds.xmin),
                   ft_pix_to_int_floor(glyph.bounds.ymin));
    const int col = UNLIKELY(glyph.g->c == '\t') ? (tab_columns - (columns % tab_columns)) :
                                                   BLI_wcwidth_safe(char32_t(glyph.g->c));
    columns += col;
  }

  blf_batch_draw_end();

  blf_glyph_cache_release(font);
  return columns;
}

#ifndef WITH_HEADLESS
void blf_draw_svg_icon(FontBLF *font,
                       const uint icon_id,
                       const float x,
                       const float y,
                       const float size,
                       const float color[4],
                       const float outline_alpha,
                       const bool multicolor,
                       FunctionRef<void(std::string &)> edit_source_cb)
{
  BLI_assert(outline_alpha <= 1.0f); /* Higher values overflow, caller must ensure. */
  blf_font_size(font, size);
  font->pos[0] = int(x);
  font->pos[1] = int(y);
  font->pos[2] = 0;

  if (color != nullptr) {
    rgba_float_to_uchar(font->color, color);
  }

  if (outline_alpha > 0.0f) {
    font->flags |= BLF_SHADOW;
    font->shadow = FontShadowType::Outline;
    font->shadow_x = 0;
    font->shadow_y = 0;
    font->shadow_color[0] = 0;
    font->shadow_color[1] = 0;
    font->shadow_color[2] = 0;
    font->shadow_color[3] = char(outline_alpha * 255.0f);
  }

  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  blf_batch_draw_begin(font);

  GlyphBLF *g = blf_glyph_ensure_icon(gc, icon_id, multicolor, edit_source_cb);
  if (g) {
    blf_glyph_draw(font, gc, g, 0, 0);
  }

  if (outline_alpha > 0) {
    font->flags &= ~BLF_SHADOW;
  }

  blf_batch_draw_end();
  blf_glyph_cache_release(font);
}

Array<uchar> blf_svg_icon_bitmap(FontBLF *font,
                                 const uint icon_id,
                                 const float size,
                                 int *r_width,
                                 int *r_height,
                                 const bool multicolor,
                                 FunctionRef<void(std::string &)> edit_source_cb)
{
  blf_font_size(font, size);
  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  GlyphBLF *g = blf_glyph_ensure_icon(gc, icon_id, multicolor, edit_source_cb);

  if (!g) {
    blf_glyph_cache_release(font);
    *r_width = 0;
    *r_height = 0;
    return {};
  }

  *r_width = g->dims[0];
  *r_height = g->dims[1];
  Array<uchar> bitmap(g->dims[0] * g->dims[1] * 4);

  if (g->num_channels == 4) {
    memcpy(bitmap.data(), g->bitmap, size_t(bitmap.size()));
  }
  else if (g->num_channels == 1) {
    for (int64_t y = 0; y < int64_t(g->dims[1]); y++) {
      for (int64_t x = 0; x < int64_t(g->dims[0]); x++) {
        int64_t offs_in = (y * int64_t(g->pitch)) + x;
        bitmap[int64_t(offs_in * 4)] = g->bitmap[offs_in];
        bitmap[int64_t(offs_in * 4 + 1)] = g->bitmap[offs_in];
        bitmap[int64_t(offs_in * 4 + 2)] = g->bitmap[offs_in];
        bitmap[int64_t(offs_in * 4 + 3)] = g->bitmap[offs_in];
      }
    }
  }
  blf_glyph_cache_release(font);
  return bitmap;
}
#endif /* WITH_HEADLESS */

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Drawing: Buffer
 * \{ */

/**
 * Draw glyph `g` into `buf_info` pixels.
 */
static void blf_glyph_draw_buffer(FontBufInfoBLF *buf_info,
                                  GlyphBLF *g,
                                  const ft_pix pen_x,
                                  const ft_pix pen_y_basis)
{
  const int chx = ft_pix_to_int(pen_x + ft_pix_from_int(g->pos[0]));
  const int chy = ft_pix_to_int(pen_y_basis + ft_pix_from_int(g->dims[1]));

  ft_pix pen_y = (g->pitch < 0) ? (pen_y_basis + ft_pix_from_int(g->dims[1] - g->pos[1])) :
                                  (pen_y_basis - ft_pix_from_int(g->dims[1] - g->pos[1]));

  if ((chx + g->dims[0]) < 0 ||                  /* Out of bounds: left. */
      chx >= buf_info->dims[0] ||                /* Out of bounds: right. */
      (ft_pix_to_int(pen_y) + g->dims[1]) < 0 || /* Out of bounds: bottom. */
      ft_pix_to_int(pen_y) >= buf_info->dims[1]  /* Out of bounds: top. */
  )
  {
    return;
  }

  /* Don't draw beyond the buffer bounds. */
  int width_clip = g->dims[0];
  int height_clip = g->dims[1];
  int yb_start = g->pitch < 0 ? 0 : g->dims[1] - 1;

  if (width_clip + chx > buf_info->dims[0]) {
    width_clip -= chx + width_clip - buf_info->dims[0];
  }
  if (height_clip + ft_pix_to_int(pen_y) > buf_info->dims[1]) {
    height_clip -= ft_pix_to_int(pen_y) + height_clip - buf_info->dims[1];
  }

  /* Clip drawing below the image. */
  if (pen_y < 0) {
    yb_start += (g->pitch < 0) ? -ft_pix_to_int(pen_y) : ft_pix_to_int(pen_y);
    height_clip += ft_pix_to_int(pen_y);
    pen_y = 0;
  }

  /* Avoid conversions in the pixel writing loop. */
  const int pen_y_px = ft_pix_to_int(pen_y);

  const float *b_col_float = buf_info->col_float;
  const uchar *b_col_char = buf_info->col_char;

  if (buf_info->fbuf) {
    int yb = yb_start;
    for (int y = ((chy >= 0) ? 0 : -chy); y < height_clip; y++) {
      const int x_start = (chx >= 0) ? 0 : -chx;
      const uchar *a_ptr = g->bitmap + x_start + (yb * g->pitch);
      const int64_t buf_ofs = (int64_t(buf_info->dims[0]) * (pen_y_px + y) + (chx + x_start)) * 4;
      float *fbuf = buf_info->fbuf + buf_ofs;
      for (int x = x_start; x < width_clip; x++, a_ptr++, fbuf += 4) {
        const char a_byte = *a_ptr;
        if (a_byte) {
          const float a = (a_byte / 255.0f) * b_col_float[3];

          float font_pixel[4];
          font_pixel[0] = b_col_float[0] * a;
          font_pixel[1] = b_col_float[1] * a;
          font_pixel[2] = b_col_float[2] * a;
          font_pixel[3] = a;
          blend_color_mix_float(fbuf, fbuf, font_pixel);
        }
      }

      if (g->pitch < 0) {
        yb++;
      }
      else {
        yb--;
      }
    }
  }

  if (buf_info->cbuf) {
    int yb = yb_start;
    for (int y = ((chy >= 0) ? 0 : -chy); y < height_clip; y++) {
      const int x_start = (chx >= 0) ? 0 : -chx;
      const uchar *a_ptr = g->bitmap + x_start + (yb * g->pitch);
      const int64_t buf_ofs = (int64_t(buf_info->dims[0]) * (pen_y_px + y) + (chx + x_start)) * 4;
      uchar *cbuf = buf_info->cbuf + buf_ofs;
      for (int x = x_start; x < width_clip; x++, a_ptr++, cbuf += 4) {
        const char a_byte = *a_ptr;

        if (a_byte) {
          const float a = (a_byte / 255.0f) * b_col_float[3];

          uchar font_pixel[4];
          font_pixel[0] = b_col_char[0];
          font_pixel[1] = b_col_char[1];
          font_pixel[2] = b_col_char[2];
          font_pixel[3] = unit_float_to_uchar_clamp(a);
          blend_color_mix_byte(cbuf, cbuf, font_pixel);
        }
      }

      if (g->pitch < 0) {
        yb++;
      }
      else {
        yb--;
      }
    }
  }
}

/* Sanity checks are done by BLF_draw_buffer() */
static void blf_font_draw_buffer_ex(FontBLF *font,
                                    GlyphCacheBLF *gc,
                                    const char *str,
                                    const size_t str_len,
                                    const ft_pix pen_y,
                                    ResultBLF *r_info)
{
  ft_pix pen_x = ft_pix_from_int(font->pos[0]);
  ft_pix pen_y_basis = ft_pix_from_int(font->pos[1]) + pen_y;

  /* Buffer specific variables. */
  FontBufInfoBLF *buf_info = &font->buf_info;

  /* Another buffer specific call for color conversion. */

  ShapingData text(font, gc, str, str_len);
  for (const Glyph &glyph : text.glyphs) {
    blf_glyph_draw_buffer(
        buf_info, glyph.g, pen_x + glyph.bounds.xmin, pen_y_basis + glyph.bounds.ymin);
  }

  if (r_info) {
    r_info->lines = 1;
    r_info->width = ft_pix_to_int(text.width);
  }
}

void blf_font_draw_buffer(FontBLF *font, const char *str, const size_t str_len, ResultBLF *r_info)
{
  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  blf_font_draw_buffer_ex(font, gc, str, str_len, 0, r_info);
  blf_glyph_cache_release(font);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Evaluation: Width to String Length
 * \{ */

size_t blf_font_width_to_strlen(
    FontBLF *font, const char *str, const size_t str_len, int width, int *r_width)
{
  if (str_len == 0 || !str || !str[0]) {
    if (r_width) {
      *r_width = 0;
    }
    return 0;
  }

  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  ShapingData text(font, gc, str, str_len);
  blf_glyph_cache_release(font);
  size_t len = strlen(str);
  int w = ft_pix_to_int(text.width);

  for (const Glyph &g : text.glyphs) {
    if (g.bounds.xmax > ft_pix_from_int(width)) {
      len = g.index_utf8;
      w = ft_pix_to_int(g.bounds.xmax);
      break;
    }
  }

  if (r_width) {
    *r_width = w;
  }

  return len;
}

size_t blf_font_width_to_rstrlen(
    FontBLF *font, const char *str, const size_t str_len, int width, int *r_width)
{
  if (str_len == 0 || !str || !str[0]) {
    if (r_width) {
      *r_width = 0;
    }
    return 0;
  }

  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  ShapingData text(font, gc, str, str_len);
  blf_glyph_cache_release(font);
  size_t len = strlen(str);
  int w = ft_pix_to_int(text.width);

  for (const Glyph &g : text.glyphs) {
    if (g.bounds.xmin > (text.width - ft_pix_from_int(width))) {
      len = g.index_utf8;
      w = ft_pix_to_int(text.width - g.bounds.xmax);
      break;
    }
  }

  if (r_width) {
    *r_width = w;
  }
  return len;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Evaluation: Glyph Bound Box with Callback
 * \{ */

static void blf_font_boundbox_ex(FontBLF *font,
                                 GlyphCacheBLF *gc,
                                 const char *str,
                                 const size_t str_len,
                                 rcti *r_box,
                                 ResultBLF *r_info,
                                 ft_pix pen_y)
{
  if (!str[0] || !str_len) {
    return;
  }

  ShapingData text(font, gc, str, str_len);

	r_box->xmin = 0;
  r_box->xmax = ft_pix_to_int(text.width);
  r_box->ymin = ft_pix_to_int(pen_y);
  r_box->ymax = ft_pix_to_int(pen_y + text.height);

  if (r_info) {
    r_info->lines = 1;
    r_info->width = r_box->xmax;
  }
}
void blf_font_boundbox(
    FontBLF *font, const char *str, const size_t str_len, rcti *r_box, ResultBLF *r_info)
{
  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  blf_font_boundbox_ex(font, gc, str, str_len, r_box, r_info, 0);
  blf_glyph_cache_release(font);
}

void blf_font_width_and_height(FontBLF *font,
                               const char *str,
                               const size_t str_len,
                               float *r_width,
                               float *r_height,
                               ResultBLF *r_info)
{
  rcti box;

  if (font->flags & BLF_WORD_WRAP) {
    blf_font_boundbox__wrap(font, str, str_len, &box, r_info);
  }
  else {
    blf_font_boundbox(font, str, str_len, &box, r_info);
  }

  const float xa = (font->flags & BLF_ASPECT) ? font->aspect[0] : 1.0f;
  const float ya = (font->flags & BLF_ASPECT) ? font->aspect[1] : 1.0f;

  *r_width = (float(BLI_rcti_size_x(&box)) * xa);
  *r_height = (float(BLI_rcti_size_y(&box)) * ya);
}

float blf_font_width(FontBLF *font, const char *str, const size_t str_len, ResultBLF *r_info)
{
  rcti box;

  if (font->flags & BLF_WORD_WRAP) {
    blf_font_boundbox__wrap(font, str, str_len, &box, r_info);
  }
  else {
    blf_font_boundbox(font, str, str_len, &box, r_info);
  }

  const float xa = (font->flags & BLF_ASPECT) ? font->aspect[0] : 1.0f;
  return float(BLI_rcti_size_x(&box)) * xa;
}

float blf_font_height(FontBLF *font, const char *str, const size_t str_len, ResultBLF *r_info)
{
  rcti box;

  if (font->flags & BLF_WORD_WRAP) {
    blf_font_boundbox__wrap(font, str, str_len, &box, r_info);
  }
  else {
    blf_font_boundbox(font, str, str_len, &box, r_info);
  }

  const float ya = (font->flags & BLF_ASPECT) ? font->aspect[1] : 1.0f;
  return float(BLI_rcti_size_y(&box)) * ya;
}

float blf_font_fixed_width(FontBLF *font)
{
  const GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  float width = (gc) ? float(gc->fixed_width) : font->size / 2.0f;
  blf_glyph_cache_release(font);
  return width;
}

int blf_font_glyph_advance(FontBLF *font, const char *str)
{
  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  const uint charcode = BLI_str_utf8_as_unicode_safe(str);
  const GlyphBLF *g = blf_glyph_ensure(font, gc, charcode);

  if (UNLIKELY(g == nullptr)) {
    blf_glyph_cache_release(font);
    return 0;
  }

  const int glyph_advance = ft_pix_to_int(g->advance_x);

  blf_glyph_cache_release(font);
  return glyph_advance;
}

void blf_font_boundbox_foreach_glyph(FontBLF *font,
                                     const char *str,
                                     const size_t str_len,
                                     BLF_GlyphBoundsFn user_fn,
                                     void *user_data)
{
  if (str_len == 0 || str[0] == 0) {
    /* Early exit. */
    return;
  }

  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);

  ShapingData text(font, gc, str, str_len);
  for (const Glyph &glyph : text.glyphs) {
    if (glyph.g->advance_x <= 0) {
      /* Ignore combining marks. */
      continue;
    }
    rcti bounds = glyph.integer_bounds();
    if (user_fn(str, glyph.index_utf8, &bounds, user_data) == false) {
      break;
    }
  }

  blf_glyph_cache_release(font);
}

size_t blf_str_offset_from_cursor_position(FontBLF *font,
                                           const char *str,
                                           size_t str_len,
                                           int location_x)
{
  /* Do not early exit if location_x <= 0, as this can result in an incorrect
   * offset for RTL text. Instead of offset of character responsible for first
   * glyph you'd get offset of first character, which could be the last glyph. */
  if (!str || !str[0] || !str_len) {
    return 0;
  }

  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  ShapingData text(font, gc, str, strlen(str));
  blf_glyph_cache_release(font);

  for (const Glyph &glyph : text.glyphs) {
    if (ft_pix_from_int(location_x) < ((glyph.bounds.xmin + glyph.bounds.xmax) / 2)) {
      return glyph.index_utf8;
    }
  }

  return text.glyphs.is_empty() ? 0 : text.glyphs.last().index_utf8 + 1;
}

struct StrOffsetToGlyphBounds_Data {
  size_t str_offset;
  rcti bounds;
};

static bool blf_str_offset_foreach_glyph(const char * /*str*/,
                                         const size_t str_step_ofs,
                                         const rcti *bounds,
                                         void *user_data)
{
  StrOffsetToGlyphBounds_Data *data = static_cast<StrOffsetToGlyphBounds_Data *>(user_data);
  if (data->str_offset == str_step_ofs) {
    data->bounds = *bounds;
    return false;
  }
  return true;
}

void blf_str_offset_to_glyph_bounds(FontBLF *font,
                                    const char *str,
                                    size_t str_offset,
                                    rcti *r_glyph_bounds)
{
  if (!str || !str[0]) {
    std::memset(r_glyph_bounds, 0, sizeof(rcti));
    return;
  }

  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  ShapingData text(font, gc, str, strlen(str));
  blf_glyph_cache_release(font);
  for (const Glyph &g : text.glyphs) {
    if (g.index_utf8 >= str_offset) {
      *r_glyph_bounds = g.integer_bounds();
      return;
    }
  }
  std::memset(r_glyph_bounds, 0, sizeof(rcti));
}

int blf_str_offset_to_cursor(FontBLF *font,
                             const char *str,
                             const size_t str_len,
                             const size_t str_offset,
                             const int cursor_width)
{
  const ft_pix half_width = ft_pix_from_int(cursor_width) / 2;

  if (!str || !str[0]) {
    return ft_pix_to_int(-half_width);
  }

  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);
  ShapingData text(font, gc, str, str_len);
  blf_glyph_cache_release(font);
  int64_t index = 0;
  for (const Glyph &glyph : text.glyphs) {
    if (glyph.index_utf8 >= str_offset) {
      break;
    }
    index++;
  }
  ft_pix cursor = 0;

  /* Right edge of the previous character, if available. */
  rcti prev = {0};
  if (index > 0) {
    prev = text.glyphs[index - 1].bounds;
  }

  /* Left edge of the next character, if available. */
  rcti next = {0};
  if (index <= (text.glyphs.size() - 1)) {
    next = text.glyphs[index].bounds;
  }

  if ((prev.xmax == prev.xmin) && next.xmax) {
    /* Nothing (or a space) to the left, so align to right character. */
    cursor = next.xmin - half_width;
  }
  else if ((prev.xmax != prev.xmin) && !next.xmax) {
    /* End of string, so align to last character. */
    cursor = prev.xmax + half_width;
  }
  else if (prev.xmax && next.xmax) {
    /* Between two characters, so use the center. */
    if (next.xmin >= prev.xmax || next.xmin == next.xmax) {
      cursor = (prev.xmax + next.xmin) / 2 - half_width;
    }
    else {
      /* A nicer center if reversed order - RTL. */
      cursor = (next.xmax + prev.xmin) / 2 - half_width;
    }
  }
  else {
    cursor = text.width + half_width;
  }

  return ft_pix_to_int(cursor);
}

Vector<Bounds<int>> blf_str_selection_boxes(
    FontBLF *font, const char *str, size_t str_len, size_t sel_start, size_t sel_length)
{
  Vector<Bounds<int>> boxes;
  const int start = blf_str_offset_to_cursor(font, str, str_len, sel_start, 0);
  const int end = blf_str_offset_to_cursor(font, str, str_len, sel_start + sel_length, 0);
  boxes.append(Bounds(start, end));
  return boxes;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Evaluation: Word-Wrap with Callback
 * \{ */

/**
 * Generic function to add word-wrap support for other existing functions.
 *
 * Wraps on spaces and respects newlines.
 * Intentionally ignores non-unix newlines, tabs and more advanced text formatting.
 *
 * \note If we want rich text - we better have a higher level API to handle that
 * (color, bold, switching fonts... etc).
 */
static void blf_font_wrap_apply(FontBLF *font,
                                const char *str,
                                const size_t str_len,
                                const int max_pixel_width,
                                BLFWrapMode mode,
                                ResultBLF *r_info,
                                void (*callback)(FontBLF *font,
                                                 GlyphCacheBLF *gc,
                                                 const char *str,
                                                 const size_t str_len,
                                                 ft_pix pen_y,
                                                 void *userdata),
                                void *userdata)
{
  uint codepoint = 0;
  uint codepoint_prev = 0;
  ft_pix pen_x = 0;
  ft_pix pen_y = 0;
  size_t i = 0;
  int lines = 0;
  ft_pix pen_x_next = 0;

  /* Size of characters not shown at the end of the wrapped line. */
  size_t clip_bytes = 0;

  ft_pix line_height = blf_font_height_max_ft_pix(font);

  GlyphCacheBLF *gc = blf_glyph_cache_acquire(font);

  struct WordWrapVars {
    ft_pix wrap_width;
    size_t start, last[2];
  } wrap = {max_pixel_width != -1 ? ft_pix_from_int(max_pixel_width) : INT_MAX, 0, {0, 0}};

  // printf("%s wrapping (%d, %d) `%s`:\n", __func__, str_len, strlen(str), str);
  while ((i < str_len) && str[i]) {

    /* Wrap variables. */
    const size_t i_curr = i;
    bool do_draw = false;

    codepoint_prev = codepoint;
    codepoint = BLI_str_utf8_as_unicode_step_safe(str, str_len, &i);
    GlyphBLF *g = blf_glyph_ensure(font, gc, codepoint);
    const ft_pix advance_x = g ? g->advance_x : 0;

    /**
     * Implementation Detail (UTF8).
     *
     * Take care with single byte offsets here,
     * since this is UTF8 we can't be sure a single byte is a single character.
     *
     * This is _only_ done when we know for sure the character is ASCII (newline or a space).
     */
    pen_x_next = pen_x + advance_x;
    /* Ensure at least one character in the wrapped line. */
    const bool overflows = pen_x_next >= wrap.wrap_width && pen_x != 0;

    if (UNLIKELY(overflows && (wrap.start != wrap.last[0]))) {
      do_draw = true;
    }
    else if (UNLIKELY((int(mode) & int(BLFWrapMode::HardLimit)) && overflows && (advance_x != 0)))
    {
      wrap.last[0] = i_curr;
      wrap.last[1] = i_curr;
      do_draw = true;
      clip_bytes = 0;
    }
    else if (UNLIKELY(((i < str_len) && str[i]) == 0)) {
      /* Need check here for trailing newline, else we draw it. */
      wrap.last[0] = i + ((codepoint != '\n') ? 1 : 0);
      wrap.last[1] = i;
      do_draw = true;
      clip_bytes = 0;
    }
    else if (UNLIKELY(codepoint == '\n')) {
      wrap.last[0] = i_curr + 1;
      wrap.last[1] = i;
      do_draw = true;
      clip_bytes = 1;
    }
    else if (UNLIKELY(((int(mode) & int(BLFWrapMode::Minimal)) == int(BLFWrapMode::Minimal)) &&
                      codepoint != ' ' && codepoint_prev == ' '))
    {
      wrap.last[0] = i_curr;
      wrap.last[1] = i_curr;
      clip_bytes = 1;
    }
    else if (UNLIKELY(int(mode) & int(BLFWrapMode::Path))) {
      if (ELEM(codepoint, SEP, ' ', '?', '&', '=')) {
        /* Break and leave at the end of line. */
        wrap.last[0] = i;
        wrap.last[1] = i;
        clip_bytes = 0;
      }
      else if (ELEM(codepoint, '-', '_', '.', '%')) {
        /* Break and move to the next line. */
        wrap.last[0] = i_curr;
        wrap.last[1] = i_curr;
        clip_bytes = 0;
      }
    }
    else if (UNLIKELY((int(mode) & int(BLFWrapMode::Typographical)) &&
                      !BLI_str_utf32_char_is_breaking_space(codepoint) &&
                      BLI_str_utf32_char_is_breaking_space(codepoint_prev)))
    {
      /* Optional break after space, removing it. */
      wrap.last[0] = i_curr;
      wrap.last[1] = i_curr;
      clip_bytes = BLI_str_utf8_from_unicode_len(codepoint_prev);
    }
    else if (UNLIKELY((int(mode) & int(BLFWrapMode::Typographical)) &&
                      BLI_str_utf32_char_is_optional_break_after(codepoint, codepoint_prev)))
    {
      /* Optional break after various characters, keeping it. */
      wrap.last[0] = i;
      wrap.last[1] = i;
      clip_bytes = 0;
    }
    else if (UNLIKELY((int(mode) & int(BLFWrapMode::Typographical)) &&
                      BLI_str_utf32_char_is_optional_break_before(codepoint, codepoint_prev)))
    {
      /* Optional break before various characters. */
      wrap.last[0] = i_curr;
      wrap.last[1] = i_curr;
      clip_bytes = 0;
    }

    if (UNLIKELY(do_draw)) {
#if 0
      printf("(%03d..%03d)  `%.*s`\n",
             wrap.start,
             wrap.last[0],
             (wrap.last[0] - wrap.start) - 1,
             &str[wrap.start]);
#endif

      callback(font,
               gc,
               &str[wrap.start],
               std::min(wrap.last[0] - wrap.start - clip_bytes, str_len - wrap.start),
               pen_y,
               userdata);
      wrap.start = wrap.last[0];
      i = wrap.last[1];
      pen_x = 0;
      pen_y -= line_height;
      lines += 1;
      continue;
    }

    pen_x = pen_x_next;
  }

  // printf("done! lines: %d, width, %d\n", lines, pen_x_next);

  if (r_info) {
    r_info->lines = lines;
    /* Width of last line only (with wrapped lines). */
    r_info->width = ft_pix_to_int(pen_x_next);
  }

  blf_glyph_cache_release(font);
}

/** Utility for #blf_font_draw__wrap. */
static void blf_font_draw__wrap_cb(FontBLF *font,
                                   GlyphCacheBLF *gc,
                                   const char *str,
                                   const size_t str_len,
                                   ft_pix pen_y,
                                   void * /*userdata*/)
{
  blf_font_draw_ex(font, gc, str, str_len, nullptr, pen_y);
}
void blf_font_draw__wrap(FontBLF *font, const char *str, const size_t str_len, ResultBLF *r_info)
{
  blf_font_wrap_apply(font,
                      str,
                      str_len,
                      font->wrap_width,
                      font->wrap_mode,
                      r_info,
                      blf_font_draw__wrap_cb,
                      nullptr);
}

/** Utility for #blf_font_boundbox__wrap. */
static void blf_font_boundbox_wrap_cb(FontBLF *font,
                                      GlyphCacheBLF *gc,
                                      const char *str,
                                      const size_t str_len,
                                      ft_pix pen_y,
                                      void *userdata)
{
  rcti *box = static_cast<rcti *>(userdata);
  rcti box_single;

  blf_font_boundbox_ex(font, gc, str, str_len, &box_single, nullptr, pen_y);
  BLI_rcti_union(box, &box_single);
}
void blf_font_boundbox__wrap(
    FontBLF *font, const char *str, const size_t str_len, rcti *r_box, ResultBLF *r_info)
{
  r_box->xmin = 32000;
  r_box->xmax = -32000;
  r_box->ymin = 32000;
  r_box->ymax = -32000;

  blf_font_wrap_apply(font,
                      str,
                      str_len,
                      font->wrap_width,
                      font->wrap_mode,
                      r_info,
                      blf_font_boundbox_wrap_cb,
                      r_box);
}

/** Utility for  #blf_font_draw_buffer__wrap. */
static void blf_font_draw_buffer__wrap_cb(FontBLF *font,
                                          GlyphCacheBLF *gc,
                                          const char *str,
                                          const size_t str_len,
                                          const ft_pix pen_y,
                                          void * /*userdata*/)
{
  blf_font_draw_buffer_ex(font, gc, str, str_len, pen_y, nullptr);
}
void blf_font_draw_buffer__wrap(FontBLF *font,
                                const char *str,
                                const size_t str_len,
                                ResultBLF *r_info)
{
  blf_font_wrap_apply(font,
                      str,
                      str_len,
                      font->wrap_width,
                      font->wrap_mode,
                      r_info,
                      blf_font_draw_buffer__wrap_cb,
                      nullptr);
}

/** Wrap a StringRef. */
static void blf_font_string_wrap_cb(FontBLF * /*font*/,
                                    GlyphCacheBLF * /*gc*/,
                                    const char *str,
                                    const size_t str_len,
                                    const ft_pix /*pen_y*/,
                                    void *str_list_ptr)
{
  Vector<StringRef> *list = static_cast<Vector<StringRef> *>(str_list_ptr);
  StringRef line(str, str + str_len);
  list->append(line);
}

Vector<StringRef> blf_font_string_wrap(FontBLF *font,
                                       StringRef str,
                                       int max_pixel_width,
                                       BLFWrapMode mode)
{
  Vector<StringRef> list;
  blf_font_wrap_apply(font,
                      str.data(),
                      size_t(str.size()),
                      max_pixel_width,
                      mode,
                      nullptr,
                      blf_font_string_wrap_cb,
                      &list);
  return list;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Font Query: Attributes
 * \{ */

static ft_pix blf_font_height_max_ft_pix(FontBLF *font)
{
  std::lock_guard lock(ft_cache_size_mutex);
  blf_ensure_size(font);
  /* #Metrics::height is rounded to pixel. Force minimum of one pixel. */
  return std::max(ft_pix(font->ft_size->metrics.height), ft_pix_from_int(1));
}

int blf_font_height_max(FontBLF *font)
{
  return ft_pix_to_int(blf_font_height_max_ft_pix(font));
}

static ft_pix blf_font_width_max_ft_pix(FontBLF *font)
{
  std::lock_guard lock(ft_cache_size_mutex);
  blf_ensure_size(font);
  /* #Metrics::max_advance is rounded to pixel. Force minimum of one pixel. */
  return std::max(ft_pix(font->ft_size->metrics.max_advance), ft_pix_from_int(1));
}

int blf_font_width_max(FontBLF *font)
{
  return ft_pix_to_int(blf_font_width_max_ft_pix(font));
}

int blf_font_descender(FontBLF *font)
{
  std::lock_guard lock(ft_cache_size_mutex);
  blf_ensure_size(font);
  return ft_pix_to_int(ft_pix(font->ft_size->metrics.descender));
}

int blf_font_ascender(FontBLF *font)
{
  std::lock_guard lock(ft_cache_size_mutex);
  blf_ensure_size(font);
  return ft_pix_to_int(ft_pix(font->ft_size->metrics.ascender));
}

bool blf_font_bounds_max(FontBLF *font, rctf *r_bounds)
{
  if (!blf_ensure_face(font)) {
    return false;
  }

  r_bounds->xmin = float(font->face->bbox.xMin) / float(font->face->units_per_EM) * font->size;
  r_bounds->xmax = float(font->face->bbox.xMax) / float(font->face->units_per_EM) * font->size;
  r_bounds->ymin = float(font->face->bbox.yMin) / float(font->face->units_per_EM) * font->size;
  r_bounds->ymax = float(font->face->bbox.yMax) / float(font->face->units_per_EM) * font->size;
  return true;
}

char *blf_display_name(FontBLF *font)
{
  if (!blf_ensure_face(font) || !font->face->family_name) {
    return nullptr;
  }
  return BLI_sprintfN("%s %s", font->face->family_name, font->face->style_name);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Font Subsystem Init/Exit
 * \{ */

int blf_font_init()
{
  memset(&g_batch, 0, sizeof(g_batch));
  int err = FT_Init_FreeType(&ft_lib);
  if (err == FT_Err_Ok) {
    /* Create a FreeType cache manager. */
    err = FTC_Manager_New(ft_lib,
                          BLF_CACHE_MAX_FACES,
                          BLF_CACHE_MAX_SIZES,
                          BLF_CACHE_BYTES,
                          blf_cache_face_requester,
                          nullptr,
                          &ftc_manager);
    if (err == FT_Err_Ok) {
      /* Create a character-map cache to speed up glyph index lookups. */
      err = FTC_CMapCache_New(ftc_manager, &ftc_charmap_cache);
    }
  }
  return err;
}

void blf_font_exit()
{
  if (ftc_manager) {
    FTC_Manager_Done(ftc_manager);
  }
  if (ft_lib) {
    FT_Done_FreeType(ft_lib);
  }
  blf_batch_draw_exit();
}

void BLF_cache_flush_set_fn(void (*cache_flush_fn)())
{
  blf_draw_cache_flush = cache_flush_fn;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Font New/Free
 * \{ */

static void blf_font_fill(FontBLF *font)
{
  font->aspect[0] = 1.0f;
  font->aspect[1] = 1.0f;
  font->aspect[2] = 1.0f;
  font->pos[0] = 0;
  font->pos[1] = 0;
  font->angle = 0.0f;

#ifdef WITH_HARFBUZZ
  font->hb_font = NULL;
#endif

  /* Use an easily identifiable bright color (yellow)
   * so its clear when #BLF_color calls are missing. */
  font->color[0] = 255;
  font->color[1] = 255;
  font->color[2] = 0;
  font->color[3] = 255;

  font->clip_rec.xmin = 0;
  font->clip_rec.xmax = 0;
  font->clip_rec.ymin = 0;
  font->clip_rec.ymax = 0;
  font->flags = BLF_NONE;
  font->size = 0;
  font->char_weight = 400;
  font->char_slant = 0.0f;
  font->char_width = 1.0f;
  font->char_spacing = 0.0f;

  font->tex_size_max = -1;

  font->buf_info.fbuf = nullptr;
  font->buf_info.cbuf = nullptr;
  font->buf_info.dims[0] = 0;
  font->buf_info.dims[1] = 0;
  font->buf_info.col_init[0] = 0;
  font->buf_info.col_init[1] = 0;
  font->buf_info.col_init[2] = 0;
  font->buf_info.col_init[3] = 0;
}

/**
 * NOTE(@Harley): that the data the following function creates is not yet used.
 * But do not remove it as it will be used in the near future.
 */
static void blf_font_metrics(FT_Face face, FontMetrics *metrics)
{
  /* Members with non-zero defaults. */
  metrics->weight = 400;
  metrics->width = 1.0f;

  const TT_OS2 *os2_table = static_cast<const TT_OS2 *>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
  if (os2_table) {
    /* The default (resting) font weight. */
    if (os2_table->usWeightClass >= 1 && os2_table->usWeightClass <= 1000) {
      metrics->weight = short(os2_table->usWeightClass);
    }

    /* Width value is one of integers 1-9 with known values. */
    if (os2_table->usWidthClass >= 1 && os2_table->usWidthClass <= 9) {
      switch (os2_table->usWidthClass) {
        case 1:
          metrics->width = 0.5f;
          break;
        case 2:
          metrics->width = 0.625f;
          break;
        case 3:
          metrics->width = 0.75f;
          break;
        case 4:
          metrics->width = 0.875f;
          break;
        case 5:
          metrics->width = 1.0f;
          break;
        case 6:
          metrics->width = 1.125f;
          break;
        case 7:
          metrics->width = 1.25f;
          break;
        case 8:
          metrics->width = 1.5f;
          break;
        case 9:
          metrics->width = 2.0f;
          break;
      }
    }

    metrics->strikeout_position = short(os2_table->yStrikeoutPosition);
    metrics->strikeout_thickness = short(os2_table->yStrikeoutSize);
    metrics->subscript_size = short(os2_table->ySubscriptYSize);
    metrics->subscript_xoffset = short(os2_table->ySubscriptXOffset);
    metrics->subscript_yoffset = short(os2_table->ySubscriptYOffset);
    metrics->superscript_size = short(os2_table->ySuperscriptYSize);
    metrics->superscript_xoffset = short(os2_table->ySuperscriptXOffset);
    metrics->superscript_yoffset = short(os2_table->ySuperscriptYOffset);
    metrics->family_class = short(os2_table->sFamilyClass);
    metrics->selection_flags = short(os2_table->fsSelection);
    metrics->first_charindex = short(os2_table->usFirstCharIndex);
    metrics->last_charindex = short(os2_table->usLastCharIndex);
    if (os2_table->version > 1) {
      metrics->cap_height = short(os2_table->sCapHeight);
      metrics->x_height = short(os2_table->sxHeight);
    }
  }

  /* The Post table usually contains a slant value, but in counter-clockwise degrees. */
  const TT_Postscript *post_table = static_cast<const TT_Postscript *>(
      FT_Get_Sfnt_Table(face, FT_SFNT_POST));
  if (post_table) {
    if (post_table->italicAngle != 0) {
      metrics->slant = float(post_table->italicAngle) / -65536.0f;
    }
  }

  /* Metrics copied from those gathered by FreeType. */
  metrics->units_per_EM = short(face->units_per_EM);
  metrics->ascender = short(face->ascender);
  metrics->descender = short(face->descender);
  metrics->line_height = short(face->height);
  metrics->max_advance_width = short(face->max_advance_width);
  metrics->max_advance_height = short(face->max_advance_height);
  metrics->underline_position = short(face->underline_position);
  metrics->underline_thickness = short(face->underline_thickness);
  metrics->num_glyphs = int(face->num_glyphs);

  if (metrics->cap_height == 0) {
    /* Calculate or guess cap height if it is not set in the font. */
    FT_UInt gi = FT_Get_Char_Index(face, uint('H'));
    if (gi && FT_Load_Glyph(face, gi, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP) == FT_Err_Ok) {
      metrics->cap_height = short(face->glyph->metrics.height);
    }
    else {
      metrics->cap_height = short(float(metrics->units_per_EM) * 0.7f);
    }
  }

  if (metrics->x_height == 0) {
    /* Calculate or guess x-height if it is not set in the font. */
    FT_UInt gi = FT_Get_Char_Index(face, uint('x'));
    if (gi && FT_Load_Glyph(face, gi, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP) == FT_Err_Ok) {
      metrics->x_height = short(face->glyph->metrics.height);
    }
    else {
      metrics->x_height = short(float(metrics->units_per_EM) * 0.5f);
    }
  }

  FT_UInt gi = FT_Get_Char_Index(face, uint('o'));
  if (gi && FT_Load_Glyph(face, gi, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP) == FT_Err_Ok) {
    metrics->o_proportion = float(face->glyph->metrics.width) / float(face->glyph->metrics.height);
  }

  if (metrics->ascender == 0) {
    /* Set a sane value for ascender if not set in the font. */
    metrics->ascender = short(float(metrics->units_per_EM) * 0.8f);
  }

  if (metrics->descender == 0) {
    /* Set a sane value for descender if not set in the font. */
    metrics->descender = metrics->ascender - metrics->units_per_EM;
  }

  if (metrics->weight == 400 && face->style_flags & FT_STYLE_FLAG_BOLD) {
    /* Normal weight yet this is an bold font, so set a sane weight value. */
    metrics->weight = 700;
  }

  if (metrics->slant == 0.0f && face->style_flags & FT_STYLE_FLAG_ITALIC) {
    /* No slant yet this is an italic font, so set a sane slant value. */
    metrics->slant = 8.0f;
  }

  if (metrics->underline_position == 0) {
    metrics->underline_position = short(float(metrics->units_per_EM) * -0.2f);
  }

  if (metrics->underline_thickness == 0) {
    metrics->underline_thickness = short(float(metrics->units_per_EM) * 0.07f);
  }

  if (metrics->strikeout_position == 0) {
    metrics->strikeout_position = short(float(metrics->x_height) * 0.6f);
  }

  if (metrics->strikeout_thickness == 0) {
    metrics->strikeout_thickness = metrics->underline_thickness;
  }

  if (metrics->subscript_size == 0) {
    metrics->subscript_size = short(float(metrics->units_per_EM) * 0.6f);
  }

  if (metrics->subscript_yoffset == 0) {
    metrics->subscript_yoffset = short(float(metrics->units_per_EM) * 0.075f);
  }

  if (metrics->superscript_size == 0) {
    metrics->superscript_size = short(float(metrics->units_per_EM) * 0.6f);
  }

  if (metrics->superscript_yoffset == 0) {
    metrics->superscript_yoffset = short(float(metrics->units_per_EM) * 0.35f);
  }

  metrics->valid = true;
}

/**
 * Extra FontBLF setup needed after it gets a Face. Called from
 * both blf_ensure_face and from the blf_cache_face_requester callback.
 */
static bool blf_setup_face(FontBLF *font)
{
  font->face_flags = font->face->face_flags;

  if (FT_HAS_MULTIPLE_MASTERS(font) && !font->variations) {
    FT_Get_MM_Var(font->face, &(font->variations));
  }

  if (!font->metrics.valid) {
    blf_font_metrics(font->face, &font->metrics);
    font->char_weight = font->metrics.weight;
    font->char_slant = font->metrics.slant;
    font->char_width = font->metrics.width;
    font->char_spacing = font->metrics.spacing;
  }

  if (FT_IS_FIXED_WIDTH(font)) {
    font->flags |= BLF_MONOSPACED;
  }

  return true;
}

bool blf_ensure_face(FontBLF *font)
{
  if (font->face) {
    return true;
  }

  if (font->flags & BLF_BAD_FONT) {
    return false;
  }

  FT_Error err = FTC_Manager_LookupFace(ftc_manager, font, &font->face);

  if (err) {
    if (ELEM(err, FT_Err_Unknown_File_Format, FT_Err_Unimplemented_Feature)) {
      printf("Format of this font file is not supported\n");
    }
    else {
      printf("Error encountered while opening font file\n");
    }
    font->flags |= BLF_BAD_FONT;
    return false;
  }

  if (font->face && !(font->face->face_flags & FT_FACE_FLAG_SCALABLE)) {
    printf("Font is not scalable\n");
    return false;
  }

  err = FT_Select_Charmap(font->face, FT_ENCODING_UNICODE);
  if (err) {
    err = FT_Select_Charmap(font->face, FT_ENCODING_APPLE_ROMAN);
  }
  if (err && font->face->num_charmaps > 0) {
    err = FT_Select_Charmap(font->face, font->face->charmaps[0]->encoding);
  }
  if (err) {
    printf("Can't set a character map!\n");
    font->flags |= BLF_BAD_FONT;
    return false;
  }

  if (font->filepath) {
    char *mfile = blf_dir_metrics_search(font->filepath);
    if (mfile) {
      err = FT_Attach_File(font->face, mfile);
      if (err) {
        fprintf(stderr,
                "FT_Attach_File failed to load '%s' with error %d\n",
                font->filepath,
                int(err));
      }
      MEM_delete(mfile);
    }
  }

  /* Setup Font details that require having a Face. */
  return blf_setup_face(font);
}

struct FaceDetails {
  char filename[50];
  uint coverage1;
  uint coverage2;
  uint coverage3;
  uint coverage4;
};

/* Details about the fallback fonts we ship, so that we can load only when needed. */
static const FaceDetails static_face_details[] = {
    {"Noto Sans CJK Regular.woff2",
     TT_UCR_HANGUL_JAMO,
     TT_UCR_CJK_SYMBOLS | TT_UCR_HIRAGANA | TT_UCR_KATAKANA | TT_UCR_BOPOMOFO | TT_UCR_CJK_MISC |
         TT_UCR_ENCLOSED_CJK_LETTERS_MONTHS | TT_UCR_CJK_COMPATIBILITY |
         TT_UCR_CJK_UNIFIED_IDEOGRAPHS | TT_UCR_CJK_COMPATIBILITY_IDEOGRAPHS |
         TT_UCR_HANGUL_COMPATIBILITY_JAMO | TT_UCR_HANGUL,
     TT_UCR_CJK_COMPATIBILITY_FORMS | TT_UCR_HALFWIDTH_FULLWIDTH_FORMS,
     0},
    {"NotoEmoji-VariableFont_wght.woff2", 0x80000003L, 0x241E4ACL, 0x14000000L, 0x4000000L},
    {"NotoSansArabic-VariableFont_wdth,wght.woff2",
     TT_UCR_ARABIC,
     uint(TT_UCR_ARABIC_PRESENTATION_FORMS_A),
     TT_UCR_ARABIC_PRESENTATION_FORMS_B,
     0},
    {"NotoSansArmenian-VariableFont_wdth,wght.woff2", TT_UCR_ARMENIAN, 0, 0, 0},
    {"NotoSansBengali-VariableFont_wdth,wght.woff2", TT_UCR_BENGALI, 0, 0, 0},
    {"NotoSansDevanagari-Regular.woff2", TT_UCR_DEVANAGARI, 0, 0, 0},
    {"NotoSansEthiopic-Regular.woff2", 0, 0, TT_UCR_ETHIOPIC, 0},
    {"NotoSansGeorgian-VariableFont_wdth,wght.woff2", TT_UCR_GEORGIAN, 0, 0, 0},
    {"NotoSansGujarati-Regular.woff2", TT_UCR_GUJARATI, 0, 0, 0},
    {"NotoSansGurmukhi-VariableFont_wdth,wght.woff2", TT_UCR_GURMUKHI, 0, 0, 0},
    {"NotoSansHebrew-Regular.woff2", TT_UCR_HEBREW, 0, 0, 0},
    {"NotoSansJavanese-Regular.woff2", 0x80000003L, 0x2000L, 0, 0},
    {"NotoSansKannada-VariableFont_wdth,wght.woff2", TT_UCR_KANNADA, 0, 0, 0},
    {"NotoSansKhmer-VariableFont_wdth,wght.woff2", 0, 0, TT_UCR_KHMER, 0},
    {"NotoSansMalayalam-VariableFont_wdth,wght.woff2", TT_UCR_MALAYALAM, 0, 0, 0},
    {"NotoSansMath-Regular.woff2", 0, TT_UCR_MATHEMATICAL_OPERATORS, 0, 0},
    {"NotoSansMyanmar-Regular.woff2", 0, 0, TT_UCR_MYANMAR, 0},
    {"NotoSansSymbols-VariableFont_wght.woff2", 0x3L, 0x200E4B4L, 0, 0},
    {"NotoSansSymbols2-Regular.woff2", 0x80000003L, 0x200E3E4L, 0x40020L, 0x580A048L},
    {"NotoSansTamil-VariableFont_wdth,wght.woff2", TT_UCR_TAMIL, 0, 0, 0},
    {"NotoSansTelugu-VariableFont_wdth,wght.woff2", TT_UCR_TELUGU, 0, 0, 0},
    {"NotoSansThai-VariableFont_wdth,wght.woff2", TT_UCR_THAI, 0, 0, 0},
};

/**
 * Create a new font from filename OR memory pointer.
 */
static FontBLF *blf_font_new_impl(const char *filepath,
                                  const char *mem_name,
                                  const uchar *mem,
                                  const size_t mem_size)
{
  FontBLF *font = MEM_new<FontBLF>(__func__);

  font->mem_name = mem_name ? BLI_strdup(mem_name) : nullptr;
  font->filepath = filepath ? BLI_strdup(filepath) : nullptr;
  if (mem) {
    font->mem = mem;
    font->mem_size = mem_size;
  }
  blf_font_fill(font);
  font->ft_lib = ft_lib;

  /* Defaults for consistent behavior. Some often overwritten by user preferences. */
  blf_font_otf_feature_set(font, "kern", 1); /* Kerning. */
  blf_font_otf_feature_set(font, "locl", 1); /* Localized Forms. */
  blf_font_otf_feature_set(font, "liga", 1); /* Standard Ligatures. */
  blf_font_otf_feature_set(font, "case", 1); /* Case Sensitive Forms. */
  blf_font_otf_feature_set(font, "calt", 1); /* Contextual Alternates. */
  blf_font_otf_feature_set(font, "tnum", 1); /* Tabular Numbers. */
  blf_font_otf_feature_set(font, "dlig", 0); /* Discretionary Ligatures. */
  blf_font_otf_feature_set(font, "hlig", 0); /* Historical Ligatures. */
  blf_font_otf_feature_set(font, "zero", 0); /* Slashed Zero. */
  blf_font_otf_feature_set(font, "salt", 0); /* Stylistic Alternates. */

  /* If we have static details about this font file, we don't have to load the Face yet. */
  bool face_needed = true;

  if (font->filepath) {
    const char *filename = BLI_path_basename(font->filepath);
    for (int i = 0; i < int(ARRAY_SIZE(static_face_details)); i++) {
      if (BLI_path_cmp(static_face_details[i].filename, filename) == 0) {
        const FaceDetails *static_details = &static_face_details[i];
        font->unicode_ranges[0] = static_details->coverage1;
        font->unicode_ranges[1] = static_details->coverage2;
        font->unicode_ranges[2] = static_details->coverage3;
        font->unicode_ranges[3] = static_details->coverage4;
        face_needed = false;
        break;
      }
    }
    if (STREQ(filename, BLF_DEFAULT_PROPORTIONAL_FONT)) {
      blf_font_otf_feature_set(font, "ss01", 1); /* Open Digits. */
      blf_font_otf_feature_set(font, "ss04", 1); /* Disambiguation w/o zero. */
    }
  }

  if (face_needed) {
    if (!blf_ensure_face(font)) {
      blf_font_free(font);
      return nullptr;
    }

    /* Save TrueType table with bits to quickly test most unicode block coverage. */
    const TT_OS2 *os2_table = static_cast<const TT_OS2 *>(
        FT_Get_Sfnt_Table(font->face, FT_SFNT_OS2));
    if (os2_table) {
      font->unicode_ranges[0] = uint(os2_table->ulUnicodeRange1);
      font->unicode_ranges[1] = uint(os2_table->ulUnicodeRange2);
      font->unicode_ranges[2] = uint(os2_table->ulUnicodeRange3);
      font->unicode_ranges[3] = uint(os2_table->ulUnicodeRange4);
    }
  }

  /* Detect "Last resort" fonts. They have everything. Usually except last 5 bits. */
  if (font->unicode_ranges[0] == 0xffffffffU && font->unicode_ranges[1] == 0xffffffffU &&
      font->unicode_ranges[2] == 0xffffffffU && font->unicode_ranges[3] >= 0x7FFFFFFU)
  {
    font->flags |= BLF_LAST_RESORT;
  }

  return font;
}

FontBLF *blf_font_new_from_filepath(const char *filepath)
{
  return blf_font_new_impl(filepath, nullptr, nullptr, 0);
}

FontBLF *blf_font_new_from_mem(const char *mem_name, const uchar *mem, const size_t mem_size)
{
  return blf_font_new_impl(nullptr, mem_name, mem, mem_size);
}

void blf_font_attach_from_mem(FontBLF *font, const uchar *mem, const size_t mem_size)
{
  FT_Open_Args open;

  open.flags = FT_OPEN_MEMORY;
  open.memory_base = static_cast<const FT_Byte *>(mem);
  open.memory_size = FT_Long(mem_size);
  if (blf_ensure_face(font)) {
    FT_Attach_Stream(font->face, &open);
  }
}

void blf_font_free(FontBLF *font)
{
  blf_glyph_cache_clear(font);

#ifdef WITH_HARFBUZZ
  if (font->hb_font) {
    hb_font_destroy(font->hb_font);
  }
#endif

  if (font->variations) {
    FT_Done_MM_Var(font->ft_lib, font->variations);
  }

  if (font->face) {
    std::scoped_lock lock(ft_face_load_mutex);
    FTC_Manager_RemoveFaceID(ftc_manager, font);
    font->face = nullptr;
  }
  if (font->filepath) {
    MEM_delete(font->filepath);
  }
  if (font->mem_name) {
    MEM_delete(font->mem_name);
  }

  MEM_delete(font);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Font Configure
 * \{ */

void blf_ensure_size(FontBLF *font)
{
  if (font->ft_size) {
    return;
  }

  FTC_ScalerRec scaler = {nullptr};
  scaler.face_id = font;
  scaler.width = 0;
  scaler.height = round_fl_to_uint(font->size * 64.0f);
  scaler.pixel = 0;
  scaler.x_res = BLF_DPI;
  scaler.y_res = BLF_DPI;
  if (FTC_Manager_LookupSize(ftc_manager, &scaler, &font->ft_size) == FT_Err_Ok) {
    font->ft_size->generic.data = static_cast<void *>(font);
    font->ft_size->generic.finalizer = blf_size_finalizer;
    return;
  }

  BLI_assert_unreachable();
}

bool blf_font_size(FontBLF *font, float size)
{
  if (!blf_ensure_face(font)) {
    return false;
  }

  /* FreeType uses fixed-point integers in 64ths. */
  FT_UInt ft_size = round_fl_to_uint(size * 64.0f);
  /* Adjust our new size to be on even 64ths. */
  size = float(ft_size) / 64.0f;

  if (font->size != size) {
    std::lock_guard lock(ft_cache_size_mutex);
    FTC_ScalerRec scaler = {nullptr};
    scaler.face_id = font;
    scaler.width = 0;
    scaler.height = ft_size;
    scaler.pixel = 0;
    scaler.x_res = BLF_DPI;
    scaler.y_res = BLF_DPI;
    if (FTC_Manager_LookupSize(ftc_manager, &scaler, &font->ft_size) != FT_Err_Ok) {
      return false;
    }
    font->ft_size->generic.data = static_cast<void *>(font);
    font->ft_size->generic.finalizer = blf_size_finalizer;
  }

  font->size = size;
  return true;
}

/** \} */

}  // namespace blender
