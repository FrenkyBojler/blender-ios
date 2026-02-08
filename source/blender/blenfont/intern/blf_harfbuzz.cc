/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blf
 *
 * Text shaping and glyph positioning using Harfbuzz.
 */

#  include <harfbuzz/hb-ft.h>
#  include <harfbuzz/hb-ot.h>
#  include <harfbuzz/hb.h>

#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"

#include "BLT_lang.hh"

#include "blf_internal.hh"
#include "blf_internal_types.hh"

namespace blender {

rcti ShapedGlyph::integer_bounds() const
{
  rcti r;
  r.xmin = ft_pix_to_int_floor(bounds.xmin);
  r.xmax = ft_pix_to_int_floor(bounds.xmax);
  r.ymin = ft_pix_to_int_floor(bounds.ymin);
  r.ymax = ft_pix_to_int_ceil(bounds.ymax);
  return r;
}

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
  if (!hb_buf) {
    return; /* Out of memory */
  }

  /* Include space for null terminator. */
  size_t char_count = BLI_strnlen_utf8(str, len);
  std::u32string str32(char_count + 1, 0);
  /* Convert entire input string into array of 32-bit code points. */
  BLI_str_utf8_as_utf32(str32.data(), str, char_count + 1);

  /* Process text by script segments. Harfbuzz requires text to be
   * shaped in runs of the same script, direction, and font. We break
   * on script changes, ignoring Common/Inherited which can merge. */
  while ((segment_start + segment_len) < char_count) {
    segment_start += segment_len;
    segment_len = 0;
    size_t i;
    for (i = segment_start; i < char_count; i++) {
      script = hb_unicode_script(hb_unicode_funcs_get_default(), str32[i]);
      if (script != last_script && script != HB_SCRIPT_INHERITED && script != HB_SCRIPT_COMMON) {
        last_script = script;
        if (i > segment_start) {
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
      if (blf_ensure_face(segment_font)) {
        segment_font->hb_font = hb_ft_font_create_referenced(segment_font->face);
        hb_ot_font_set_funcs(segment_font->hb_font);
      }
      else {
        segment_font = font;
      }
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
    ft_pix pen_x = this->width; /* Continue from previous segment. */
    int max_height = this->height;
    int cwidth = std::max(gc->fixed_width, 1);
    uint glyph_count;
    hb_glyph_info_t *hb_glyph_info = hb_buffer_get_glyph_infos(hb_buf, &glyph_count);
    hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(hb_buf, nullptr);
    const bool need_release = (!gc || segment_font != font);
    GlyphCacheBLF *segment_gc = need_release ? blf_glyph_cache_acquire(segment_font) : gc;
    size_t str8_offset = 0;
    for (i = 0; i < glyph_count; i++) {
      uint32_t glyph_id = hb_glyph_info[i].codepoint;
      char32_t codepoint = str32[hb_glyph_info[i].cluster];
      GlyphBLF *g = blf_glyph_ensure(segment_font, segment_gc, codepoint, glyph_id);
      if (UNLIKELY(g == nullptr)) {
        /* Still track UTF-8 offset for missing glyphs */
        str8_offset += BLI_str_utf8_from_unicode_len(codepoint);
        continue;
      }
      const int advance = ((font->flags & BLF_MONOSPACED) ?
                               ft_pix_from_int(cwidth) * BLI_wcwidth_safe(codepoint) :
                               glyph_pos[i].x_advance);
      if (g->box_xmin == g->box_xmax) {
        /* Can happen with some spacing characters. */
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
      max_height = std::max(g->box_ymax - g->box_ymin, max_height);
    }
    this->width = pen_x; /* Update total width. */
    this->height = max_height;
    if (set_mono) {
      segment_font->flags &= ~BLF_MONOSPACED;
    }
    if (need_release) {
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

/** \} */

}  // namespace blender
