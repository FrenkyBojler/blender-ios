/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include "COM_context.hh"
#include "COM_domain.hh"
#include "COM_evaluator.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_sequence_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "SEQ_render.hh"

#include "effects.hh"
#include "render.hh"

namespace blender::seq {

class CompositorEffectContext : public compositor::Context {
  const RenderData &render_data_;
  bNodeTree *node_group_;

  ImBuf *input_1_;
  ImBuf *input_2_;
  ImBuf *output_;
  float factor_;
  float2 result_translation_ = float2(0, 0);
  const Strip *strip_;

 public:
  CompositorEffectContext(const RenderData &render_data,
                          bNodeTree *node_tree,
                          ImBuf *input_1,
                          ImBuf *input_2,
                          ImBuf *output,
                          float factor,
                          const Strip &strip)
      : compositor::Context(),
        render_data_(render_data),
        node_group_(node_tree),
        input_1_(input_1),
        input_2_(input_2),
        output_(output),
        factor_(factor),
        strip_(&strip)
  {
  }

  float2 get_result_translation() const
  {
    return result_translation_;
  }

  const Scene &get_scene() const override
  {
    return *render_data_.scene;
  }

  const bNodeTree &get_node_tree() const override
  {
    return *DEG_get_evaluated<bNodeTree>(render_data_.depsgraph, this->node_group_);
  }

  compositor::OutputTypes needed_outputs() const override
  {
    compositor::OutputTypes needed_outputs = compositor::OutputTypes::Composite;
    if (!render_data_.render) {
      needed_outputs |= compositor::OutputTypes::Viewer;
    }
    return needed_outputs;
  }

  bool treat_viewer_as_compositor_output() const override
  {
    return true;
  }

  compositor::Domain get_compositing_domain() const override
  {
    return compositor::Domain(int2(this->output_->x, this->output_->y));
  }

  void write_output(const compositor::Result &result) override
  {
    if (result.is_single_value()) {
      IMB_rectfill(this->output_, result.get_single_value<compositor::Color>());
      return;
    }

    result_translation_ = result.domain().transformation.location();
    std::memcpy(this->output_->float_buffer.data,
                result.cpu_data().data(),
                IMB_get_pixel_count(this->output_) * sizeof(float) * 4);
  }

  void write_viewer(const compositor::Result &result) override
  {
    /* Within compositor modifier, output and viewer output function the same. */
    this->write_output(result);
  }

  compositor::Result get_input(StringRef name) override
  {
    compositor::Result result = this->create_result(compositor::ResultType::Color);
    if (name == "Image") {
      result.wrap_external(this->input_1_->float_buffer.data,
                           int2(this->input_1_->x, this->input_1_->y));
    }
    else if (name == "Image2") {
      result.wrap_external(this->input_2_->float_buffer.data,
                           int2(this->input_2_->x, this->input_2_->y));
    }
    else if (name == "Factor") {
      result = this->create_result(compositor::ResultType::Float);
      result.allocate_single_value();
      result.set_single_value(this->factor_);
    }
    return result;
  }

  const Strip *get_strip() const override
  {
    return strip_;
  }

  bool use_gpu() const override
  {
    return false;
  }
};

static bool is_linear_float_buffer(const ImBuf *image)
{
  return image->float_buffer.data &&
         IMB_colormanagement_space_is_scene_linear(image->float_buffer.colorspace);
}

static ImBuf *make_linear_float_buffer(ImBuf *src)
{
  if (!src) {
    return nullptr;
  }

  /* Already have scene linear float pixels, return same buffer. */
  if (is_linear_float_buffer(src)) {
    return src;
  }

  ImBuf *dst = IMB_allocImBuf(
      src->x, src->y, src->planes, IB_float_data | IB_uninitialized_pixels);
  const char *to_colorspace = IMB_colormanagement_role_colorspace_name_get(
      COLOR_ROLE_SCENE_LINEAR);
  if (src->float_buffer.data == nullptr) {
    const char *from_colorspace = IMB_colormanagement_get_rect_colorspace(src);
    IMB_colormanagement_transform_byte_to_float(dst->float_buffer.data,
                                                src->byte_buffer.data,
                                                src->x,
                                                src->y,
                                                src->channels,
                                                from_colorspace,
                                                to_colorspace);
  }
  else {
    const char *from_colorspace = IMB_colormanagement_get_float_colorspace(src);
    //@TODO: src->dst transform would be faster instead of copy + transform in-place
    memcpy(dst->float_buffer.data,
           src->float_buffer.data,
           IMB_get_pixel_count(src) * src->channels * sizeof(float));
    IMB_colormanagement_transform_float(dst->float_buffer.data,
                                        dst->x,
                                        dst->y,
                                        dst->channels,
                                        from_colorspace,
                                        to_colorspace,
                                        true);
  }
  IMB_colormanagement_assign_float_colorspace(dst, to_colorspace);
  return dst;
}

static ImBuf *do_compositor_effect(const RenderData *context,
                                   SeqRenderState * /*state*/,
                                   Strip *strip,
                                   float /*timeline_frame*/,
                                   float fac,
                                   ImBuf *src1,
                                   ImBuf *src2)
{
  const int x = context->rectx;
  const int y = context->recty;
  ImBuf *out = IMB_allocImBuf(x, y, 32, IB_float_data | IB_uninitialized_pixels);
  IMB_colormanagement_assign_float_colorspace(
      out, IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_SCENE_LINEAR));

  CompositorEffectVars *data = static_cast<CompositorEffectVars *>(strip->effectdata);
  if (!data || !data->node_group) {
    IMB_rectfill(out, float4(0, 0, 0, 1));
  }
  else {
    ImBuf *linear_src1 = make_linear_float_buffer(src1);
    ImBuf *linear_src2 = make_linear_float_buffer(src2);

    CompositorEffectContext com_context(
        *context, data->node_group, linear_src1, linear_src2, out, fac, *strip);
    compositor::Evaluator evaluator(com_context);
    evaluator.evaluate();
    // context.result_translation += com_context.get_result_translation(); //@TODO?

    if (linear_src1 != src1) {
      IMB_freeImBuf(linear_src1);
    }
    if (linear_src2 != src2) {
      IMB_freeImBuf(linear_src2);
    }
    seq_imbuf_to_sequencer_space(context->scene, out, true);
  }
  return out;
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
