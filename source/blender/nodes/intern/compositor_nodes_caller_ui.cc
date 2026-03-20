/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_context.hh"

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_sequence_types.h"

#include "ED_undo.hh"

#include "NOD_composite.hh"
#include "NOD_compositor_nodes_caller_ui.hh"
#include "NOD_compositor_nodes_srna.hh"
#include "NOD_socket_usage_inference.hh"

#include "SEQ_iterator.hh"
#include "SEQ_modifier.hh"
#include "SEQ_modifiertypes.hh"
#include "SEQ_sequencer.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_string_search.hh"

namespace blender::ui {
struct SearchItems;
};

namespace blender::nodes {

namespace {

struct SearchInfo {
  Span<std::string> all_strip_names;
  std::optional<PointerRNA> socket_props_ptr;
};

struct SocketSearchData {
  blender::Strip *strip;
  char strip_modifier_name[MAX_NAME];
  char socket_identifier[MAX_NAME];

  SearchInfo get_search_info(const bContext &C) const;
};
/* This class must not have a destructor, since it is used by buttons and freed with
 * #MEM_delete_void. */
BLI_STATIC_ASSERT(std::is_trivially_destructible_v<SocketSearchData>, "");
struct DrawGroupInputsContext {
  const bContext &C;
  bNodeTree *tree;
  PointerRNA *properties_ptr;
  PointerRNA *bmain_ptr;

  Array<nodes::socket_usage_inference::SocketUsage> input_usages;
  Array<nodes::socket_usage_inference::SocketUsage> output_usages;

  std::function<SocketSearchData(const bNodeTreeInterfaceSocket &)> socket_search_data_fn;
  std::function<void(ui::Layout &, int icon, const bNodeTreeInterfaceSocket &)>
      draw_strip_toggle_fn;

  bool input_is_visible(const bNodeTreeInterfaceSocket &socket) const
  {
    return this->input_usages[this->tree->interface_input_index(socket)].is_visible;
  }

