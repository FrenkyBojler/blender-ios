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
     .name = N_("Texture"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 1.0f},
    {.type = SOCK_VECTOR,
     .name = N_("Coordinates"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = -1.0f,
     .max = 1.0f,
     .subtype = PROP_NONE},
    {.type = -1, .name = ""},
};
static bke::bNodeSocketTemplate outputs[] = {
    {.type = SOCK_RGBA, .name = N_("Texture")},
    {.type = -1, .name = ""},
};

static void colorfn(float *out, TexParams *p, bNode * /*node*/, bNodeStack **in, short thread)
{
  TexParams np = *p;
  float new_co[3];
  np.co = new_co;

  tex_input_vec(new_co, in[1], p, thread);
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

void register_node_type_tex_at()
{
  static bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeAt", TEX_NODE_AT);
  ntype.ui_name = "At";
  ntype.enum_name_legacy = "AT";
  ntype.nclass = NODE_CLASS_DISTORT;
  bke::node_type_socket_templates(&ntype, inputs, outputs);
  bke::node_type_size(ntype, 140, 100, 320);
  ntype.exec_fn = exec;

  bke::node_register_type(ntype);
}

}  // namespace blender
