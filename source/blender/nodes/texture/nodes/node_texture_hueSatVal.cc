/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup texnodes
 */

#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "node_texture_util.hh"

namespace blender {

static bke::bNodeSocketTemplate inputs[] = {
    {.type = SOCK_FLOAT,
     .name = N_("Hue"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = -0.5f,
     .max = 0.5f,
     .subtype = PROP_NONE},
    {.type = SOCK_FLOAT,
     .name = N_("Saturation"),
     .val1 = 1.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = 0.0f,
     .max = 2.0f,
     .subtype = PROP_NONE},
    {.type = SOCK_FLOAT,
     .name = N_("Value"),
     .val1 = 1.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = 0.0f,
     .max = 2.0f,
     .subtype = PROP_NONE},
    {.type = SOCK_FLOAT,
     .name = N_("Factor"),
     .val1 = 1.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = 0.0f,
     .max = 1.0f,
     .subtype = PROP_NONE},
    {.type = SOCK_RGBA,
     .name = N_("Color"),
     .val1 = 0.8f,
     .val2 = 0.8f,
     .val3 = 0.8f,
     .val4 = 1.0f},
    {.type = -1, .name = ""},
};
static bke::bNodeSocketTemplate outputs[] = {
    {.type = SOCK_RGBA, .name = N_("Color")},
    {.type = -1, .name = ""},
};

static void do_hue_sat_fac(
    bNode * /*node*/, float *out, float hue, float sat, float val, float *in, float fac)
{
  if (fac != 0 && (hue != 0.5f || sat != 1 || val != 1)) {
    float col[3], hsv[3], mfac = 1.0f - fac;

    rgb_to_hsv(in[0], in[1], in[2], hsv, hsv + 1, hsv + 2);
    hsv[0] += (hue - 0.5f);
    if (hsv[0] > 1.0f) {
      hsv[0] -= 1.0f;
    }
    else if (hsv[0] < 0.0f) {
      hsv[0] += 1.0f;
    }
    hsv[1] *= sat;
    if (hsv[1] > 1.0f) {
      hsv[1] = 1.0f;
    }
    else if (hsv[1] < 0.0f) {
      hsv[1] = 0.0f;
    }
    hsv[2] *= val;
    if (hsv[2] > 1.0f) {
      hsv[2] = 1.0f;
    }
    else if (hsv[2] < 0.0f) {
      hsv[2] = 0.0f;
    }
    hsv_to_rgb(hsv[0], hsv[1], hsv[2], col, col + 1, col + 2);

    out[0] = mfac * in[0] + fac * col[0];
    out[1] = mfac * in[1] + fac * col[1];
    out[2] = mfac * in[2] + fac * col[2];
  }
  else {
    copy_v4_v4(out, in);
  }
}

static void colorfn(float *out, TexParams *p, bNode *node, bNodeStack **in, short thread)
{
  float hue = tex_input_value(in[0], p, thread);
  float sat = tex_input_value(in[1], p, thread);
  float val = tex_input_value(in[2], p, thread);
  float fac = tex_input_value(in[3], p, thread);

  float col[4];
  tex_input_rgba(col, in[4], p, thread);

  hue += 0.5f; /* [-0.5, 0.5] -> [0, 1] */

  do_hue_sat_fac(node, out, hue, sat, val, col, fac);

  out[3] = col[3];
}

static void exec(void *data,
                 int /*thread*/,
                 bNode *node,
                 bNodeExecData *execdata,
                 bNodeStack **in,
                 bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &colorfn, static_cast<TexCallData *>(data));
}

void register_node_type_tex_hue_sat()
{
  static bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeHueSaturation", TEX_NODE_HUE_SAT);
  ntype.ui_name = "Hue/Saturation/Value";
  ntype.enum_name_legacy = "HUE_SAT";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  bke::node_type_socket_templates(&ntype, inputs, outputs);
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Middle);
  ntype.exec_fn = exec;

  bke::node_register_type(ntype);
}

}  // namespace blender
