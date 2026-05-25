/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_lib_id.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_collection_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_sound_types.h"
#include "DNA_text_types.h"
#include "DNA_vfont_types.h"

#include "NOD_rna_define.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_function_util.hh"

namespace blender::nodes::node_fn_data_block_name_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  const bNode *node = b.node_or_null();
  if (node) {
    const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);
    b.add_input(data_type, "Data Block"_ustr).optional_label();
  }
  b.add_output<decl::String>("Name"_ustr);
  b.add_output<decl::String>("Library Name"_ustr);
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

/* old
static void node_geo_exec(GeoNodeExecParams params)
{
  const bNode &node = params.node();
  const auto data_type = eNodeSocketDatatype(node.custom1);
  ID *id = nullptr;

  switch (data_type) {
    case SOCK_OBJECT: {
      id = &params.extract_input<Object *>("Data Block"_ustr)->id;
      break;
    }
    case SOCK_IMAGE: {
      id = &params.extract_input<Image *>("Data Block"_ustr)->id;
      break;
    }
    case SOCK_COLLECTION: {
      id = &params.extract_input<Collection *>("Data Block"_ustr)->id;
      break;
    }
    case SOCK_MATERIAL: {
      id = &params.extract_input<Material *>("Data Block"_ustr)->id;
      break;
    }
    case SOCK_FONT: {
      id = &params.extract_input<VFont *>("Data Block"_ustr)->id;
      break;
    }
    case SOCK_SOUND: {
      id = &params.extract_input<bSound *>("Data Block"_ustr)->id;
      break;
    }
    default:
      break;
  }

  if (id == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  params.set_output<std::string>("Name"_ustr, BKE_id_name(*id));

  if (!params.output_is_required("Library Name"_ustr)) {
    params.set_default_remaining_outputs();
    return;
  }

  Library *lib = id->lib;
  if (lib == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  params.set_output<std::string>("Library Name"_ustr, BKE_id_name(lib->id));
}*/
/*
static std::string data_blocks_are_equal(const ID *a)
{
  std::string b = "";
  return b;
}*/

template<typename Fn>
static auto to_static_data_block_type(const eNodeSocketDatatype socket_type, Fn &&fn)
{
  switch (socket_type) {
    case SOCK_OBJECT:
      return fn.template operator()<Object>();
    case SOCK_IMAGE:
      return fn.template operator()<Image>();
    case SOCK_COLLECTION:
      return fn.template operator()<Collection>();
    case SOCK_MATERIAL:
      return fn.template operator()<Material>();
    case SOCK_FONT:
      return fn.template operator()<VFont>();
    case SOCK_SOUND:
      return fn.template operator()<bSound>();
    default:
      BLI_assert_unreachable();
      return fn.template operator()<Object>();
  }
}
static const mf::MultiFunction *get_multi_function(const bNode &node)
{
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(node.custom1);

  return to_static_data_block_type(data_type, [&]<typename T>() -> const mf::MultiFunction * {
    static auto fn = mf::build::SI1_SO2<T *, std::string, std::string>(
        "Data Block Name",
        [](const T *data_block, std::string &name, std::string &library_name) {
          if (data_block == nullptr) {
            name = "";
            library_name = "";
            return;
          }

          const ID *id = id_cast<const ID *>(data_block);

          name = BKE_id_name(*id);

          if (id->lib != nullptr) {
            library_name = BKE_id_name(id->lib->id);
          }
          else {
            library_name = "";
          }
        },
        mf::build::exec_presets::Simple{});
    return &fn;
  });
}

static void node_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const mf::MultiFunction *fn = get_multi_function(builder.node());
  builder.set_matching_fn(fn);
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
        return enum_items_filter(rna_enum_node_socket_data_type_items,
                                 [](const EnumPropertyItem &item) -> bool {
                                   return ELEM(item.value,
                                               SOCK_OBJECT,
                                               SOCK_IMAGE,
                                               SOCK_COLLECTION,
                                               SOCK_MATERIAL,
                                               SOCK_FONT,
                                               SOCK_SOUND);
                                 });
      });
}

static void node_register()
{
  static bke::bNodeType ntype;

  fn_cmp_node_type_base(&ntype, "FunctionNodeDataBlockName"_ustr);
  ntype.ui_name = "Data Block Name";
  ntype.ui_description = "Retrieve the name of a data block";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.build_multi_function = node_build_multi_function;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_fn_data_block_name_cc
