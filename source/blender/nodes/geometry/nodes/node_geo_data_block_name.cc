/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_collection_types.h"
#include "DNA_material_types.h"
#include "DNA_sound_types.h"
#include "DNA_text_types.h"
#include "DNA_vfont_types.h"

#include "NOD_rna_define.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_data_block_name_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (node) {
    const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);
    b.add_input(data_type, "Data Block").optional_label();
  }
  b.add_output<decl::String>("Name");
  b.add_output<decl::String>("Library Name");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = SOCK_OBJECT;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const bNode &node = params.node();
  const auto data_type = eNodeSocketDatatype(node.custom1);

  std::string name = "";
  std::string lib_name = "";
  ID *id = nullptr;

  switch (data_type) {
    case SOCK_OBJECT: {
      Object *data_block = params.extract_input<Object *>("Data Block");
      id = &data_block->id;
      break;
    }
    case SOCK_IMAGE: {
      Image *data_block = params.extract_input<Image *>("Data Block");
      id = &data_block->id;
      break;
    }
    case SOCK_COLLECTION: {
      Collection *data_block = params.extract_input<Collection *>("Data Block");
      id = &data_block->id;
      break;
    }
    case SOCK_MATERIAL: {
      Material *data_block = params.extract_input<Material *>("Data Block");
      id = &data_block->id;
      break;
    }
    case SOCK_FONT: {
      VFont *data_block = params.extract_input<VFont *>("Data Block");
      id = &data_block->id;
      break;
    }
    default:
      break;
  }

  if (id == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  name = std::string(id->name + 2);
  params.set_output("Name", std::move(name));

  if (!params.output_is_required("Library Name")) {
    params.set_default_remaining_outputs();
    return;
  }

  Library *lib = id->lib;
  if (lib == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  lib_name = std::string(lib->id.name + 2);
  params.set_output("Library Name", std::move(lib_name));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "data_type",
      "Data Type",
      "",
      rna_enum_node_socket_data_type_items,
      NOD_inline_enum_accessors(custom1),
      SOCK_OBJECT,
      [](bContext * /*C*/, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        return enum_items_filter(
            rna_enum_node_socket_data_type_items, [](const EnumPropertyItem &item) -> bool {
              return ELEM(
                  item.value, SOCK_OBJECT, SOCK_IMAGE, SOCK_COLLECTION, SOCK_MATERIAL, SOCK_FONT);
            });
      });
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeDataBlockName");
  ntype.ui_name = "Data Block Name";
  ntype.ui_description = "Retrieve the name of a data block";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_data_block_name_cc
