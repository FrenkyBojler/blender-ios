/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blf
 *
 * Complex text shaping. RTL languages, ligatures, etc.
 */

#include <fribidi/fribidi.h>
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb-ot.h>
#include <harfbuzz/hb.h>

#include "BLF_api.hh"

#include "BLI_rect.h"
#include "BLI_string_utf8.h"

#include "blf_internal.hh"
#include "blf_internal_types.hh"

/* -------------------------------------------------------------------- */
/** \name Complex Text Layout (Bidirectional, Shaping, Ligatures, etc)
 * \{ */

/* Using FriBiDi to reorder input string. Harfbuzz can do so but only per-script so
 * [lang1_RTL lang2_RTL] would result in [lang1_LTR lang2_LTR] instead of correct
 * [lang2_LTR lang1_LTR].
 * Caret position and selection based on positions in logical (input) string, which is required
 * to select portions of text with differing directions. Uses Harfbuzz Level 1 (recommended),
 * which does not merge clusters (combine marks and modifiers with base character's cluster value).
 */

/* Harley: Disable OpenType features "liga" and "clig" if letter-spacing. */

typedef struct ShapingData {
  /* For the entire string. */
  size_t char_count = 0;
  std::u32string logical_str = {};
  std::u32string visual_str = {};
  blender::Vector<int> positions_L2V = {};
  blender::Vector<int> positions_V2L = {};
  int width = 0;
  int height = 0;

  hb_unicode_funcs_t *hb_ufuncs = nullptr;
  hb_buffer_t *hb_buf = nullptr;
#ifdef WITH_FRIBIDI
  FriBidiParType base_direction = FRIBIDI_PAR_LTR;
#endif

  struct SegmentData {
    /* For each run of language/script/direction. */
    size_t char_offset = 0;
    size_t char_count = 0;
    FontBLF *font = nullptr;
    GlyphCacheBLF *gc = nullptr;
    hb_script_t last_script = HB_SCRIPT_LATIN;
    hb_script_t current_script = HB_SCRIPT_LATIN;

    uint glyph_count = 0;
    blender::Vector<GlyphBLF *> glyphs = {};
    hb_glyph_info_t *hb_glyph_info = nullptr;
    hb_glyph_position_t *glyph_pos = nullptr;
    blender::Vector<rcti> bounds = {};
  } segment;

  ShapingData(const char *str, size_t len);
  ~ShapingData();
  bool next_segment();
  bool process(FontBLF *def_font, GlyphCacheBLF *gc = nullptr, ResultBLF *r_info = nullptr);
} ShapingData;

ShapingData::ShapingData(const char *str, size_t len)
{
  this->hb_ufuncs = hb_unicode_funcs_get_default();
  this->hb_buf = hb_buffer_create();

  /* Include space for null terminator. */
  this->char_count = BLI_strnlen_utf8(str, len) + 1;

  this->logical_str.resize(this->char_count, 0);
  this->visual_str.resize(this->char_count, 0);

  this->positions_L2V.resize(this->char_count, 0);
  this->positions_V2L.resize(this->char_count, 0);

  /* Convert input string into array of 32-bit code points. */
  BLI_str_utf8_as_utf32(this->logical_str.data(), str, this->char_count);

#ifdef WITH_FRIBIDI
  /* FriBiDi reorder. Do not include null terminator or it might move too. */
  fribidi_log2vis((FriBidiChar *)this->logical_str.data(),
                  int(this->char_count) - 1,
                  &this->base_direction,
                  (FriBidiChar *)this->visual_str.data(),
                  this->positions_L2V.data(),
                  this->positions_V2L.data(),
                  nullptr);
#else
  this->visual_str = this->logical_str;
#endif
}

ShapingData::~ShapingData()
{
  if (this->hb_buf) {
    hb_buffer_destroy(this->hb_buf);
  }
}

