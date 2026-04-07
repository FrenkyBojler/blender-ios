/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_memory_utils.hh"
#include "BLI_path_utils.hh"
#include "BLI_rect.h"
#include "BLI_string.h"

#include "DNA_packedFile_types.h"
#include "DNA_vfont_types.h"

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_vfont.hh"

#include "BLF_api.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_string_to_image_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("String"_ustr).optional_label();
  b.add_input<decl::Font>("Font"_ustr)
      .default_value_fn(
          [](const bNode & /*node*/) { return id_cast<ID *>(BKE_vfont_builtin_ensure()); })
      .optional_label();
  b.add_input<decl::Float>("Size"_ustr).default_value(256.0f).min(0.0f);

  b.add_output<decl::Color>("Image"_ustr)
      .structure_type(StructureType::Dynamic)
      .description("The image containing the paragraph of text");
}

using namespace blender::compositor;

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

class StringToImageOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const std::string string = this->get_input("String").get_single_value_default<std::string>();
    const VFont *font = this->get_input("Font").get_single_value_default<VFont *>();
    const float size = this->get_input("Size").get_single_value_default<float>();
    if (string.empty() || !font || size <= 0.0f) {
      this->allocate_default_remaining_outputs();
      return;
    }

    const int font_identifier = load_font(font);
    if (font_identifier == -1) {
      this->allocate_default_remaining_outputs();
      return;
    }
    BLI_SCOPED_DEFER([&]() { BLF_unload_id(font_identifier); });

    rcti box;
    BLF_size(font_identifier, size);
    BLF_boundbox(font_identifier, string.c_str(), string.length(), &box);
    const int width = BLI_rcti_size_x(&box);
    const int height = BLI_rcti_size_y(&box);

    Result &result = this->get_result("Image");
    result.allocate_texture(int2(width, height));
    parallel_for(result.domain().data_size, [&](const int2 texel) {
      result.store_pixel(texel, Color(float4(0.0f, 0.0f, 0.0f, 1.0f)));
    });

    BLF_buffer_col(font_identifier, Color(1.0f, 1.0f, 1.0f, 1.0f));
    BLF_buffer(font_identifier,
               static_cast<float *>(result.cpu_data().data()),
               nullptr,
               width,
               height,
               nullptr);

    BLF_position(font_identifier, -float(box.xmin), -float(box.ymin), 0.0f);
    BLF_draw_buffer(font_identifier, string.c_str(), string.length());

    BLF_buffer(font_identifier, nullptr, nullptr, 0, 0, nullptr);
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new StringToImageOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeStringToImage");
  ntype.ui_name = "String To Image";
  ntype.ui_description = "Generates an image containing the given paragraph of text";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_string_to_image_cc
