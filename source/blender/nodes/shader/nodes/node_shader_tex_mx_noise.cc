/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender::nodes::node_shader_tex_mx_noise_cc {

static void declare_common_output(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Value"_ustr).no_muted_links();
  b.add_output<decl::Color>("Color"_ustr).no_muted_links();
}

static void declare_base_noise(NodeDeclarationBuilder &b, const bool is_2d)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector"_ustr).implicit_field(is_2d ? NODE_DEFAULT_INPUT_POSITION_FIELD :
                                                                  NODE_DEFAULT_INPUT_POSITION_FIELD);
  b.add_input<decl::Float>("Amplitude"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Pivot"_ustr).default_value(0.0f);
  declare_common_output(b);
}

static void declare_fractal_noise(NodeDeclarationBuilder &b, const bool is_2d)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector"_ustr).implicit_field(is_2d ? NODE_DEFAULT_INPUT_POSITION_FIELD :
                                                                  NODE_DEFAULT_INPUT_POSITION_FIELD);
  b.add_input<decl::Float>("Amplitude"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Octaves"_ustr).default_value(3.0f).min(0.0f).max(32.0f);
  b.add_input<decl::Float>("Lacunarity"_ustr).default_value(2.0f);
  b.add_input<decl::Float>("Diminish"_ustr).default_value(0.5f);
  declare_common_output(b);
}

static void declare_cell_noise(NodeDeclarationBuilder &b, const bool is_2d)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector"_ustr).implicit_field(is_2d ? NODE_DEFAULT_INPUT_POSITION_FIELD :
                                                                  NODE_DEFAULT_INPUT_POSITION_FIELD);
  declare_common_output(b);
}

static void declare_worley_noise(NodeDeclarationBuilder &b, const bool is_2d)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector"_ustr).implicit_field(is_2d ? NODE_DEFAULT_INPUT_POSITION_FIELD :
                                                                  NODE_DEFAULT_INPUT_POSITION_FIELD);
  b.add_input<decl::Float>("Jitter"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Style"_ustr).default_value(0.0f).min(0.0f).max(1.0f);
  declare_common_output(b);
}

static void declare_unified_noise(NodeDeclarationBuilder &b, const bool is_2d)
{
  b.is_function_node();
  b.add_input<decl::Vector>("Vector"_ustr).implicit_field(is_2d ? NODE_DEFAULT_INPUT_POSITION_FIELD :
                                                                  NODE_DEFAULT_INPUT_POSITION_FIELD);
  b.add_input<decl::Vector>("Frequency"_ustr).default_value({1.0f, 1.0f, 1.0f});
  b.add_input<decl::Vector>("Offset"_ustr).default_value({0.0f, 0.0f, 0.0f});
  b.add_input<decl::Float>("Type"_ustr).default_value(0.0f).min(0.0f).max(3.0f);
  b.add_input<decl::Float>("Jitter"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Style"_ustr).default_value(0.0f).min(0.0f).max(1.0f);
  b.add_input<decl::Float>("Octaves"_ustr).default_value(3.0f).min(0.0f).max(32.0f);
  b.add_input<decl::Float>("Lacunarity"_ustr).default_value(2.0f);
  b.add_input<decl::Float>("Diminish"_ustr).default_value(0.5f);
  b.add_input<decl::Float>("Out Min"_ustr).default_value(0.0f);
  b.add_input<decl::Float>("Out Max"_ustr).default_value(1.0f);
  b.add_input<decl::Float>("Clamp Output"_ustr).default_value(1.0f).min(0.0f).max(1.0f);
  declare_common_output(b);
}

static void declare_noise_2d(NodeDeclarationBuilder &b)
{
  declare_base_noise(b, true);
}

static void declare_noise_3d(NodeDeclarationBuilder &b)
{
  declare_base_noise(b, false);
}

static void declare_fractal_2d(NodeDeclarationBuilder &b)
{
  declare_fractal_noise(b, true);
}

static void declare_fractal_3d(NodeDeclarationBuilder &b)
{
  declare_fractal_noise(b, false);
}

static void declare_cell_2d(NodeDeclarationBuilder &b)
{
  declare_cell_noise(b, true);
}

static void declare_cell_3d(NodeDeclarationBuilder &b)
{
  declare_cell_noise(b, false);
}

static void declare_worley_2d(NodeDeclarationBuilder &b)
{
  declare_worley_noise(b, true);
}

static void declare_worley_3d(NodeDeclarationBuilder &b)
{
  declare_worley_noise(b, false);
}

static void declare_unified_2d(NodeDeclarationBuilder &b)
{
  declare_unified_noise(b, true);
}

static void declare_unified_3d(NodeDeclarationBuilder &b)
{
  declare_unified_noise(b, false);
}

static void register_mx_node_type(bke::bNodeType &ntype,
                                  const UString idname,
                                  const int legacy_type,
                                  const char *ui_name,
                                  bke::NodeDeclareFunction declare)
{
  common_node_type_base(&ntype, idname, legacy_type);
  ntype.ui_name = ui_name;
  ntype.ui_description = "MaterialX-compatible procedural noise";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = declare;
  blender::bke::node_register_type(ntype);
}

}  // namespace blender::nodes::node_shader_tex_mx_noise_cc

