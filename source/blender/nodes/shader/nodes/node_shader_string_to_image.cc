/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

#include "BLI_array.hh"
#include "BLI_memory_utils.hh"
#include "BLI_path_utils.hh"
#include "BLI_rect.h"
#include "BLI_string.h"

#include "DNA_node_types.h"
#include "DNA_packedFile_types.h"
#include "DNA_vfont_types.h"

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_vfont.hh"

#include "BLF_api.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_string_to_image_cc {

static const EnumPropertyItem rna_node_shader_string_to_image_horizontal_alignment_items[] = {
    {CMP_NODE_STRING_TO_IMAGE_HORIZONTAL_ALIGNMENT_LEFT,
     "LEFT",
     ICON_ALIGN_LEFT,
     "Left",
     "Align text to the left"},
    {CMP_NODE_STRING_TO_IMAGE_HORIZONTAL_ALIGNMENT_CENTER,
     "CENTER",
     ICON_ALIGN_CENTER,
     "Center",
     "Align text to the center"},
    {CMP_NODE_STRING_TO_IMAGE_HORIZONTAL_ALIGNMENT_RIGHT,
     "RIGHT",
     ICON_ALIGN_RIGHT,
     "Right",
     "Align text to the right"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_node_shader_string_to_image_vertical_alignment_items[] = {
    {CMP_NODE_STRING_TO_IMAGE_VERTICAL_ALIGNMENT_TOP,
     "TOP",
     ICON_ALIGN_TOP,
     "Top",
     "Align text to the top"},
    {CMP_NODE_STRING_TO_IMAGE_VERTICAL_ALIGNMENT_TOP_BASELINE,
     "TOP_BASELINE",
     ICON_ALIGN_TOP,
     "Top Baseline",
     "Align text to the top line's baseline"},
    {CMP_NODE_STRING_TO_IMAGE_VERTICAL_ALIGNMENT_MIDDLE,
     "MIDDLE",
     ICON_ALIGN_MIDDLE,
     "Middle",
     "Align text to the middle"},
    {CMP_NODE_STRING_TO_IMAGE_VERTICAL_ALIGNMENT_BOTTOM_BASELINE,
     "BOTTOM_BASELINE",
     ICON_ALIGN_BOTTOM,
     "Bottom Baseline",
     "Align text to the bottom line's baseline"},
    {CMP_NODE_STRING_TO_IMAGE_VERTICAL_ALIGNMENT_BOTTOM,
     "BOTTOM",
     ICON_ALIGN_BOTTOM,
     "Bottom",
     "Align text to the bottom"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_output<decl::Color>("Color"_ustr).no_muted_links();
  b.add_output<decl::Float>("Alpha"_ustr).no_muted_links();
  b.add_default_layout();
  b.add_input<decl::Vector>("Vector"_ustr).implicit_field(NODE_DEFAULT_INPUT_POSITION_FIELD);

  b.add_input<decl::String>("String"_ustr).optional_label();
  b.add_input<decl::Font>("Font"_ustr)
      .default_value_fn(
          [](const bNode & /*node*/) { return id_cast<ID *>(BKE_vfont_builtin_ensure()); })
      .optional_label();
  b.add_input<decl::Float>("Size"_ustr)
      .default_value(128.0f)
      .subtype(PROP_PIXEL)
      .min(0.0f)
      .description("The height of each line in pixels");

  {
    PanelDeclarationBuilder &panel = b.add_panel("Alignment"_ustr).default_closed(true);
    panel.add_input<decl::Menu>("Horizontal Alignment"_ustr)
        .static_items(rna_node_shader_string_to_image_horizontal_alignment_items)
        .default_value(CMP_NODE_STRING_TO_IMAGE_HORIZONTAL_ALIGNMENT_CENTER)
        .optional_label();
    panel.add_input<decl::Menu>("Vertical Alignment"_ustr)
        .static_items(rna_node_shader_string_to_image_vertical_alignment_items)
        .default_value(CMP_NODE_STRING_TO_IMAGE_VERTICAL_ALIGNMENT_MIDDLE)
        .optional_label();
  }

  {
    PanelDeclarationBuilder &panel = b.add_panel("Wrap"_ustr).default_closed(true);
    panel.add_input<decl::Bool>("Wrap"_ustr)
        .default_value(true)
        .panel_toggle()
        .description("Wrap text into new lines if it exceeds the specified width");
    panel.add_input<decl::Int>("Width"_ustr, "Wrap Width"_ustr)
        .default_value(1920)
        .min(0)
        .subtype(PROP_PIXEL)
        .description(
            "The maximum width of each line in pixels. Lines with larger widths will be wrapped "
            "into new lines");
  }
}

struct StringImageBuffer {
  int width = 0;
  int height = 0;
  float *pixels = nullptr;
};

static int load_font(const VFont *font)
{
  if (!font || BKE_vfont_is_builtin(font)) {
    return BLF_load_default(true);
  }

  if (font->packedfile != nullptr) {
    char name[MAX_ID_FULL_NAME];
    BKE_id_full_name_get(name, &font->id, 0);
    return BLF_load_mem_unique(
        name, static_cast<const uchar *>(font->packedfile->data), font->packedfile->size);
  }

  char file_path[FILE_MAX];
  STRNCPY(file_path, font->filepath);
  BLI_path_abs(file_path, ID_BLEND_PATH_FROM_GLOBAL(&font->id));
  return BLF_load_unique(file_path);
}

static float compute_draw_horizontal_position(
    const int start_offset,
    const int line_width,
    const int total_width,
    const CMPNodeStringToImageHorizontalAlignment alignment)
{
  switch (alignment) {
    case CMP_NODE_STRING_TO_IMAGE_HORIZONTAL_ALIGNMENT_LEFT:
      return float(-start_offset);
    case CMP_NODE_STRING_TO_IMAGE_HORIZONTAL_ALIGNMENT_CENTER:
      return -start_offset + (total_width - line_width) / 2.0f;
    case CMP_NODE_STRING_TO_IMAGE_HORIZONTAL_ALIGNMENT_RIGHT:
      return float(-start_offset + total_width - line_width);
  }

  BLI_assert_unreachable();
  return float(-start_offset);
}

static float compute_draw_vertical_position(const int lines_count,
                                            const int line_index,
                                            const int line_height,
                                            const int descender)
{
  return (lines_count - 1 - line_index) * line_height - float(descender);
}

static std::optional<StringImageBuffer> rasterize_string_image(
    const StringRef string,
    const VFont *font,
    const float size,
    const CMPNodeStringToImageHorizontalAlignment horizontal_alignment,
    const std::optional<int> wrap_width)
{
  if (string.is_empty() || size <= 0.0f) {
    return std::nullopt;
  }

  const int font_identifier = load_font(font);
  if (font_identifier == -1) {
    return std::nullopt;
  }
  BLI_SCOPED_DEFER([&]() { BLF_unload_id(font_identifier); });

  BLF_size(font_identifier, size);

  Vector<StringRef> lines = BLF_string_wrap(
      font_identifier, string, wrap_width.value_or(-1), BLFWrapMode::Typographical);
  if (lines.is_empty()) {
    return std::nullopt;
  }

  int total_width = 0;
  Array<int> line_widths(lines.size());
  int start_offset = std::numeric_limits<int>::max();
  for (const int64_t i : lines.index_range()) {
    rcti line_bounding_box;
    BLF_boundbox(font_identifier, lines[i].data(), lines[i].size(), &line_bounding_box);
    line_widths[i] = BLI_rcti_size_x(&line_bounding_box);
    total_width = std::max(total_width, line_widths[i]);
    start_offset = std::min(start_offset, line_bounding_box.xmin);
  }

  const int line_height = BLF_height_max(font_identifier);
  const int total_height = line_height * lines.size();
  if (total_width <= 0 || total_height <= 0) {
    return std::nullopt;
  }

  Array<float> alpha_pixels(size_t(total_width) * total_height, 0.0f);
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  BLF_buffer_col(font_identifier, white);
  BLF_buffer(font_identifier, alpha_pixels.data(), nullptr, total_width, total_height, 1, nullptr);

  const int descender = BLF_descender(font_identifier);
  for (const int64_t i : lines.index_range()) {
    const float vertical_position = compute_draw_vertical_position(
        lines.size(), i, line_height, descender);
    const float horizontal_position = compute_draw_horizontal_position(
        start_offset, line_widths[i], total_width, horizontal_alignment);
    BLF_position(font_identifier, horizontal_position, vertical_position, 0.0f);
    BLF_draw_buffer(font_identifier, lines[i].data(), lines[i].size());
  }

  BLF_buffer(font_identifier, nullptr, nullptr, 0, 0, 1, nullptr);

  float *pixels = MEM_new_array_uninitialized<float>(
      size_t(total_width) * total_height * 4, __func__);
  for (const int64_t i : alpha_pixels.index_range()) {
    const float mask = alpha_pixels[i];
    pixels[i * 4 + 0] = mask;
    pixels[i * 4 + 1] = mask;
    pixels[i * 4 + 2] = mask;
    pixels[i * 4 + 3] = mask;
  }

  return StringImageBuffer{total_width, total_height, pixels};
}

static const bNodeSocket *find_input_socket(const bNode &node, const StringRefNull identifier)
{
  return bke::node_find_socket(node, SOCK_IN, identifier);
}

static StringRefNull socket_string_value(const bNode &node, const StringRefNull identifier)
{
  const bNodeSocket *socket = find_input_socket(node, identifier);
  if (!socket) {
    return "";
  }
  return socket->default_value_typed<bNodeSocketValueString>()->value;
}

static const VFont *socket_font_value(const bNode &node, const StringRefNull identifier)
{
  const bNodeSocket *socket = find_input_socket(node, identifier);
  if (!socket) {
    return BKE_vfont_builtin_ensure();
  }
  const VFont *font = socket->default_value_typed<bNodeSocketValueFont>()->value;
  return font ? font : BKE_vfont_builtin_ensure();
}

static float socket_float_value(const bNode &node, const StringRefNull identifier)
{
  const bNodeSocket *socket = find_input_socket(node, identifier);
  return socket ? socket->default_value_typed<bNodeSocketValueFloat>()->value : 0.0f;
}

static int socket_int_value(const bNode &node, const StringRefNull identifier)
{
  const bNodeSocket *socket = find_input_socket(node, identifier);
  return socket ? socket->default_value_typed<bNodeSocketValueInt>()->value : 0;
}

static bool socket_bool_value(const bNode &node, const StringRefNull identifier)
{
  const bNodeSocket *socket = find_input_socket(node, identifier);
  return socket ? socket->default_value_typed<bNodeSocketValueBoolean>()->value : false;
}

static int socket_menu_value(const bNode &node, const StringRefNull identifier)
{
  const bNodeSocket *socket = find_input_socket(node, identifier);
  return socket ? socket->default_value_typed<bNodeSocketValueMenu>()->value : 0;
}

static void node_shader_buts_string_to_image(ui::Layout &layout,
                                             bContext * /*C*/,
                                             PointerRNA *ptr)
{
  layout.prop(ptr, "extension", ui::ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
}

static float horizontal_alignment_offset(
    const CMPNodeStringToImageHorizontalAlignment horizontal_alignment)
{
  switch (horizontal_alignment) {
    case CMP_NODE_STRING_TO_IMAGE_HORIZONTAL_ALIGNMENT_LEFT:
      return -0.5f;
    case CMP_NODE_STRING_TO_IMAGE_HORIZONTAL_ALIGNMENT_CENTER:
      return 0.0f;
    case CMP_NODE_STRING_TO_IMAGE_HORIZONTAL_ALIGNMENT_RIGHT:
      return 0.5f;
  }

  BLI_assert_unreachable();
  return 0.0f;
}

static float vertical_alignment_offset(
    const CMPNodeStringToImageVerticalAlignment vertical_alignment)
{
  switch (vertical_alignment) {
    case CMP_NODE_STRING_TO_IMAGE_VERTICAL_ALIGNMENT_TOP:
    case CMP_NODE_STRING_TO_IMAGE_VERTICAL_ALIGNMENT_TOP_BASELINE:
      return 0.5f;
    case CMP_NODE_STRING_TO_IMAGE_VERTICAL_ALIGNMENT_MIDDLE:
      return 0.0f;
    case CMP_NODE_STRING_TO_IMAGE_VERTICAL_ALIGNMENT_BOTTOM_BASELINE:
    case CMP_NODE_STRING_TO_IMAGE_VERTICAL_ALIGNMENT_BOTTOM:
      return -0.5f;
  }

  BLI_assert_unreachable();
  return 0.0f;
}

static int gpu_shader_string_to_image(GPUMaterial *mat,
                                      bNode *node,
                                      bNodeExecData * /*execdata*/,
                                      GPUNodeStack *in,
                                      GPUNodeStack *out)
{
  const StringRefNull string = socket_string_value(*node, "String");
  const VFont *font = socket_font_value(*node, "Font");
  const float size = socket_float_value(*node, "Size");
  const auto horizontal_alignment = CMPNodeStringToImageHorizontalAlignment(
      socket_menu_value(*node, "Horizontal Alignment"));
  const auto vertical_alignment = CMPNodeStringToImageVerticalAlignment(
      socket_menu_value(*node, "Vertical Alignment"));
  const bool use_wrap = socket_bool_value(*node, "Wrap");
  const std::optional<int> wrap_width = use_wrap ?
                                           std::optional<int>(std::max(
                                               0, socket_int_value(*node, "Wrap Width"))) :
                                           std::nullopt;

  std::optional<StringImageBuffer> string_image = rasterize_string_image(
      string, font, size, horizontal_alignment, wrap_width);
  if (!string_image) {
    GPU_link(mat, "set_rgba_zero", &out[0].link);
    GPU_link(mat, "set_value_zero", &out[1].link);
    return true;
  }

  GPUNodeLink **texco = &in[0].link;
  if (!*texco) {
    *texco = GPU_attribute(mat, CD_AUTO_FROM_NAME, "");
    node_shader_gpu_bump_tex_coord(mat, node, texco);
  }

  GPUSamplerState sampler_state = GPUSamplerState::default_sampler();
  sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER;
  sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER;
  sampler_state.filtering = GPU_SAMPLER_FILTERING_LINEAR;

  switch (node->custom1) {
    case SHD_IMAGE_EXTENSION_EXTEND:
      sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_EXTEND;
      sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_EXTEND;
      break;
    case SHD_IMAGE_EXTENSION_REPEAT:
      sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_REPEAT;
      sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_REPEAT;
      break;
    case SHD_IMAGE_EXTENSION_CLIP:
      sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER;
      sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER;
      break;
    case SHD_IMAGE_EXTENSION_MIRROR:
      sampler_state.extend_x = GPU_SAMPLER_EXTEND_MODE_MIRRORED_REPEAT;
      sampler_state.extend_yz = GPU_SAMPLER_EXTEND_MODE_MIRRORED_REPEAT;
      break;
    default:
      break;
  }

  const float alignment_offset[3] = {
      horizontal_alignment_offset(horizontal_alignment),
      vertical_alignment_offset(vertical_alignment),
      0.0f,
  };
  GPUNodeLink *aligned_texco = nullptr;
  GPU_link(
      mat, "node_string_to_image_align", *texco, GPU_constant(alignment_offset), &aligned_texco);

  GPUNodeLink *gpu_image = GPU_image_generated(mat,
                                               string_image->width,
                                               string_image->height,
                                               string_image->pixels,
                                               sampler_state);

  GPUNodeStack tex_image_in[2] = {};
  tex_image_in[0] = in[0];
  tex_image_in[0].link = aligned_texco;
  tex_image_in[1].end = true;
  return GPU_stack_link(mat, node, "node_tex_image_linear", tex_image_in, out, gpu_image);
}

}  // namespace nodes::node_shader_string_to_image_cc

static void node_shader_init_string_to_image(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = SHD_IMAGE_EXTENSION_CLIP;
}

void register_node_type_sh_string_to_image()
{
  namespace file_ns = nodes::node_shader_string_to_image_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeStringToImage"_ustr);
  ntype.ui_name = "String To Image";
  ntype.ui_description = "Generate a texture containing the given paragraph of text";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.draw_buttons = file_ns::node_shader_buts_string_to_image;
  ntype.declare = file_ns::node_declare;
  ntype.initfunc = node_shader_init_string_to_image;
  ntype.gpu_fn = file_ns::gpu_shader_string_to_image;

  bke::node_register_type(ntype);
}

}  // namespace blender
