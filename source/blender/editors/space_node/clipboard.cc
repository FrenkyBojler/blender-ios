/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_space_types.h"

#include "BLI_listbase.h"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_main_idmap.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_report.hh"

// todo(habib): cleanup
#include "BKE_blender_copybuffer.hh"
#include "BKE_blendfile.hh"
// for get copybuffer path. todo(habib(): verify is necessary
#include "BKE_appdir.hh"
#include "BLI_path_utils.hh"
#include "BLO_readfile.hh"

#include "ED_node.hh"
#include "ED_render.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "DEG_depsgraph_build.hh"

#include "node_intern.hh"

namespace blender::ed::space_node {

/* -------------------------------------------------------------------- */
/** \name Local Utilities
 * \{ */

static void node_copybuffer_filepath_get(char filepath[FILE_MAX], size_t filepath_maxncpy)
{
  BLI_path_join(filepath, filepath_maxncpy, BKE_tempdir_base(), "copybuffer_nodes.blend");
}

static bool node_copy_local(bNodeTree &from_tree,
                            bNodeTree &to_tree,
                            const bool allow_duplicate_names,
                            const float2 offset,
                            ReportList *reports)
{
  node_select_paired(from_tree);

  Map<const bNode *, bNode *> node_map;
  Map<const bNodeSocket *, bNodeSocket *> socket_map;
  const char *disabled_hint = nullptr;

  for (bNode *node : get_selected_nodes(from_tree)) {
    if (!node->typeinfo->poll_instance ||
        node->typeinfo->poll_instance(node, &to_tree, &disabled_hint))
    {
      bNode *new_node = bke::node_copy_with_mapping(&to_tree,
                                                    *node,
                                                    LIB_ID_COPY_DEFAULT,
                                                    std::nullopt,
                                                    std::nullopt,
                                                    socket_map,
                                                    allow_duplicate_names);
      node_map.add_new(node, new_node);
      new_node->location[0] += offset.x;
      new_node->location[1] += offset.y;
    }
    else {
      if (disabled_hint) {
        BKE_reportf(reports,
                    RPT_ERROR,
                    "Cannot add node %s into node tree %s: %s",
                    node->name,
                    to_tree.id.name + 2,
                    disabled_hint);
      }
      else {
        BKE_reportf(reports,
                    RPT_ERROR,
                    "Cannot add node %s into node tree %s",
                    node->name,
                    to_tree.id.name + 2);
      }
    }
  }

  if (node_map.is_empty()) {
    return false;
  }

  for (bNode *new_node : node_map.values()) {
    /* Parent pointer must be redirected to new node or detached if parent is not copied. */
    if (new_node->parent) {
      if (node_map.contains(new_node->parent)) {
        new_node->parent = node_map.lookup(new_node->parent);
      }
      else {
        bke::node_detach_node(to_tree, *new_node);
      }
    }
  }

  remap_node_pairing(to_tree, node_map);

  /* Copy links between selected nodes. */
  LISTBASE_FOREACH (bNodeLink *, link, &from_tree.links) {
    if (link->tonode->flag & NODE_SELECT && link->fromnode->flag & NODE_SELECT) {
      BLI_assert(node_map.contains(link->tonode) && node_map.contains(link->fromnode));
      bNode *from_node = node_map.lookup(link->fromnode);
      bNode *to_node = node_map.lookup(link->tonode);

      bNodeSocket *from = bke::node_find_socket(*from_node, SOCK_OUT, link->fromsock->identifier);
      bNodeSocket *to = bke::node_find_socket(*to_node, SOCK_IN, link->tosock->identifier);
      if (!from || !to) {
        continue;
      }

      bNodeLink &new_link = bke::node_add_link(to_tree, *from_node, *from, *to_node, *to);
      new_link.multi_input_sort_id = link->multi_input_sort_id;
    }

    to_tree.ensure_topology_cache();
    for (bNode *new_node : node_map.values()) {
      /* Update multi input socket indices in case all connected nodes weren't copied. */
      update_multi_input_indices_for_removed_links(*new_node);
    }
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Copy
 * \{ */

static wmOperatorStatus node_clipboard_copy_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::blendfile;

  Main *bmain = CTX_data_main(C);
  SpaceNode *snode = CTX_wm_space_node(C);
  bNodeTree *node_tree = snode->edittree;

  PartialWriteContext copy_buffer{*bmain};
  bNodeTree *copy_tree = reinterpret_cast<bNodeTree *>(
      copy_buffer.id_create(ID_NT,
                            "Copy Tree",
                            nullptr,
                            {(PartialWriteContext::IDAddOperations::SET_FAKE_USER |
                              PartialWriteContext::IDAddOperations::SET_CLIPBOARD_MARK)}));

  /* Copy node interface to avoid losing links to Group Input and Group Output nodes.
   * Note: this doesn't create new interface items if they don't exist. */
  /* #bNodeTreeInterface::copy_data() allocates runtime memory, so we need to free it to avoid
   * memory leak. */
  copy_tree->tree_interface.free_data();
  copy_tree->tree_interface.copy_data(node_tree->tree_interface, LIB_ID_COPY_DEFAULT);

  // todo(habib): set using ntree_set_typeinfo(ntree, node_tree_type_find(idname));
  bNodeTree *dummy_ntree = blender::bke::node_tree_add_tree(
      bmain, "DummyForTypeinfo", node_tree->typeinfo->idname);
  // dummy_ntree->tree_interface.copy_data(node_tree->tree_interface, LIB_ID_COPY_DEFAULT);
  node_copy_local(*node_tree, *dummy_ntree, true, float2(0), op->reports);

  copy_tree->typeinfo = dummy_ntree->typeinfo;
  copy_tree->type = dummy_ntree->typeinfo->type;
  strcpy(copy_tree->idname, dummy_ntree->typeinfo->idname.c_str());
  BKE_id_delete(bmain, &dummy_ntree->id);  // todo(habib): for debug only

  if (!node_copy_local(*node_tree, *copy_tree, true, float2(0), op->reports)) {
    return OPERATOR_CANCELLED;
  }

  auto add_tree_ids_dependencies_cb = [&copy_buffer,
                                       copy_tree](LibraryIDLinkCallbackData *cb_data) -> int {
    ID *id_src = *cb_data->id_pointer;
    if (!id_src) {
      return IDWALK_RET_NOP;
    }

    printf("id_src->name: %s\n", id_src->name);

    ID *id_dst = nullptr;
    const ID_Type id_type = GS((id_src)->name);

    // todo(habib): doc
    // todo(habib): verify all necessary IDs. Maybe invert the condition and consider Scene only?
    if (ELEM(id_type, ID_SCE, ID_NT, ID_IM, ID_MC, ID_MSK) ||
        (cb_data->cb_flag & IDWALK_CB_NEVER_NULL))
    {
      /* A scene may contain a compositing node trees which references the scene itself. Don't
       * add compositing node trees in this case to avoid circular dependencies. */
      const bool is_root_compositing_node_group = id_type == ID_NT && id_src == &copy_tree->id;
      if (is_root_compositing_node_group) {
        return IDWALK_RET_NOP;
      }

      id_dst = copy_buffer.id_add(
          id_src, {PartialWriteContext::IDAddOperations::CLEAR_DEPENDENCIES}, nullptr);
    }
    *cb_data->id_pointer = id_dst;
    return IDWALK_RET_NOP;
  };

  BKE_library_foreach_ID_link(
      nullptr, &copy_tree->id, add_tree_ids_dependencies_cb, nullptr, IDWALK_NOP);

  char filepath[FILE_MAX];
  node_copybuffer_filepath_get(filepath, sizeof(filepath));
  copy_buffer.write(filepath, *op->reports);

  return OPERATOR_FINISHED;
}

void NODE_OT_clipboard_copy(wmOperatorType *ot)
{
  ot->name = "Copy to Clipboard";
  ot->description = "Copy the selected nodes to the internal clipboard";
  ot->idname = "NODE_OT_clipboard_copy";

  ot->exec = node_clipboard_copy_exec;
  ot->poll = ED_operator_node_active;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Paste
 * \{ */

static wmOperatorStatus node_clipboard_paste_exec(bContext *C, wmOperator *op)
{
  SpaceNode *snode = CTX_wm_space_node(C);

  char filepath[FILE_MAX];
  node_copybuffer_filepath_get(filepath, sizeof(filepath));

  const BlendFileReadParams params{};
  BlendFileReadReport bf_reports{};
  BlendFileData *bfd = BKE_blendfile_read(filepath, &params, &bf_reports);

  if (bfd == nullptr) {
    BKE_report(op->reports, RPT_INFO, "No data to paste");
    return OPERATOR_CANCELLED;
  }

  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));

  Main *bmain_src = bfd->main;
  bfd->main = nullptr;
  BLO_blendfiledata_free(bfd);

  Main *bmain_dst = CTX_data_main(C);
  MainMergeReport merge_reports = {};
  /* Frees bmain_src. */
  BKE_main_merge(bmain_dst, &bmain_src, merge_reports);

  bNodeTree *from_tree = nullptr;
  FOREACH_NODETREE_BEGIN (bmain_dst, node_tree, id) {
    if (node_tree->id.flag & ID_FLAG_CLIPBOARD_MARK) {
      from_tree = node_tree;
      break;
    }
  }
  FOREACH_NODETREE_END;
  BLI_assert(from_tree != nullptr);

  bNodeTree *to_tree = snode->edittree;
  node_deselect_all(*to_tree);

  float2 offset(0);
  PropertyRNA *offset_prop = RNA_struct_find_property(op->ptr, "offset");
  if (RNA_property_is_set(op->ptr, offset_prop)) {
    rctf bbox;
    float2 center;
    BLI_rctf_init_minmax(&bbox);
    for (bNode *node : from_tree->all_nodes()) {
      bbox.xmin = math::min(node->location[0], bbox.xmin);
      bbox.ymin = math::min(node->location[1], bbox.ymin);

      bbox.xmax = math::max(node->location[0] + node->width, bbox.xmax);
      bbox.ymax = math::max(node->location[1] - node->height / 2, bbox.ymax);
    }
    center.x = BLI_rctf_cent_x(&bbox);
    center.y = BLI_rctf_cent_y(&bbox);

    float2 mouse_location;
    RNA_property_float_get_array(op->ptr, offset_prop, mouse_location);
    offset = mouse_location / UI_SCALE_FAC - center;
  }

  if (!node_copy_local(*from_tree, *snode->edittree, false, offset, op->reports)) {
    return OPERATOR_CANCELLED;
  };
  BKE_id_delete(bmain_dst, &from_tree->id);

  BKE_main_ensure_invariants(*bmain_dst);
  /* Pasting nodes can create arbitrary new relations because nodes can reference IDs. */
  DEG_relations_tag_update(bmain_dst);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus node_clipboard_paste_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  const ARegion *region = CTX_wm_region(C);
  float2 cursor;
  UI_view2d_region_to_view(&region->v2d, event->mval[0], event->mval[1], &cursor.x, &cursor.y);
  RNA_float_set_array(op->ptr, "offset", cursor);
  return node_clipboard_paste_exec(C, op);
}

void NODE_OT_clipboard_paste(wmOperatorType *ot)
{
  ot->name = "Paste from Clipboard";
  ot->description = "Paste nodes from the internal clipboard to the active node tree";
  ot->idname = "NODE_OT_clipboard_paste";

  ot->invoke = node_clipboard_paste_invoke;
  ot->exec = node_clipboard_paste_exec;
  ot->poll = ED_operator_node_editable;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_float_array(
      ot->srna,
      "offset",
      2,
      nullptr,
      -FLT_MAX,
      FLT_MAX,
      "Location",
      "The 2D view location for the center of the new nodes, or unchanged if not set",
      -FLT_MAX,
      FLT_MAX);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/** \} */

}  // namespace blender::ed::space_node
