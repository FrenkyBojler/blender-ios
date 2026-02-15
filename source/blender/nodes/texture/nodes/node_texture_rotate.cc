/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup texnodes
 */

#include <cmath>

#include "BLI_math_vector.h"

#include "node_texture_util.hh"

namespace blender {

static bke::bNodeSocketTemplate inputs[] = {
    {.type = SOCK_RGBA,
     .name = N_("Color"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 1.0f},
    {.type = SOCK_FLOAT,
     .name = N_("Turns"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = -1.0f,
     .max = 1.0f,
     .subtype = PROP_NONE},
    {.type = SOCK_VECTOR,
     .name = N_("Axis"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 1.0f,
     .val4 = 0.0f,
     .min = -1.0f,
     .max = 1.0f,
     .subtype = PROP_DIRECTION},
    {.type = -1, .name = ""},
};

static bke::bNodeSocketTemplate outputs[] = {
    {.type = SOCK_RGBA, .name = N_("Color")},
    {.type = -1, .name = ""},
};

static void rotate(float new_co[3], float a, const float ax[3], const float co[3])
{
  float para[3];
  float perp[3];
  float cp[3];

  float cos_a = cosf(a * float(2 * M_PI));
  float sin_a = sinf(a * float(2 * M_PI));

  /* `x' = xcosa + n(n.x)(1-cosa) + (x*n)sina`. */

  mul_v3_v3fl(perp, co, cos_a);
  mul_v3_v3fl(para, ax, dot_v3v3(co, ax) * (1 - cos_a));

  cross_v3_v3v3(cp, ax, co);
  mul_v3_fl(cp, sin_a);

  new_co[0] = para[0] + perp[0] + cp[0];
  new_co[1] = para[1] + perp[1] + cp[1];
  new_co[2] = para[2] + perp[2] + cp[2];
}

static void colorfn(float *out, TexParams *p, bNode * /*node*/, bNodeStack **in, short thread)
{
  float new_co[3], a, ax[3];

  a = tex_input_value(in[1], p, thread);
  tex_input_vec(ax, in[2], p, thread);

  rotate(new_co, a, ax, p->co);

  {
    TexParams np = *p;
    np.co = new_co;
    tex_input_rgba(out, in[0], &np, thread);
  }
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

void register_node_type_tex_rotate()
{
  static bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeRotate", TEX_NODE_ROTATE);
  ntype.ui_name = "Rotate";
  ntype.enum_name_legacy = "ROTATE";
  ntype.nclass = NODE_CLASS_DISTORT;
  bke::node_type_socket_templates(&ntype, inputs, outputs);
  ntype.exec_fn = exec;

  bke::node_register_type(ntype);
}

}  // namespace blender
