/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup texnodes
 */

#include "BKE_colorband.hh"
#include "IMB_colormanagement.hh"
#include "node_texture_util.hh"
#include "node_util.hh"

namespace blender {

/* **************** VALTORGB ******************** */
static bke::bNodeSocketTemplate valtorgb_in[] = {
    {.type = SOCK_FLOAT,
     .name = N_("Fac"),
     .val1 = 0.5f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 0.0f,
     .min = 0.0f,
     .max = 1.0f,
     .subtype = PROP_FACTOR},
    {.type = -1, .name = ""},
};
static bke::bNodeSocketTemplate valtorgb_out[] = {
    {.type = SOCK_RGBA, .name = N_("Color")},
    {.type = -1, .name = ""},
};

static void valtorgb_colorfn(float *out, TexParams *p, bNode *node, bNodeStack **in, short thread)
{
  if (node->storage) {
    float fac = tex_input_value(in[0], p, thread);

    BKE_colorband_evaluate(static_cast<const ColorBand *>(node->storage), fac, out);
  }
}

static void valtorgb_exec(void *data,
                          int /*thread*/,
                          bNode *node,
                          bNodeExecData *execdata,
                          bNodeStack **in,
                          bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &valtorgb_colorfn, static_cast<TexCallData *>(data));
}

static void valtorgb_init(bNodeTree * /*ntree*/, bNode *node)
{
  node->storage = BKE_colorband_add(true);
}

void register_node_type_tex_valtorgb()
{
  static bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeValToRGB", TEX_NODE_VALTORGB);
  ntype.ui_name = "Color Ramp";
  ntype.enum_name_legacy = "VALTORGB";
  ntype.nclass = NODE_CLASS_CONVERTER;
  bke::node_type_socket_templates(&ntype, valtorgb_in, valtorgb_out);
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Large);
  ntype.initfunc = valtorgb_init;
  bke::node_type_storage(
      ntype, "ColorBand", node_free_standard_storage, node_copy_standard_storage);
  ntype.exec_fn = valtorgb_exec;

  bke::node_register_type(ntype);
}

/* **************** RGBTOBW ******************** */
static bke::bNodeSocketTemplate rgbtobw_in[] = {
    {.type = SOCK_RGBA,
     .name = N_("Color"),
     .val1 = 0.5f,
     .val2 = 0.5f,
     .val3 = 0.5f,
     .val4 = 1.0f,
     .min = 0.0f,
     .max = 1.0f},
    {.type = -1, .name = ""},
};
static bke::bNodeSocketTemplate rgbtobw_out[] = {
    {.type = SOCK_FLOAT,
     .name = N_("Val"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 1.0f,
     .min = 0.0f,
     .max = 1.0f},
    {.type = -1, .name = ""},
};

static void rgbtobw_valuefn(
    float *out, TexParams *p, bNode * /*node*/, bNodeStack **in, short thread)
{
  float cin[4];
  tex_input_rgba(cin, in[0], p, thread);
  *out = IMB_colormanagement_get_luminance(cin);
}

static void rgbtobw_exec(void *data,
                         int /*thread*/,
                         bNode *node,
                         bNodeExecData *execdata,
                         bNodeStack **in,
                         bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &rgbtobw_valuefn, static_cast<TexCallData *>(data));
}

void register_node_type_tex_rgbtobw()
{
  static bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeRGBToBW", TEX_NODE_RGBTOBW);
  ntype.ui_name = "RGB to BW";
  ntype.enum_name_legacy = "RGBTOBW";
  ntype.nclass = NODE_CLASS_CONVERTER;
  bke::node_type_socket_templates(&ntype, rgbtobw_in, rgbtobw_out);
  ntype.exec_fn = rgbtobw_exec;

  bke::node_register_type(ntype);
}

}  // namespace blender
