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

template<typename T> class DataBlockNameFunction : public mf::MultiFunction {
 public:
  DataBlockNameFunction()
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Data Block Name", signature};

      builder.single_input<T *>("Data Block");
      builder.single_output<std::string>("Name");
      builder.single_output<std::string>("Library Name", mf::ParamFlag::SupportsUnusedOutput);
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<T *> &data_blocks = params.readonly_single_input<T *>(0, "Data Block");
    MutableSpan<std::string> names = params.uninitialized_single_output<std::string>(1, "Name");
    MutableSpan<std::string> library_names =
        params.uninitialized_single_output_if_required<std::string>(2, "Library Name");

    const bool socket_used = !library_names.is_empty();

    mask.foreach_index_optimized<int64_t>([&](const int64_t i) {
      const T *data_block = data_blocks[i];

      if (data_block == nullptr) {
        new (&names[i]) std::string("");
        if (socket_used) {
          new (&library_names[i]) std::string("");
        }
        return;
      }

      const ID *id = id_cast<const ID *>(data_block);
      new (&names[i]) std::string(BKE_id_name(*id));

      if (socket_used) {
        if (id->lib == nullptr) {
          new (&library_names[i]) std::string("");
        }
        else {
          new (&library_names[i]) std::string(BKE_id_name(id->lib->id));
        }
      }
    });
  }
};

static const mf::MultiFunction *get_multi_function(const bNode &node)
{
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(node.custom1);

  return to_static_data_block_type(data_type, [&]<typename T>() -> const mf::MultiFunction * {
    static DataBlockNameFunction<T> fn;
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
