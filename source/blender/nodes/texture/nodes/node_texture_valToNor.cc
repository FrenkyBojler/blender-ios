/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup texnodes
 */

#include "node_texture_util.hh"

namespace blender {

static bke::bNodeSocketTemplate inputs[] = {
    {.type = SOCK_FLOAT,
     .name = N_("Val"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 1.0f,
     .min = 0.0f,
     .max = 1.0f,
     .subtype = PROP_NONE},
    {.type = SOCK_FLOAT,
     .name = N_("Nabla"),
     .val1 = 0.025f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = 0.001f,
     .max = 0.1f,
     .subtype = PROP_UNSIGNED},
    {.type = -1, .name = ""},
};

static bke::bNodeSocketTemplate outputs[] = {
    {.type = SOCK_VECTOR, .name = N_("Normal")},
    {.type = -1, .name = ""},
};

static void normalfn(float *out, TexParams *p, bNode * /*node*/, bNodeStack **in, short thread)
{
  float new_co[3];
  const float *co = p->co;

  float nabla = tex_input_value(in[1], p, thread);
  float val;
  float nor[3];

  TexParams np = *p;
  np.co = new_co;

  val = tex_input_value(in[0], p, thread);

  new_co[0] = co[0] + nabla;
  new_co[1] = co[1];
  new_co[2] = co[2];
  nor[0] = tex_input_value(in[0], &np, thread);

  new_co[0] = co[0];
  new_co[1] = co[1] + nabla;
  nor[1] = tex_input_value(in[0], &np, thread);

  new_co[1] = co[1];
  new_co[2] = co[2] + nabla;
  nor[2] = tex_input_value(in[0], &np, thread);

  out[0] = val - nor[0];
  out[1] = val - nor[1];
  out[2] = val - nor[2];
}
static void exec(void *data,
                 int /*thread*/,
                 bNode *node,
                 bNodeExecData *execdata,
                 bNodeStack **in,
                 bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &normalfn, static_cast<TexCallData *>(data));
}

void register_node_type_tex_valtonor()
{
  static bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeValToNor", TEX_NODE_VALTONOR);
  ntype.ui_name = "Value to Normal";
  ntype.enum_name_legacy = "VALTONOR";
  ntype.nclass = NODE_CLASS_CONVERTER;
  bke::node_type_socket_templates(&ntype, inputs, outputs);
  ntype.exec_fn = exec;

  bke::node_register_type(ntype);
}

}  // namespace blender
