/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2003-2024 Blender Authors
 * SPDX-FileCopyrightText: 2005-2006 Peter Schlaile <peter [at] schlaile [dot] de>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BKE_lib_id.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"
#include "IMB_interp.hh"
#include "IMB_metadata.hh"

#include "BLI_math_color_blend.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_path_utils.hh"
#include "BLI_rect.h"

#include "SEQ_channels.hh"
#include "SEQ_effects.hh"
#include "SEQ_proxy.hh"
#include "SEQ_relations.hh"
#include "SEQ_render.hh"
#include "SEQ_time.hh"
#include "SEQ_utils.hh"

#include "effects.hh"
#include "render.hh"


static void init_alpha_over_or_under(Sequence *seq)
{
  Sequence *seq1 = seq->seq1;
  Sequence *seq2 = seq->seq2;

  seq->seq2 = seq1;
  seq->seq1 = seq2;
}

static bool alpha_opaque(uchar alpha)
{
  return alpha == 255;
}

static bool alpha_opaque(float alpha)
{
  return alpha >= 1.0f;
}

/* dst = src1 over src2 (alpha from src1) */
template<typename T>
static void do_alphaover_effect(
    float fac, int width, int height, const T *src1, const T *src2, T *dst)
{
  if (fac <= 0.0f) {
    memcpy(dst, src2, sizeof(T) * 4 * width * height);
    return;
  }

  for (int pixel_idx = 0; pixel_idx < width * height; pixel_idx++) {
    if (src1[3] <= 0.0f) {
      /* Alpha of zero. No color addition will happen as the colors are pre-multiplied. */
      memcpy(dst, src2, sizeof(T) * 4);
    }
    else if (fac == 1.0f && alpha_opaque(src1[3])) {
      /* No change to `src1` as `fac == 1` and fully opaque. */
      memcpy(dst, src1, sizeof(T) * 4);
    }
    else {
      float4 col1 = load_premul_pixel(src1);
      float mfac = 1.0f - fac * col1.w;
      float4 col2 = load_premul_pixel(src2);
      float4 col = fac * col1 + mfac * col2;
      store_premul_pixel(col, dst);
    }
    src1 += 4;
    src2 += 4;
    dst += 4;
  }
}

static void do_alphaover_effect(const SeqRenderData *context,
                                Sequence * /*seq*/,
                                float /*timeline_frame*/,
                                float fac,
                                const ImBuf *ibuf1,
                                const ImBuf *ibuf2,
                                int start_line,
                                int total_lines,
                                ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_alphaover_effect(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(context, ibuf1, ibuf2, out, start_line, &rect1, &rect2, &rect_out);

    do_alphaover_effect(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}
