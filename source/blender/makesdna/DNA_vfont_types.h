/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 *
 * Vector Fonts used for text in the 3D Viewport
 * (unrelated to text used to render the GUI).
 */

#pragma once

#include "DNA_ID.h"

/** Overlap removal method for VFont. */
enum eVFont_SimplifyMethod {
  DNA_VFONT_SIMPLIFY_SKIA = 0,
  DNA_VFONT_SIMPLIFY_FONTFORGE = 1,
};

namespace blender {

struct PackedFile;
struct VFontData;

struct VFont {
#ifdef __cplusplus
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_VF;
#endif

  ID id;

  char filepath[/*FILE_MAX*/ 1024] = "";

  struct VFontData *data = nullptr;
  struct PackedFile *packedfile = nullptr;

  /* runtime only, holds memory for freetype to read from
   * TODO: replace this with #blf_font_new() style loading. */
  struct PackedFile *temp_pf = nullptr;

  /** Remove overlapping regions from glyph curves. */
  char use_simplify = 0;
  char simplify_method = 0;
  char _pad[6];
};

#define FO_BUILTIN_NAME "<builtin>"

}  // namespace blender