bool ShapingData::next_segment()
{
  this->segment.char_offset += this->segment.char_count;
  this->segment.char_count = 0;
  this->segment.glyph_count = 0;

  if (this->segment.char_offset >= this->char_count - 1) {
    return false;
  }

  size_t i;

  for (i = this->segment.char_offset; i < this->char_count && this->visual_str[i]; i++) {
    hb_script_t script = hb_unicode_script(this->hb_ufuncs, this->visual_str[i]);
    if (script != this->segment.last_script && script != HB_SCRIPT_INHERITED &&
        script != HB_SCRIPT_COMMON)
    {
      this->segment.current_script = this->segment.last_script;
      this->segment.last_script = script;
      if (i > 0) {
        break;
      }
    }
  }

  /* End of string. */
  if (!this->visual_str[i]) {
    this->segment.current_script = this->segment.last_script;
  }

  this->segment.char_count = (i - this->segment.char_offset);
  if (this->segment.char_count == 0) {
    return false;
  }

  return true;
}

bool ShapingData::process(FontBLF *font, GlyphCacheBLF *gc, ResultBLF *r_info)
{
  if (!this->next_segment()) {
    return false;
  }

  this->segment.font = font;
  this->segment.gc = gc;

  if (hb_buffer_get_length(this->hb_buf) != 0) {
    hb_buffer_clear_contents(this->hb_buf);
  }

  hb_buffer_add_utf32(this->hb_buf,
                      (uint32_t *)this->visual_str.data(),
                      int(this->char_count),
                      uint(this->segment.char_offset),
                      int(this->segment.char_count));

  this->segment.hb_glyph_info = hb_buffer_get_glyph_infos(this->hb_buf,
                                                          &this->segment.glyph_count);
  for (unsigned int i = 0; i < this->segment.glyph_count; i++) {
    this->segment.hb_glyph_info[i].cluster = (uint32_t)(this->segment.char_offset + i);
  }

  hb_buffer_guess_segment_properties(this->hb_buf);
  hb_buffer_set_script(this->hb_buf, this->segment.current_script);
  hb_buffer_set_direction(this->hb_buf, HB_DIRECTION_LTR);
  hb_buffer_set_cluster_level(this->hb_buf, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);

  /* Can the current font handle this script? */
  if (!ELEM(this->segment.current_script,
            HB_SCRIPT_COMMON,
            HB_SCRIPT_INHERITED,
            HB_SCRIPT_UNKNOWN,
            HB_SCRIPT_LATIN))
  {
    this->segment.font = blf_font_script_ensure(this->segment.font,
                                                this->visual_str[this->segment.char_offset]);
  }

  if (!this->segment.font->hb_font) {
    this->segment.font->hb_font = hb_ft_font_create_referenced(this->segment.font->face);
    hb_ot_font_set_funcs(this->segment.font->hb_font);
  }

  hb_font_set_scale(this->segment.font->hb_font, int(font->size * 64.0f), int(font->size * 64.0f));

  hb_feature_t userfeatures[1];
  userfeatures[0].tag = HB_TAG('k', 'e', 'r', 'n');
  userfeatures[0].value = 1;
  userfeatures[0].start = HB_FEATURE_GLOBAL_START;
  userfeatures[0].end = HB_FEATURE_GLOBAL_END;

  hb_shape_full(this->segment.font->hb_font, this->hb_buf, userfeatures, 1, nullptr);

  this->segment.hb_glyph_info = hb_buffer_get_glyph_infos(this->hb_buf,
                                                          &this->segment.glyph_count);
  this->segment.glyph_pos = hb_buffer_get_glyph_positions(this->hb_buf, nullptr);

  bool set_mono = this->segment.font != font && font->flags & BLF_MONOSPACED &&
                  !(this->segment.font->flags & BLF_MONOSPACED);
  if (set_mono) {
    this->segment.font->flags |= BLF_MONOSPACED;
  }

  this->segment.glyphs.resize(this->segment.glyph_count);
  this->segment.bounds.resize(this->char_count);

  int cwidth = std::max(gc->fixed_width, 1);
  int pen_x = this->width * 64;
  int max_width = pen_x;

  this->segment.gc = (!gc || this->segment.font != font) ?
                         blf_glyph_cache_acquire(this->segment.font) :
                         gc;

  for (uint i = 0; i < this->segment.glyph_count; i++) {
    uint32_t glyph_id = this->segment.hb_glyph_info[i].codepoint;
    char32_t codepoint = this->visual_str[this->segment.hb_glyph_info[i].cluster];

    this->segment.glyphs[i] = blf_glyph_ensure(
        this->segment.font, this->segment.gc, codepoint, glyph_id);

    GlyphBLF *g = this->segment.glyphs[i];
    rcti *bounds = &this->segment.bounds[i];
    hb_glyph_position_t *pos = &this->segment.glyph_pos[i];

    const int advance = ((font->flags & BLF_MONOSPACED) ?
                             ft_pix_from_int(cwidth) * BLI_wcwidth_safe(codepoint) :
                             pos->x_advance);

    if (g->box_xmin == g->box_xmax) {
      g->box_xmax = g->box_xmin + advance;
    }

    bounds->xmin = pen_x + g->box_xmin + pos->x_offset;
    bounds->xmax = pen_x + g->box_xmax + pos->x_offset;
    bounds->ymin = g->box_ymin + pos->y_offset + pos->y_offset;
    bounds->ymax = bounds->ymin + g->box_ymax + pos->y_offset;

#ifndef BLF_SUBPIXEL_POSITION
    pen_x = FT_PIX_ROUND(pen_x);
#endif
#ifdef BLF_SUBPIXEL_AA
    this->segment.glyphs[i] = blf_glyph_ensure_subpixel(
        this->segment.font, this->segment.gc, g, pen_x);
#endif

    max_width = pen_x + std::max(pos->x_advance, g->advance_x);
    pen_x += advance;
    height = std::max(this->segment.glyphs[i]->pos[1], height);
  }

  this->width = ft_pix_to_int(max_width);
  this->height += ft_pix_to_int(height);

  if (set_mono) {
    font->flags &= ~BLF_MONOSPACED;
  }
  if (!gc || this->segment.font != font) {
    blf_glyph_cache_release(this->segment.font);
  }

  if (r_info) {
    r_info->lines = 1;
    r_info->width = this->width;
  }

  /* Is there more data still to process after this run? */
  return (this->char_count > (this->segment.char_offset + this->segment.char_count));
}

