/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup texnodes
 */

#include "node_texture_util.hh"

#include "BKE_material.hh"
#include "BKE_node_runtime.hh"

#include "BLI_math_vector.h"

#include "DNA_material_types.h"

#include "RE_texture.h"

namespace blender {

static bke::bNodeSocketTemplate inputs[] = {
    {.type = SOCK_RGBA,
     .name = N_("Color1"),
     .val1 = 1.0f,
     .val2 = 1.0f,
     .val3 = 1.0f,
     .val4 = 1.0f},
    {.type = SOCK_RGBA,
     .name = N_("Color2"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 1.0f},
    {.type = -1, .name = ""},
};

static bke::bNodeSocketTemplate outputs[] = {
    {.type = SOCK_RGBA, .name = N_("Color")},
    {.type = -1, .name = ""},
};

static void colorfn(float *out, TexParams *p, bNode *node, bNodeStack **in, short thread)
{
  Tex *nodetex = id_cast<Tex *>(node->id);
  static float red[] = {1, 0, 0, 1};
  static float white[] = {1, 1, 1, 1};
  float co[3];

  copy_v3_v3(co, p->co);

  if (node->custom2 || node->runtime->need_exec == 0) {
    /* this node refers to its own texture tree! */
    copy_v4_v4(out, (fabsf(co[0] - co[1]) < 0.01f) ? white : red);
  }
  else if (nodetex) {
    TexResult texres;
    int textype;
    float col1[4], col2[4];

    tex_input_rgba(col1, in[0], p, thread);
    tex_input_rgba(col2, in[1], p, thread);

    textype = multitex_nodes(nodetex, co, &texres, thread, 0, p->mtex, nullptr);

    if (textype & TEX_RGB) {
      copy_v4_v4(out, texres.trgba);
    }
    else {
      copy_v4_v4(out, col1);
      ramp_blend(MA_RAMP_BLEND, out, texres.tin, col2);
    }
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

void register_node_type_tex_texture()
{
  static bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeTexture", TEX_NODE_TEXTURE);
  ntype.ui_name = "Texture";
  ntype.enum_name_legacy = "TEXTURE";
  ntype.nclass = NODE_CLASS_INPUT;
  bke::node_type_socket_templates(&ntype, inputs, outputs);
  ntype.exec_fn = exec;
  ntype.flag |= NODE_PREVIEW;

  bke::node_register_type(ntype);
}

}  // namespace blender