namespace blender {

void register_node_type_sh_tex_mx_noise()
{
  namespace file_ns = nodes::node_shader_tex_mx_noise_cc;

  static bke::bNodeType noise_2d;
  static bke::bNodeType noise_3d;
  static bke::bNodeType fractal_2d;
  static bke::bNodeType fractal_3d;
  static bke::bNodeType cell_2d;
  static bke::bNodeType cell_3d;
  static bke::bNodeType worley_2d;
  static bke::bNodeType worley_3d;
  static bke::bNodeType unified_2d;
  static bke::bNodeType unified_3d;

  file_ns::register_mx_node_type(noise_2d,
                                 "ShaderNodeMxNoise2D"_ustr,
                                 SH_NODE_TEX_MX_NOISE2D,
                                 "MaterialX Noise 2D",
                                 file_ns::declare_noise_2d);
  file_ns::register_mx_node_type(noise_3d,
                                 "ShaderNodeMxNoise3D"_ustr,
                                 SH_NODE_TEX_MX_NOISE3D,
                                 "MaterialX Noise 3D",
                                 file_ns::declare_noise_3d);
  file_ns::register_mx_node_type(fractal_2d,
                                 "ShaderNodeMxFractal2D"_ustr,
                                 SH_NODE_TEX_MX_FRACTAL2D,
                                 "MaterialX Fractal 2D",
                                 file_ns::declare_fractal_2d);
  file_ns::register_mx_node_type(fractal_3d,
                                 "ShaderNodeMxFractal3D"_ustr,
                                 SH_NODE_TEX_MX_FRACTAL3D,
                                 "MaterialX Fractal 3D",
                                 file_ns::declare_fractal_3d);
  file_ns::register_mx_node_type(cell_2d,
                                 "ShaderNodeMxCellNoise2D"_ustr,
                                 SH_NODE_TEX_MX_CELLNOISE2D,
                                 "MaterialX Cell Noise 2D",
                                 file_ns::declare_cell_2d);
  file_ns::register_mx_node_type(cell_3d,
                                 "ShaderNodeMxCellNoise3D"_ustr,
                                 SH_NODE_TEX_MX_CELLNOISE3D,
                                 "MaterialX Cell Noise 3D",
                                 file_ns::declare_cell_3d);
  file_ns::register_mx_node_type(worley_2d,
                                 "ShaderNodeMxWorleyNoise2D"_ustr,
                                 SH_NODE_TEX_MX_WORLEYNOISE2D,
                                 "MaterialX Worley Noise 2D",
                                 file_ns::declare_worley_2d);
  file_ns::register_mx_node_type(worley_3d,
                                 "ShaderNodeMxWorleyNoise3D"_ustr,
                                 SH_NODE_TEX_MX_WORLEYNOISE3D,
                                 "MaterialX Worley Noise 3D",
                                 file_ns::declare_worley_3d);
  file_ns::register_mx_node_type(unified_2d,
                                 "ShaderNodeMxUnifiedNoise2D"_ustr,
                                 SH_NODE_TEX_MX_UNIFIEDNOISE2D,
                                 "MaterialX Unified Noise 2D",
                                 file_ns::declare_unified_2d);
  file_ns::register_mx_node_type(unified_3d,
                                 "ShaderNodeMxUnifiedNoise3D"_ustr,
                                 SH_NODE_TEX_MX_UNIFIEDNOISE3D,
                                 "MaterialX Unified Noise 3D",
                                 file_ns::declare_unified_3d);
}

}  // namespace blender