int blf_shaping_draw(FontBLF *font,
                     GlyphCacheBLF *gc,
                     const char *str,
                     const size_t str_len,
                     struct ResultBLF *r_info,
                     ft_pix pen_y)
{
  if (!str[0] || !str_len) {
    return 0;
  }

  ShapingData text(str, str_len);
  blf_batch_draw_begin(font);

  while (text.process(font, gc, r_info)) {
    for (uint i = 0; i < text.segment.glyph_count; i++) {
      blf_glyph_draw(
          text.segment.font,
          text.segment.gc,
          text.segment.glyphs[i],
          ft_pix_to_int_floor(text.segment.bounds[i].xmin - text.segment.glyphs[i]->box_xmin),
          ft_pix_to_int_floor(pen_y + text.segment.glyph_pos[i].y_offset));
    }
  }

  if (!g_batch.active) {
    blf_batch_draw();
  }

  return text.width;
}

void blf_shaping_bounds(FontBLF *font,
                        GlyphCacheBLF *gc,
                        const char *str,
                        const size_t str_len,
                        rcti *bounds,
                        ft_pix pen_y)
{
  if (!str[0] || !str_len) {
    return;
  }

  ShapingData text(str, str_len);

  bounds->xmin = 0;
  bounds->xmax = -30000;
  bounds->ymin = pen_y;
  bounds->ymax = -30000;

  while (text.process(font, gc, nullptr)) {
  }
  bounds->xmax = std::max(bounds->xmax, text.width);
  bounds->ymax = std::max(bounds->ymax, text.height);
}

