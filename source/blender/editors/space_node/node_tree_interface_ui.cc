/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_screen.hh"
#include "BLI_listbase.h"
#include "BLI_string_utf8.h"
#include "BLT_translation.hh"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_intern.hh"

namespace blender::ed::space_node {

bool node_tree_interface_panel_poll(const bContext *C, PanelType * /*pt*/)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  if (!snode) {
    return false;
  }
  bNodeTree *ntree = snode->edittree;
  if (!ntree) {
    return false;
  }
  if (ntree->flag & ID_FLAG_EMBEDDED_DATA) {
    return false;
  }
  if (ntree->typeinfo->no_group_interface) {
    return false;
  }
  return true;
}

static void node_tree_interface_panel_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout &layout = *panel->layout;
  layout.label("Hello World", ICON_NONE);
}

void node_tree_interface_panel_register(ARegionType *art)
{
  PanelType *pt = MEM_callocN<PanelType>("NODE_PT_node_tree_interface2");
  STRNCPY_UTF8(pt->idname, "NODE_PT_node_tree_interface2");
  STRNCPY_UTF8(pt->label, N_("Group Sockets"));
  STRNCPY_UTF8(pt->category, "Group");
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->draw = node_tree_interface_panel_draw;
  pt->poll = node_tree_interface_panel_poll;
  pt->order = 10;
  BLI_addtail(&art->paneltypes, pt);
}

}  // namespace blender::ed::space_node
