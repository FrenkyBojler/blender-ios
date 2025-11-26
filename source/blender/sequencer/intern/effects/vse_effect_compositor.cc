/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include "DNA_sequence_types.h"

#include "effects.hh"

namespace blender::seq {

static uint32_t pcg_hash(uint32_t v)
{
  uint32_t state = v;
  uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

struct TempEffectOp {
  template<typename T> void apply(const T *src1, const T *src2, T *dst, int64_t size) const
  {
    int thresh = int(this->factor * 255);
    for (int64_t idx = 0; idx < size; idx++) {
      int hash = pcg_hash(idx) & 0xFF;
      memcpy(dst, hash >= thresh ? src1 : src2, sizeof(T) * 4);
      src1 += 4;
      src2 += 4;
      dst += 4;
    }
  }
  float factor;
};

static ImBuf *do_compositor_effect(const RenderData *context,
                                   SeqRenderState * /*state*/,
                                   Strip * /*strip*/,
                                   float /*timeline_frame*/,
                                   float fac,
                                   ImBuf *src1,
                                   ImBuf *src2)
{
  ImBuf *dst = prepare_effect_imbufs(context, src1, src2);
  TempEffectOp op;
  op.factor = fac;
  apply_effect_op(op, src1, src2, dst);
  return dst;
}

static void init_compositor_effect(Strip *strip)
{
  MEM_SAFE_FREE(strip->effectdata);
  CompositorEffectVars *data = MEM_callocN<CompositorEffectVars>(__func__);
  strip->effectdata = data;
}

void compositor_effect_get_handle(EffectHandle &rval)
{
  rval.init = init_compositor_effect;
  rval.execute = do_compositor_effect;
  rval.early_out = early_out_fade;
}

}  // namespace blender::seq
