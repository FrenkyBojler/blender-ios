/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup texnodes
 */

#include "node_texture_util.hh"

namespace blender {

static bke::bNodeSocketTemplate inputs[] = {
    {.type = SOCK_RGBA,
     .name = N_("Color"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 1.0f},
    {.type = SOCK_VECTOR,
     .name = N_("Offset"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = -10000.0f,
     .max = 10000.0f,
     .subtype = PROP_TRANSLATION},
    {.type = -1, .name = ""},
};

static bke::bNodeSocketTemplate outputs[] = {
    {.type = SOCK_RGBA, .name = N_("Color")},
    {.type = -1, .name = ""},
};

static void colorfn(float *out, TexParams *p, bNode * /*node*/, bNodeStack **in, short thread)
{
  float offset[3], new_co[3];
  TexParams np = *p;
  np.co = new_co;

  tex_input_vec(offset, in[1], p, thread);

  new_co[0] = p->co[0] + offset[0];
  new_co[1] = p->co[1] + offset[1];
  new_co[2] = p->co[2] + offset[2];

  tex_input_rgba(out, in[0], &np, thread);
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

void register_node_type_tex_translate()
{
  static bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeTranslate", TEX_NODE_TRANSLATE);
  ntype.ui_name = "Translate";
  ntype.enum_name_legacy = "TRANSLATE";
  ntype.nclass = NODE_CLASS_DISTORT;
  bke::node_type_socket_templates(&ntype, inputs, outputs);
  ntype.exec_fn = exec;

  bke::node_register_type(ntype);
}

}  // namespace blender
