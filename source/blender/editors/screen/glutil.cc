/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edscr
 */

#include "DNA_userdef_types.h"
#include "DNA_vec_types.h"

#include "BLI_utildefines.h"

#include "BIF_glutil.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "GPU_immediate.hh"
#include "GPU_texture.hh"

namespace blender {

/* ******************************************** */

static void immDrawPixelsTexSetupAttributes(IMMDrawPixelsTexState *state)
{
  GPUVertFormat *vert_format = immVertexFormat();
  state->pos = GPU_vertformat_attr_add(vert_format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  state->texco = GPU_vertformat_attr_add(vert_format, "texCoord", gpu::VertAttrType::SFLOAT_32_32);
}

IMMDrawPixelsTexState immDrawPixelsTexSetup(int builtin)
{
  IMMDrawPixelsTexState state;
  immDrawPixelsTexSetupAttributes(&state);

  state.shader = GPU_shader_get_builtin_shader(GPUBuiltinShader(builtin));

  /* Shader will be unbind by immUnbindProgram in a `immDrawPixelsTex` function. */
  immBindBuiltinProgram(GPUBuiltinShader(builtin));
  state.do_shader_unbind = true;

  return state;
}

void immDrawPixelsTexScaledFullSize(const IMMDrawPixelsTexState *state,
                                    const float x,
                                    const float y,
                                    const int img_w,
                                    const int img_h,
                                    const gpu::TextureFormat gpu_format,
                                    const bool use_filter,
                                    const void *rect,
                                    const float scaleX,
                                    const float scaleY,
                                    const float xzoom,
                                    const float yzoom,
                                    const float color[4])
{
  static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const float draw_width = img_w * scaleX * xzoom;
  const float draw_height = img_h * scaleY * yzoom;
  /* Down-scaling with regular bi-linear interpolation (i.e. #GL_LINEAR) doesn't give good
   * filtering results. Mipmaps can be used to get better results (i.e. #GL_LINEAR_MIPMAP_LINEAR),
   * so always use mipmaps when filtering. */
  const bool use_mipmap = use_filter && ((draw_width < img_w) || (draw_height < img_h));
  const int mip_len = use_mipmap ? 9999 : 1;

  gpu::Texture *tex = GPU_texture_create_2d("immDrawPixels",
                                            img_w,
                                            img_h,
                                            mip_len,
                                            gpu_format,
                                            GPU_TEXTURE_USAGE_SHADER_READ |
                                                GPU_TEXTURE_USAGE_SHADER_WRITE,
                                            nullptr);

  const bool use_float_data = ELEM(gpu_format,
                                   gpu::TextureFormat::SFLOAT_16_16_16_16,
                                   gpu::TextureFormat::SFLOAT_16_16_16,
                                   gpu::TextureFormat::SFLOAT_16);
  eGPUDataFormat gpu_data_format = (use_float_data) ? GPU_DATA_FLOAT : GPU_DATA_UBYTE;
  GPU_texture_update(tex, gpu_data_format, rect);

  GPU_texture_filter_mode(tex, use_filter);
  if (use_mipmap) {
    GPU_texture_update_mipmap_chain(tex);
    GPU_texture_mipmap_mode(tex, true, true);
  }
  GPU_texture_extend_mode(tex, GPU_SAMPLER_EXTEND_MODE_EXTEND);

  GPU_texture_bind(tex, 0);

  /* optional */
  /* NOTE: Shader could be null for GLSL OCIO drawing, it is fine, since
   * it does not need color.
   */
  if (state->shader != nullptr && GPU_shader_get_uniform(state->shader, "color") != -1) {
    immUniformColor4fv((color) ? color : white);
  }

  uint pos = state->pos, texco = state->texco;

  immBegin(GPU_PRIM_TRI_FAN, 4);
  immAttr2f(texco, 0.0f, 0.0f);
  immVertex2f(pos, x, y);

  immAttr2f(texco, 1.0f, 0.0f);
  immVertex2f(pos, x + draw_width, y);

  immAttr2f(texco, 1.0f, 1.0f);
  immVertex2f(pos, x + draw_width, y + draw_height);

  immAttr2f(texco, 0.0f, 1.0f);
  immVertex2f(pos, x, y + draw_height);
  immEnd();

  if (state->do_shader_unbind) {
    immUnbindProgram();
  }

  GPU_texture_unbind(tex);
  GPU_texture_free(tex);
}

void immDrawPixelsTexTiled_scaling(IMMDrawPixelsTexState *state,
                                   float x,
                                   float y,
                                   int img_w,
                                   int img_h,
                                   gpu::TextureFormat gpu_format,
                                   bool use_filter,
                                   const void *rect,
                                   float scaleX,
                                   float scaleY,
                                   float xzoom,
                                   float yzoom,
                                   const float color[4])
{
  const bool use_float_data = ELEM(gpu_format,
                                   gpu::TextureFormat::SFLOAT_16_16_16_16,
                                   gpu::TextureFormat::SFLOAT_16_16_16,
                                   gpu::TextureFormat::SFLOAT_16);
  eGPUDataFormat gpu_data = use_float_data ? GPU_DATA_FLOAT : GPU_DATA_UBYTE;

  gpu::Texture *tex = GPU_texture_create_2d(
      "immDrawPixels", img_w, img_h, 1, gpu_format, GPU_TEXTURE_USAGE_SHADER_READ, nullptr);

  GPU_texture_filter_mode(tex, use_filter);
  GPU_texture_extend_mode(tex, GPU_SAMPLER_EXTEND_MODE_EXTEND);

  GPU_texture_bind(tex, 0);

  /* optional */
  /* NOTE: Shader could be null for GLSL OCIO drawing, it is fine, since
   * it does not need color.
   */
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  if (state->shader != nullptr && GPU_shader_get_uniform(state->shader, "color") != -1) {
    immUniformColor4fv((color) ? color : white);
  }

  GPU_texture_update(tex, gpu_data, rect);

  uint pos = state->pos, texco = state->texco;

  immBegin(GPU_PRIM_TRI_FAN, 4);
  immAttr2f(texco, 0, 0);
  immVertex2f(pos, x, y);

  immAttr2f(texco, 1, 0);
  immVertex2f(pos, x + img_w * xzoom * scaleX, y);

  immAttr2f(texco, 1, 1);
  immVertex2f(pos, x + img_w * xzoom * scaleX, y + img_h * yzoom * scaleY);

  immAttr2f(texco, 0, 1);
  immVertex2f(pos, x, y + img_h * yzoom * scaleY);
  immEnd();

  if (state->do_shader_unbind) {
    immUnbindProgram();
  }

  GPU_texture_unbind(tex);
  GPU_texture_free(tex);
}

void immDrawPixelsTexTiled(IMMDrawPixelsTexState *state,
                           float x,
                           float y,
                           int img_w,
                           int img_h,
                           gpu::TextureFormat gpu_format,
                           bool use_filter,
                           const void *rect,
                           float xzoom,
                           float yzoom,
                           const float color[4])
{
  immDrawPixelsTexTiled_scaling(
      state, x, y, img_w, img_h, gpu_format, use_filter, rect, 1.0f, 1.0f, xzoom, yzoom, color);
}

/* **** Color management helper functions for GLSL display/transform ***** */

void ED_draw_imbuf(const ImBuf *ibuf,
                   float x,
                   float y,
                   bool use_filter,
                   const ColorManagedViewSettings *view_settings,
                   const ColorManagedDisplaySettings *display_settings,
                   float zoom_x,
                   float zoom_y)
{
  using namespace blender::gpu;

  /* Early out */
  if (ibuf->byte_data() == nullptr && ibuf->float_data() == nullptr) {
    return;
  }

  IMMDrawPixelsTexState state = {nullptr};
  /* We want GLSL state to be fully handled by OCIO. */
  state.do_shader_unbind = false;
  immDrawPixelsTexSetupAttributes(&state);

  const ColorSpace *colorspace = ibuf->float_data() ? ibuf->float_buffer.colorspace :
                                                      ibuf->byte_buffer.colorspace;
  const bool predivide = ibuf->float_data() != nullptr;
  if (!IMB_colormanagement_setup_glsl_draw_from_space(
          view_settings, display_settings, colorspace, ibuf->dither, predivide, false))
  {
    return;
  }

  const void *texture_data = nullptr;
  TextureFormat format = TextureFormat::Invalid;
  if (ibuf->float_data()) {
    texture_data = ibuf->float_data();
    if (ibuf->channels == 1) {
      format = TextureFormat::SFLOAT_16;
    }
    else if (ibuf->channels == 3) {
      format = TextureFormat::SFLOAT_16_16_16;
    }
    else if (ibuf->channels == 4) {
      format = TextureFormat::SFLOAT_16_16_16_16;
    }
    else {
      BLI_assert_msg(0, "Incompatible number of channels for GLSL display");
    }
  }
  else {
    texture_data = ibuf->byte_data();
    format = TextureFormat::UNORM_8_8_8_8;
  }

  if (format != TextureFormat::Invalid) {
    immDrawPixelsTexTiled_scaling(&state,
                                  x,
                                  y,
                                  ibuf->x,
                                  ibuf->y,
                                  format,
                                  use_filter,
                                  texture_data,
                                  1.0f,
                                  1.0f,
                                  zoom_x,
                                  zoom_y,
                                  nullptr);
  }

  IMB_colormanagement_finish_glsl_draw();
}

void ED_draw_imbuf_ctx(const bContext *C,
                       const ImBuf *ibuf,
                       float x,
                       float y,
                       bool use_filter,
                       float zoom_x,
                       float zoom_y)
{
  ColorManagedViewSettings *view_settings;
  ColorManagedDisplaySettings *display_settings;
  IMB_colormanagement_display_settings_from_ctx(C, &view_settings, &display_settings);
  ED_draw_imbuf(ibuf, x, y, use_filter, view_settings, display_settings, zoom_x, zoom_y);
}

void immDrawBorderCorners(uint pos, const rcti *border, float zoomx, float zoomy)
{
  float delta_x = 4.0f * UI_SCALE_FAC / zoomx;
  float delta_y = 4.0f * UI_SCALE_FAC / zoomy;

  delta_x = min_ff(delta_x, border->xmax - border->xmin);
  delta_y = min_ff(delta_y, border->ymax - border->ymin);

  /* left bottom corner */
  immBegin(GPU_PRIM_LINE_STRIP, 3);
  immVertex2f(pos, border->xmin, border->ymin + delta_y);
  immVertex2f(pos, border->xmin, border->ymin);
  immVertex2f(pos, border->xmin + delta_x, border->ymin);
  immEnd();

  /* left top corner */
  immBegin(GPU_PRIM_LINE_STRIP, 3);
  immVertex2f(pos, border->xmin, border->ymax - delta_y);
  immVertex2f(pos, border->xmin, border->ymax);
  immVertex2f(pos, border->xmin + delta_x, border->ymax);
  immEnd();

  /* right bottom corner */
  immBegin(GPU_PRIM_LINE_STRIP, 3);
  immVertex2f(pos, border->xmax - delta_x, border->ymin);
  immVertex2f(pos, border->xmax, border->ymin);
  immVertex2f(pos, border->xmax, border->ymin + delta_y);
  immEnd();

  /* right top corner */
  immBegin(GPU_PRIM_LINE_STRIP, 3);
  immVertex2f(pos, border->xmax - delta_x, border->ymax);
  immVertex2f(pos, border->xmax, border->ymax);
  immVertex2f(pos, border->xmax, border->ymax - delta_y);
  immEnd();
}

}  // namespace blender
