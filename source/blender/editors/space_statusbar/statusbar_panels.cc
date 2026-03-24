/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_idprop.hh"
#include "BKE_screen.hh"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"

#include "RNA_access.hh"

#include <fmt/format.h>
#include <string>

#include "statusbar_intern.hh"

namespace blender {

struct VersionUpdateInfo {
  std::string commit_hash;
  std::string version;
  bool is_lts;
  std::string description;
  std::string release_notes_url;
  std::string download_url;
};

static Vector<VersionUpdateInfo> &blender_available_updates()
{
  static Vector<VersionUpdateInfo> updates = {
      {
          .commit_hash = "3cfed7c6ecab",
          .version = "5.0.1",
          .is_lts = false,
          .description = "Over 100 bug fixes, it is highly recomended to update.",
          .release_notes_url = "https://www.blender.org/download/releases/5-0/",
          .download_url = "https://www.blender.org/download/",
      },
      {
          .commit_hash = "2cfed7c6ecab",
          .version = "5.1.1",
          .is_lts = false,
          .description = "Over 45 bug fixes, it is highly recomended to update.",
          .release_notes_url = "https://www.blender.org/download/releases/5-0/",
          .download_url = "https://www.blender.org/download/",
      },
      {
          .commit_hash = "1cfed7c6ecab",
          .version = "5.2.0",
          .is_lts = true,
          .description = "New LTS major release is available.",
          .release_notes_url = "https://www.blender.org/download/releases/5-0/",
          .download_url = "https://www.blender.org/download/",
      },
  };
  return updates;
}

static void version_update_draw_body(VersionUpdateInfo &update, ui::Layout &layout)
{
  ui::Layout &row = layout.row(true);
  row.emboss_set(ui::EmbossType::None);
  row.alignment_set(ui::LayoutAlign::Expand);
  row.label(update.description, ICON_NONE);

  layout.separator(0.0f);

  ui::Layout &buttons_row = layout.row(true);

  ui::Layout &left_row = buttons_row.row(false);
  left_row.alignment_set(ui::LayoutAlign::Left);
  ui::Button *button = uiDefBut(layout.block(),
                                ui::ButtonType::But,
                                "Skip " + update.version,
                                0,
                                0,
                                5.0f * UI_UNIT_X,
                                UI_UNIT_Y,
                                nullptr,
                                0,
                                0,
                                "");
  ui::button_func_set(button, [commit_hash = update.commit_hash](blender::bContext & /*C*/) {
    blender_available_updates().remove_if(
        [&](const VersionUpdateInfo &update) { return update.commit_hash == commit_hash; });
  });
  ui::Layout &right_row = buttons_row.row(false);
  right_row.alignment_set(ui::LayoutAlign::Right);
  PointerRNA op_ptr = right_row.op("WM_OT_url_open", "Download", ICON_IMPORT);
  RNA_string_set(&op_ptr, "url", update.download_url.c_str());
};

static void panel_blender_updates_draw(const bContext *C, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  Vector<VersionUpdateInfo> &available_updates = blender_available_updates();
  if (available_updates.is_empty()) {
    ui::Layout &header = layout.row(true);
    header.label(IFACE_("No updates available"), ICON_NONE);
    return;
  }
  if (available_updates.size() == 1) {
    VersionUpdateInfo &update = available_updates[0];
    ui::Layout &header = layout.row(true);
    const char *release_text = update.version.ends_with(".0") ?
                                   "New release available {}{}" :
                                   "New bugfix release available: {}{}";
    header.label(fmt::format(fmt::runtime(IFACE_(release_text)),
                             update.version,
                             update.is_lts ? " LTS" : ""),
                 ICON_NONE);
    ui::Layout &sub = header.row(false);
    sub.alignment_set(ui::LayoutAlign::Right);
    sub.link(update.release_notes_url, "Whats new", ICON_NONE);
    version_update_draw_body(update, layout.column(false));
    return;
  }

  ui::Layout &header = layout.row(true);
  header.label("New Blender Updates Available", ICON_NONE);

  ui::Layout &skip_all_row = header.row(true);
  skip_all_row.alignment_set(ui::LayoutAlign::Right);
  ui::Button *button = uiDefBut(layout.block(),
                                ui::ButtonType::But,
                                "Skip All",
                                0,
                                0,
                                5 * UI_UNIT_X,
                                UI_UNIT_Y,
                                nullptr,
                                0,
                                0,
                                "");
  ui::button_func_set(button,
                      [](blender::bContext & /*C*/) { blender_available_updates().clear(); });
  ui::button_drawflag_disable(button, ui::BUT_TEXT_RIGHT);
  for (VersionUpdateInfo &update : available_updates) {
    ui::PanelLayout panel_layout = layout.panel(C, "Update_" + update.version, false);
    panel_layout.header->label(update.version, ICON_NONE);
    ui::Layout *body = panel_layout.body;
    ui::Layout &sub = panel_layout.header->row(false);
    sub.alignment_set(ui::LayoutAlign::Right);
    sub.link(update.release_notes_url, "Whats new", ICON_NONE);
    if (!body) {
      continue;
    }
    version_update_draw_body(update, body->column(false));
  }
}

static bool panel_blender_updates_poll(const blender::bContext * /*C*/,
                                       blender::PanelType * /*pt*/)
{
  return !blender_available_updates().is_empty();
}

void panel_blender_updates_register(ARegionType *region_type)
{

  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "STATUS_PT_blender_updates");
  STRNCPY_UTF8(pt->label, N_("Updates Available"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Display availble Blender updates in a popover panel");
  pt->draw = panel_blender_updates_draw;
  pt->poll = panel_blender_updates_poll;
  pt->region_type = RGN_TYPE_HEADER;
  pt->space_type = SPACE_INFO;
  pt->ui_units_x = 16;
  BLI_addtail(&region_type->paneltypes, pt);
  WM_paneltype_add(pt);
}

}  // namespace blender
