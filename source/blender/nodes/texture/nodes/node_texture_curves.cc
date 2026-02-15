/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup texnodes
 */

#include <algorithm>

#include "BKE_colortools.hh"
#include "node_texture_util.hh"
#include "node_util.hh"

namespace blender {

/* **************** CURVE Time  ******************** */

/* custom1 = start-frame, custom2 = end-frame. */
static bke::bNodeSocketTemplate time_outputs[] = {{.type = SOCK_FLOAT, .name = N_("Value")},
                                                  {.type = -1, .name = ""}};

static void time_colorfn(
    float *out, TexParams *p, bNode *node, bNodeStack ** /*in*/, short /*thread*/)
{
  /* stack order output: fac */
  float fac = 0.0f;

  if (node->custom1 < node->custom2) {
    fac = (p->cfra - node->custom1) / float(node->custom2 - node->custom1);
  }

  CurveMapping *mapping = static_cast<CurveMapping *>(node->storage);
  BKE_curvemapping_init(mapping);
  fac = BKE_curvemapping_evaluateF(mapping, 0, fac);
  out[0] = std::clamp(fac, 0.0f, 1.0f);
}

static void time_exec(void *data,
                      int /*thread*/,
                      bNode *node,
                      bNodeExecData *execdata,
                      bNodeStack **in,
                      bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &time_colorfn, static_cast<TexCallData *>(data));
}

static void time_init(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = 1;
  node->custom2 = 250;
  node->storage = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
}

void register_node_type_tex_curve_time()
{
  static bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeCurveTime", TEX_NODE_CURVE_TIME);
  ntype.ui_name = "Time";
  ntype.enum_name_legacy = "CURVE_TIME";
  ntype.nclass = NODE_CLASS_INPUT;
  bke::node_type_socket_templates(&ntype, nullptr, time_outputs);
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Large);
  ntype.initfunc = time_init;
  bke::node_type_storage(ntype, "CurveMapping", node_free_curves, node_copy_curves);
  ntype.init_exec_fn = node_initexec_curves;
  ntype.exec_fn = time_exec;

  bke::node_register_type(ntype);
}

/* **************** CURVE RGB  ******************** */
static bke::bNodeSocketTemplate rgb_inputs[] = {
    {.type = SOCK_RGBA,
     .name = N_("Color"),
     .val1 = 0.0f,
     .val2 = 0.0f,
     .val3 = 0.0f,
     .val4 = 1.0f},
    {.type = -1, .name = ""},
};

static bke::bNodeSocketTemplate rgb_outputs[] = {
    {.type = SOCK_RGBA, .name = N_("Color")},
    {.type = -1, .name = ""},
};

static void rgb_colorfn(float *out, TexParams *p, bNode *node, bNodeStack **in, short thread)
{
  float cin[4];
  tex_input_rgba(cin, in[0], p, thread);

  BKE_curvemapping_evaluateRGBF(static_cast<CurveMapping *>(node->storage), out, cin);
  out[3] = cin[3];
}

static void rgb_exec(void *data,
                     int /*thread*/,
                     bNode *node,
                     bNodeExecData *execdata,
                     bNodeStack **in,
                     bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &rgb_colorfn, static_cast<TexCallData *>(data));
}

static void rgb_init(bNodeTree * /*ntree*/, bNode *node)
{
  node->storage = BKE_curvemapping_add(4, 0.0f, 0.0f, 1.0f, 1.0f);
}

void register_node_type_tex_curve_rgb()
{
  static bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeCurveRGB", TEX_NODE_CURVE_RGB);
  ntype.ui_name = "RGB Curves";
  ntype.enum_name_legacy = "CURVE_RGB";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  bke::node_type_socket_templates(&ntype, rgb_inputs, rgb_outputs);
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Large);
  ntype.initfunc = rgb_init;
  bke::node_type_storage(ntype, "CurveMapping", node_free_curves, node_copy_curves);
  ntype.init_exec_fn = node_initexec_curves;
  ntype.exec_fn = rgb_exec;

  bke::node_register_type(ntype);
}

}  // namespace blender