void blf_shaping_foreach(FontBLF *font,
                         GlyphCacheBLF *gc,
                         const char *str,
                         const size_t str_len,
                         BLF_GlyphBoundsFn user_fn,
                         void *user_data)
{
  ShapingData text(str, str_len);

  while (text.process(font, gc, nullptr)) {

    for (uint i = 0; i < text.segment.glyph_count; i++) {

      if (text.segment.glyphs[i]->advance_x <= 0) {
        /* Ignore combining marks. */
        continue;
      };

      rcti bounds;
      bounds.xmin = ft_pix_to_int_floor(text.segment.bounds[i].xmin);
      bounds.xmax = ft_pix_to_int_ceil(text.segment.bounds[i].xmax);
      bounds.ymin = ft_pix_to_int_floor(text.segment.bounds[i].ymin);
      bounds.ymax = ft_pix_to_int_ceil(text.segment.bounds[i].ymax);

      /* This is called per-glyph in visual order. The second argument is byte offset into
       * the original (logical) UTF-8 string, so decreasing if RTL. */
      int logical_pos = text.positions_V2L[text.segment.hb_glyph_info[i].cluster];

      /* Convert from UTF-32 position to UTF-8 byte offset. */
      size_t str_step_ofs = (size_t)BLI_str_utf8_offset_from_index(str, str_len, logical_pos);
      if (user_fn(str, str_step_ofs, &bounds, user_data) == false) {
        return;
      }
    }
  }
}

typedef struct StrSelectionGlyphBounds_Data {
  size_t sel_start;
  size_t sel_length;
  bool RTL;
  short current_box;
  rcti boxes[2];
} StrSelectionGlyphBounds_Data;

static bool blf_str_selection_foreach_glyph(const char *str,
                                            const size_t str_step_ofs,
                                            const rcti *bounds,
                                            void *user_data)
{
  StrSelectionGlyphBounds_Data *data = static_cast<StrSelectionGlyphBounds_Data *>(user_data);
  /* Called in glyph order, so str_step_ofs varies. */

  if (str_step_ofs >= data->sel_start && str_step_ofs < (data->sel_start + data->sel_length)) {
    bool RTL = BLI_char_isRTL_utf8(str + str_step_ofs);
    if (RTL != data->RTL) {
      data->current_box = 1;
    }
    if (data->boxes[data->current_box].xmin == data->boxes[data->current_box].xmax) {
      data->boxes[data->current_box].xmin = bounds->xmin;
    }
    if (bounds->xmax > data->boxes[data->current_box].xmax) {
      data->boxes[data->current_box].xmax = bounds->xmax;
    }
    data->RTL = RTL;
  }
  /* Always test all glyphs. */
  return true;
}

blender::Vector<blender::Bounds<int>> blf_shaping_selection_boxes(
    FontBLF *font, const char *str, size_t str_len, size_t sel_start, size_t sel_length)
{
  blender::Vector<blender::Bounds<int>> boxes;

  StrSelectionGlyphBounds_Data data = {
      sel_start, sel_length, BLI_char_isRTL_utf8(str + sel_start), 0, {{}}};
  blf_font_boundbox_foreach_glyph(font, str, str_len, blf_str_selection_foreach_glyph, &data);

  /* Avoid overlap when multiple boxes meet. */
  if (data.boxes[1].xmin > 0) {
    data.boxes[1].xmin += 1;
  }

  boxes.append(blender::Bounds(data.boxes[0].xmin, data.boxes[0].xmax));
  if (data.boxes[1].xmin != data.boxes[1].xmax) {
    boxes.append(blender::Bounds(data.boxes[1].xmin, data.boxes[1].xmax));
  }
  return boxes;
}