  bool input_is_active(const bNodeTreeInterfaceSocket &socket) const
  {
    return this->input_usages[this->tree->interface_input_index(socket)].is_used;
  }
};
};  // namespace

static void strip_search_add_items(const StringRef str,
                                   const Span<const std::string *> strip_names,
                                   ui::SearchItems &seach_items,
                                   const bool is_first)
{
  static std::string dummy_str;

  /* Any string may be valid, so add the current search string along with the hints. */
  if (!str.is_empty()) {
    bool contained = false;
    for (const std::string *name : strip_names) {
      if (name != nullptr && str == *name) {
        contained = true;
      }
    }
    if (!contained) {
      dummy_str = str;
      ui::search_item_add(&seach_items, str, &dummy_str, ICON_NONE, 0, 0);
    }
  }

  if (str.is_empty() && !is_first) {
    /* Allow clearing the text field when the string is empty, but not on the first pass,
     * or opening a strip name field for the first time would show this search item. */
    dummy_str = str;
    ui::search_item_add(&seach_items, str, &dummy_str, ICON_X, 0, 0);
  }

  /* Don't filter when the menu is first opened, but still run the search
   * so the items are in the same order they will appear in while searching. */
  const StringRef string = is_first ? "" : str;

  ui::string_search::StringSearch<const std::string> search;
  for (const std::string *name : strip_names) {
    search.add(*name, name);
  }

  const Vector<const std::string *> filtered_names = search.query(string);
  for (const std::string *name : filtered_names) {
    if (!ui::search_item_add(
            &seach_items, *name, (void *)name, ICON_NONE, ui::BUT_HAS_SEP_CHAR, 0))
    {
      break;
    }
  }
}

SearchInfo SocketSearchData::get_search_info(const bContext &C) const
{
  Scene *sequencer_scene = CTX_data_sequencer_scene(&C);
  if (sequencer_scene == nullptr) {
    return {};
  }
  Editing *ed = seq::editing_get(sequencer_scene);

  StripModifierData *smd = seq::modifier_find_by_name(this->strip, strip_modifier_name);
  BLI_assert(smd->type == eSeqModifierType_Compositor);
  SequencerCompositorModifierData *modifier_data =
      reinterpret_cast<SequencerCompositorModifierData *>(smd);
  if (!modifier_data->runtime->available_strip_names) {
    Strip *meta = seq::lookup_meta_by_strip(ed, this->strip);
    ListBaseT<Strip> *seqbase = (meta != nullptr) ? &meta->seqbase : &ed->seqbase;
    VectorSet<Strip *> all_strips = seq::query_by_reference(
        strip,
        seqbase,
        [&](Strip *strip_reference, ListBaseT<Strip> *seqbase, VectorSet<Strip *> &strips) {
          for (Strip &strip_test : *seqbase) {
            if (strip_reference == &strip_test) {
              continue;
            }
            if (!strip_test.has_image_output()) {
              continue;
            }
            if (strip_test.right_handle(sequencer_scene) <= strip_reference->left_handle() ||
                strip_test.left_handle() >= strip_reference->right_handle(sequencer_scene))
            {
              continue; /* Not intersecting in time. */
            }
            strips.add(&strip_test);
          }
        });
    Vector<std::string> strip_names;
    for (const Strip *strip : all_strips) {
      strip_names.append(strip->name + 2);
    }
    modifier_data->runtime->available_strip_names.emplace(std::move(strip_names));
  }
  BLI_assert(modifier_data->runtime->available_strip_names.has_value());

  PointerRNA ptr = RNA_pointer_create_discrete(
      &sequencer_scene->id, RNA_SequencerCompositorModifierData, modifier_data);
  PointerRNA properties_ptr = RNA_pointer_get(&ptr, "properties");
  PointerRNA inputs_ptr = RNA_pointer_get(&properties_ptr, "inputs");
  PointerRNA socket_props_ptr = RNA_pointer_get(&inputs_ptr, this->socket_identifier);
  return {modifier_data->runtime->available_strip_names->as_span(), socket_props_ptr};
}

static void strip_name_search_update_fn(
    const bContext *C, void *arg, const char *str, ui::SearchItems *items, const bool is_first)
{
  const SocketSearchData &data = *static_cast<SocketSearchData *>(arg);
  const SearchInfo info = data.get_search_info(*C);

  Set<StringRef> names;
  Vector<const std::string *> strip_names;
  for (const std::string &strip_name : info.all_strip_names) {
    if (names.add(strip_name)) {
      strip_names.append(&strip_name);
    }
  }

  BLI_assert(items);
  strip_search_add_items(str, strip_names.as_span(), *items, is_first);
}

static void strip_name_search_exec_fn(bContext *C, void *data_v, void *item_v)
{
  const SocketSearchData &data = *static_cast<SocketSearchData *>(data_v);
  const std::string *item = static_cast<const std::string *>(item_v);
  if (!item) {
    return;
  }
  SearchInfo info = data.get_search_info(*C);
  if (!info.socket_props_ptr) {
    return;
  }

  RNA_string_set(&*info.socket_props_ptr, "strip_name", item->c_str());
  ED_undo_push(C, "Assign Strip Name");
}

static void add_strip_search_button(DrawGroupInputsContext &ctx,
                                    ui::Layout &layout,
                                    PointerRNA *socket_props_ptr,
                                    const bNodeTreeInterfaceSocket &socket)
{
  ui::Block *block = layout.block();
  ui::Button *but = uiDefIconTextButR(block,
                                      ui::ButtonType::SearchMenu,
                                      ICON_NONE,
                                      "",
                                      0,
                                      0,
                                      10 * UI_UNIT_X, /* Dummy value, replaced by layout system. */
                                      UI_UNIT_Y,
                                      socket_props_ptr,
                                      "strip_name",
                                      0,
                                      StringRef(socket.description));
  button_placeholder_set(but, IFACE_("Strip"));

  /* Using a custom free function make the search not work currently. So make sure this data can be
   * freed with MEM_delete. */
  SocketSearchData *data = static_cast<SocketSearchData *>(
      MEM_new_uninitialized(sizeof(SocketSearchData), __func__));
  *data = ctx.socket_search_data_fn(socket);
  button_func_search_set_results_are_suggestions(but, true);
  button_func_search_set_sep_string(but, UI_MENU_ARROW_SEP);
  button_func_search_set(but,
                         nullptr,
                         strip_name_search_update_fn,
                         data,
                         true,
                         nullptr,
                         strip_name_search_exec_fn,
                         nullptr);

  /* TODO */
  // const bool strip_allowed = true;
  // if (!strip_allowed) {
  //   button_flag_enable(but, ui::BUT_REDALERT);
  // }
}

static void add_strip_search_or_value_button(
    DrawGroupInputsContext &ctx,
    ui::Layout &layout,
    const bNodeTreeInterfaceSocket &socket,
    PointerRNA *socket_props_ptr,
    const std::optional<StringRefNull> use_name = std::nullopt)
{
  const bool show_strip_input = RNA_enum_get(socket_props_ptr, "type") ==
                                int(CompositorNodesInputType::Strip);

  layout.use_property_decorate_set(false);

  ui::Layout &split = layout.split(0.4f, false);
  ui::Layout &name_row = split.row(false);
  name_row.alignment_set(ui::LayoutAlign::Right);

  ui::Layout &prop_row = layout.row(true);
  const StringRefNull socket_name = use_name.has_value() ?
                                        (*use_name) :
                                        (socket.name ? IFACE_(socket.name) : "");
  if (show_strip_input) {
    name_row.label(IFACE_(socket_name), ICON_NONE);
    add_strip_search_button(ctx, split.row(true), socket_props_ptr, socket);
    layout.label("", ICON_BLANK1);
  }
  else {
    const char *name = IFACE_(socket_name.c_str());
    prop_row.prop(socket_props_ptr, "value", UI_ITEM_NONE, name, ICON_NONE);
    layout.decorator(socket_props_ptr, "value", -1);
  }

  ctx.draw_strip_toggle_fn(prop_row, ICON_SEQ_SEQUENCER, socket);
}

/* Drawing the properties manually with #ui::Layout::prop instead of #uiDefAutoButsRNA allows using
 * the node socket identifier for the property names, since they are unique, but also having
 * the correct label displayed in the UI. */
static void draw_property_for_socket(DrawGroupInputsContext &ctx,
                                     ui::Layout &layout,
                                     const bNodeTreeInterfaceSocket &socket,
                                     PointerRNA *socket_props_ptr,
                                     const std::optional<StringRef> parent_name = std::nullopt)
{
  if (!ctx.input_is_visible(socket)) {
    /* The input is not used currently, but it would be used if any menu input is changed.
     * By convention, the input is hidden in this case instead of just grayed out. */
    return;
  }

  ui::Layout &row = layout.row(true);
  row.use_property_decorate_set(true);
  row.active_set(ctx.input_is_active(socket));

  /* Use #ui::Layout::prop_search to draw pointer properties because #ui::Layout::prop would not
   * have enough information about what type of ID to select for editing the values. This is
   * because pointer IDProperties contain no information about their type. */
  const bke::bNodeSocketType *typeinfo = socket.socket_typeinfo();
  const eNodeSocketDatatype type = typeinfo ? typeinfo->type : SOCK_CUSTOM;

  if (ELEM(type, SOCK_GEOMETRY, SOCK_MATRIX, SOCK_BUNDLE, SOCK_CLOSURE)) {
    return;
  }

  std::string name = socket.name ? IFACE_(socket.name) : "";

  /* If the property has a prefix that's the same string as the name of the panel it's in, remove
   * the prefix so it appears less verbose. */
  if (parent_name.has_value()) {
    const StringRef prefix_to_remove = *parent_name;
    const int prefix_size = prefix_to_remove.size();
    const int pos = name.find(prefix_to_remove);
    if (pos == 0 && name.size() > prefix_size && name[prefix_size] == ' ') {
      name = name.substr(prefix_size + 1);
    }
  }

  switch (type) {
    case SOCK_OBJECT: {
      row.prop_search(socket_props_ptr, "value", ctx.bmain_ptr, "objects", name, ICON_OBJECT_DATA);
      break;
    }
    case SOCK_COLLECTION: {
      row.prop_search(
          socket_props_ptr, "value", ctx.bmain_ptr, "collections", name, ICON_OUTLINER_COLLECTION);
      break;
    }
    case SOCK_MATERIAL: {
      row.prop_search(socket_props_ptr, "value", ctx.bmain_ptr, "materials", name, ICON_MATERIAL);
      break;
    }
    case SOCK_TEXTURE: {
      row.prop_search(socket_props_ptr, "value", ctx.bmain_ptr, "textures", name, ICON_TEXTURE);
      break;
    }
    case SOCK_FONT: {
      PropertyRNA *prop = RNA_struct_find_property(ctx.properties_ptr, "value");
      if (prop && RNA_property_type(prop) == PROP_POINTER) {
        template_id(&row,
                    &ctx.C,
                    ctx.properties_ptr,
                    "value",
                    nullptr,
                    "FONT_OT_open",
                    "FONT_OT_unlink",
                    ui::TEMPLATE_ID_FILTER_ALL,
                    false,
                    name);
      }
      else {
        /* #template_id only supports pointer properties currently. Node tools store
         * data-block pointers in strings currently. */
        row.prop_search(ctx.properties_ptr, "value", ctx.bmain_ptr, "fonts", name, ICON_FONT_DATA);
      }
      break;
    }
    case SOCK_SCENE: {
      row.prop_search(ctx.properties_ptr, "value", ctx.bmain_ptr, "scenes", name, ICON_SCENE);
      break;
    }
    case SOCK_TEXT_ID: {
      row.prop_search(ctx.properties_ptr, "value", ctx.bmain_ptr, "texts", name, ICON_TEXT);
      break;
    }
    case SOCK_MASK: {
      row.prop_search(ctx.properties_ptr, "value", ctx.bmain_ptr, "masks", name, ICON_NONE);
      break;
    }
    case SOCK_SOUND: {
      row.prop_search(ctx.properties_ptr, "value", ctx.bmain_ptr, "sounds", name, ICON_SOUND);
      break;
    }
    case SOCK_IMAGE: {
      template_id(&row,
                  &ctx.C,
                  socket_props_ptr,
                  "value",
                  "image.new",
                  "image.open",
                  nullptr,
                  ui::TEMPLATE_ID_FILTER_ALL,
                  false,
                  name);
      break;
    }
    case SOCK_MENU: {
      if (socket.flag & NODE_INTERFACE_SOCKET_MENU_EXPANDED) {
        /* Use a single space when the name is empty to work around a bug with expanded enums. Also
         * see #ui_item_enum_expand_exec. */
        row.prop(socket_props_ptr,
                 "value",
                 ui::ITEM_R_EXPAND,
                 StringRef(name).is_empty() ? " " : name,
                 ICON_NONE);
      }
      else {
        row.prop(socket_props_ptr, "value", UI_ITEM_NONE, name, ICON_NONE);
      }
      break;
    }
    case SOCK_RGBA: {
      add_strip_search_or_value_button(ctx, row, socket, socket_props_ptr);
      /* Adds a spacing at the end of the row. */
      row.label("", ICON_BLANK1);
      break;
    }
    default: {
      row.prop(socket_props_ptr, "value", UI_ITEM_NONE, name, ICON_NONE);
      break;
    }
  }
}

static bool interface_panel_has_socket(DrawGroupInputsContext &ctx,
                                       const bNodeTreeInterfacePanel &interface_panel)
{
  for (const bNodeTreeInterfaceItem *item : interface_panel.items()) {
    if (item->item_type == NODE_INTERFACE_SOCKET) {
      const bNodeTreeInterfaceSocket &socket = *reinterpret_cast<const bNodeTreeInterfaceSocket *>(
          item);
      if (socket.flag & NODE_INTERFACE_SOCKET_HIDE_IN_MODIFIER) {
        continue;
      }
      if (socket.flag & NODE_INTERFACE_SOCKET_INPUT) {
        if (ctx.input_is_visible(socket)) {
          return true;
        }
      }
    }
    else if (item->item_type == NODE_INTERFACE_PANEL) {
      if (interface_panel_has_socket(ctx,
                                     *reinterpret_cast<const bNodeTreeInterfacePanel *>(item)))
      {
        return true;
      }
    }
  }
  return false;
}

static bool interface_panel_affects_output(DrawGroupInputsContext &ctx,
                                           const bNodeTreeInterfacePanel &panel)
{
  for (const bNodeTreeInterfaceItem *item : panel.items()) {
    if (item->item_type == NODE_INTERFACE_SOCKET) {
      const auto &socket = *reinterpret_cast<const bNodeTreeInterfaceSocket *>(item);
      if (socket.flag & NODE_INTERFACE_SOCKET_HIDE_IN_MODIFIER) {
        continue;
      }
      if (!(socket.flag & NODE_INTERFACE_SOCKET_INPUT)) {
        continue;
      }
      if (ctx.input_is_active(socket)) {
        return true;
      }
    }
    else if (item->item_type == NODE_INTERFACE_PANEL) {
      const auto &sub_interface_panel = *reinterpret_cast<const bNodeTreeInterfacePanel *>(item);
      if (interface_panel_affects_output(ctx, sub_interface_panel)) {
        return true;
      }
    }
  }
  return false;
}

static void draw_interface_panel_content(
    DrawGroupInputsContext &ctx,
    ui::Layout &layout,
    const bNodeTreeInterfacePanel &interface_panel,
    const bool skip_first = false,
    const std::optional<StringRef> parent_name = std::nullopt);

static void draw_interface_panel_as_panel(DrawGroupInputsContext &ctx,
                                          ui::Layout &layout,
                                          const bNodeTreeInterfacePanel &interface_panel)
{
  if (!interface_panel_has_socket(ctx, interface_panel)) {
    return;
  }
  PointerRNA panels_ptr = RNA_pointer_get(ctx.properties_ptr, "panels");
  const std::string panel_open_name = fmt::format("open_{}", interface_panel.identifier);
  ui::PanelLayout panel_layout;
  bool skip_first = false;
  /* Check if the panel should have a toggle in the header. */
  const bNodeTreeInterfaceSocket *toggle_socket = interface_panel.header_toggle_socket();
  const StringRef panel_name = interface_panel.name;
  if (toggle_socket && !(toggle_socket->flag & NODE_INTERFACE_SOCKET_HIDE_IN_MODIFIER)) {
    PointerRNA inputs_ptr = RNA_pointer_get(ctx.properties_ptr, "inputs");
    PointerRNA toggle_ptr = RNA_pointer_get(&inputs_ptr, toggle_socket->identifier);
    panel_layout = layout.panel_prop_with_bool_header(
        &ctx.C, &panels_ptr, panel_open_name, &toggle_ptr, "value", IFACE_(panel_name));
    skip_first = true;
  }
  else {
    panel_layout = layout.panel_prop(&ctx.C, &panels_ptr, panel_open_name);
    panel_layout.header->label(IFACE_(panel_name), ICON_NONE);
  }
  if (!interface_panel_affects_output(ctx, interface_panel)) {
    panel_layout.header->active_set(false);
  }
  uiLayoutSetTooltipFunc(
      panel_layout.header,
      [](bContext * /*C*/, void *panel_arg, const StringRef /*tip*/) -> std::string {
        const auto *panel = static_cast<bNodeTreeInterfacePanel *>(panel_arg);
        return StringRef(panel->description);
      },
      const_cast<bNodeTreeInterfacePanel *>(&interface_panel),
      nullptr,
      nullptr);
  if (panel_layout.body) {
    draw_interface_panel_content(ctx, *panel_layout.body, interface_panel, skip_first, panel_name);
  }
}

static void draw_interface_panel_content(DrawGroupInputsContext &ctx,
                                         ui::Layout &layout,
                                         const bNodeTreeInterfacePanel &interface_panel,
                                         const bool skip_first,
                                         const std::optional<StringRef> parent_name)
{
  for (const bNodeTreeInterfaceItem *item : interface_panel.items().drop_front(skip_first ? 1 : 0))
  {
    switch (eNodeTreeInterfaceItemType(item->item_type)) {
      case NODE_INTERFACE_PANEL: {
        const auto &sub_interface_panel = *reinterpret_cast<const bNodeTreeInterfacePanel *>(item);
        draw_interface_panel_as_panel(ctx, layout, sub_interface_panel);
        break;
      }
      case NODE_INTERFACE_SOCKET: {
        const auto &interface_socket = *reinterpret_cast<const bNodeTreeInterfaceSocket *>(item);
        if (interface_socket.flag & NODE_INTERFACE_SOCKET_INPUT) {
          if (!(interface_socket.flag & NODE_INTERFACE_SOCKET_HIDE_IN_MODIFIER)) {
            PointerRNA inputs_ptr = RNA_pointer_get(ctx.properties_ptr, "inputs");
            PointerRNA socket_props_ptr = RNA_pointer_get(&inputs_ptr,
                                                          interface_socket.identifier);
            draw_property_for_socket(
                ctx, layout, interface_socket, &socket_props_ptr, parent_name);
          }
        }
        break;
      }
    }
  }
}

static void draw_mask_input_type_settings(const bContext &C, ui::Layout &layout, PointerRNA *ptr)
{
  Scene *sequencer_scene = CTX_data_sequencer_scene(&C);
  Editing *ed = seq::editing_get(sequencer_scene);

  const int input_mask_type = RNA_enum_get(ptr, "input_mask_type");

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);
  ui::Layout *row = &col.row(true);
  row->prop(ptr, "input_mask_type", ui::ITEM_R_EXPAND, IFACE_("Type"), ICON_NONE);

  if (input_mask_type == STRIP_MASK_INPUT_STRIP) {
    PointerRNA sequences_object = RNA_pointer_create_discrete(
        &sequencer_scene->id, RNA_SequenceEditor, ed);
    col.prop_search(
        ptr, "input_mask_strip", &sequences_object, "strips_all", IFACE_("Mask"), ICON_NONE);
  }
  else {
    col.prop(ptr, "input_mask_id", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    row = &col.row(true);
    row->prop(ptr, "mask_time", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  }
}

static Strip *strip_get_by_modifier(Editing *ed, StripModifierData *smd)
{
  Strip *modifier_strip = nullptr;
  seq::foreach_strip(&ed->seqbase, [&](Strip *strip) {
    if (BLI_findindex(&strip->modifiers, smd) != -1) {
      modifier_strip = strip;
      return false;
    }
    return true;
  });
  return modifier_strip;
}

void draw_compositor_nodes_modifier_ui(const bContext &C,
                                       PointerRNA *modifier_ptr,
                                       ui::Layout &layout)
{
  Main *bmain = CTX_data_main(&C);
  Scene *sequencer_scene = CTX_data_sequencer_scene(&C);
  PointerRNA bmain_ptr = RNA_main_pointer_create(bmain);
  SequencerCompositorModifierData &cmd = *modifier_ptr->data_as<SequencerCompositorModifierData>();
  PointerRNA properties_ptr = RNA_pointer_get(modifier_ptr, "properties");
  DrawGroupInputsContext ctx{C, cmd.node_group, &properties_ptr, &bmain_ptr};

  ctx.socket_search_data_fn = [&](const bNodeTreeInterfaceSocket &io_socket) -> SocketSearchData {
    SocketSearchData data{};
    Strip *strip = strip_get_by_modifier(seq::editing_get(sequencer_scene),
                                         reinterpret_cast<StripModifierData *>(&cmd));
    data.strip = strip;
    STRNCPY_UTF8(data.strip_modifier_name, cmd.modifier.name);
    STRNCPY_UTF8(data.socket_identifier, io_socket.identifier);
    return data;
  };
  ctx.draw_strip_toggle_fn =
      [&](ui::Layout &layout, const int icon, const bNodeTreeInterfaceSocket &io_socket) {
        PointerRNA props = layout.op("sequencer.compositor_strip_modifier_input_strip_toggle",
                                     "",
                                     icon,
                                     wm::OpCallContext::InvokeDefault,
                                     UI_ITEM_NONE);
        RNA_string_set(&props, "modifier_name", cmd.modifier.name);
        RNA_string_set(&props, "input_name", io_socket.identifier);
      };

  layout.use_property_split_set(true);

  if ((cmd.flag & COMPOSITOR_MODIFIER_HIDE_DATABLOCK_SELECTOR) == 0) {
    const char *newop = (cmd.node_group == nullptr) ?
                            "node.new_compositor_sequencer_node_group" :
                            "node.duplicate_compositing_modifier_node_group";
    template_id(&layout, &C, modifier_ptr, "node_group", newop, nullptr, nullptr);
  }

  if (cmd.node_group != nullptr) {
    bNodeTree &tree = *cmd.node_group;
    tree.ensure_interface_cache();
    ctx.input_usages.reinitialize(tree.interface_inputs().size());
    ctx.output_usages.reinitialize(tree.interface_outputs().size());
    nodes::socket_usage_inference::infer_group_interface_inputs_usage(
        tree, *ctx.properties_ptr, ctx.input_usages, ctx.output_usages);
    draw_interface_panel_content(ctx, layout, tree.tree_interface.root_panel);
  }

  if (ui::Layout *mask_input_layout = layout.panel_prop(
          &C, modifier_ptr, "open_mask_input_panel", IFACE_("Mask Input")))
  {
    draw_mask_input_type_settings(C, *mask_input_layout, modifier_ptr);
  }
}

};  // namespace blender::nodes
