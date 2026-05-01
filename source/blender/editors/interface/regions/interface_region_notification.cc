/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * PopUp Region (Generic)
 */

#include "BKE_screen.hh"

#include "BLF_api.hh"

#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_string_utf8.h"
#include "BLI_time.h"

#include "BLT_translation.hh"

#include "ED_screen.hh"

#include "WM_api.hh"

#include "interface_intern.hh"
#include "interface_regions_intern.hh"

namespace blender::ui::notification {

static constexpr int MAX_SHOWN = 3;

static constexpr float HEIGHT = 28.0f;
/* Vertical distance separating notifications. */
static constexpr float MARGIN = 4.0f;
/* Padding inside the notification box. */
static constexpr float PADDING = 8.0f;
static constexpr float LINE_PADDING = 2.0f;
static constexpr float MAX_TEXT_WIDTH = 800.0f;

static constexpr float FADE_IN_TIME = 0.1f;
static constexpr float SECONDS_PER_CHAR = 0.05f;
static constexpr float FADE_OUT_TIME = 0.45f;
static constexpr float EXIT_SCROLL = 8.4f;

struct NotificationData {
  std::string message;
  int icon;
  int initial_y;
  float opacity;
  float pos;
  float bg_color[4];
  float icon_color[4];
  float text_color[4];
  double time_start;
  double time_display;
  double time_hide;
  double time_end;
};

static void notification_region_draw_overlay_fn(const bContext * /*C*/, ARegion *region)
{
  NotificationData *data = static_cast<NotificationData *>(region->regiondata);
  const double now = BLI_time_now_seconds();
  int num_visible = 0;
  float prior_pos = 0.0f;

  ARegion *region_iter = region;
  while (region_iter->next) {
    region_iter = region_iter->next;
    if (region_iter->regiontype == RGN_TYPE_NOTIFICATION) {
      NotificationData *previous = static_cast<NotificationData *>(region_iter->regiondata);
      num_visible++;
      prior_pos = std::max(prior_pos, previous->pos);
    }
  }

  if (now > (data->time_end) || num_visible > MAX_SHOWN) {
    /* Hide. This will cause removal. */
    region->flag |= RGN_FLAG_HIDDEN;
    return;
  }

  if (now < data->time_display) {
    /* Scroll up and fade in. */
    const float prop = ((data->time_display - now) / FADE_IN_TIME);
    data->pos = prior_pos + ((MARGIN * UI_SCALE_FAC) + ((1.0f - prop) * (HEIGHT * UI_SCALE_FAC)));
    data->opacity = 1.0f - prop;
  }
  else if (now > data->time_hide) {
    /* Scroll up and fade out. */
    const float prop = (now - data->time_hide) / FADE_OUT_TIME;
    data->pos = prior_pos + (HEIGHT + MARGIN) * UI_SCALE_FAC + (prop * EXIT_SCROLL * UI_SCALE_FAC);
    data->opacity = 1.0f - prop;
  }
  else {
    /* Display the notification. */
    data->pos = prior_pos + (HEIGHT + MARGIN) * UI_SCALE_FAC;
    data->opacity = 1.0f;
  }

  data->bg_color[3] = data->opacity;
  data->icon_color[3] = data->opacity;
  data->text_color[3] = data->opacity;

  bTheme *btheme = theme::theme_get();
  const float corner_radius = btheme->tui.wcol_menu_back.roundness * U.widget_unit;
  rctf rect = {(MARGIN * UI_SCALE_FAC),
               float(region->winx) - (MARGIN * UI_SCALE_FAC),
               data->pos,
               data->pos + (HEIGHT * UI_SCALE_FAC)};

  draw_roundbox_corner_set(CNR_ALL);

  /* Background. */
  draw_dropshadow(&rect,
                  corner_radius,
                  theme::get_menu_shadow_width(),
                  1.0f,
                  btheme->tui.menu_shadow_fac * data->opacity);
  draw_roundbox_4fv(&rect, true, corner_radius, data->bg_color);

  /* Outline. */
  const uchar *outline_color_uchar = btheme->tui.wcol_menu_back.outline;
  float outline_color[4];
  rgba_uchar_to_float(outline_color, outline_color_uchar);
  outline_color[3] = outline_color[3] * data->opacity;
  const float outline_width = U.pixelsize;
  draw_roundbox_4fv_ex(&rect, nullptr, nullptr, 1.0f, outline_color, outline_width, corner_radius);

  /* Icon. */
  uchar icon_color[4];
  rgba_float_to_uchar(icon_color, data->icon_color);
  icon_draw_ex(rect.xmin + (PADDING + LINE_PADDING) * UI_SCALE_FAC,
               rect.ymin + (5.5f * UI_SCALE_FAC),
               data->icon,
               1.0f / UI_SCALE_FAC,
               data->opacity,
               0.0f,
               icon_color,
               false,
               nullptr);

  /* Text. */
  const uiStyle *style = style_get_dpi();
  fontstyle_set(&style->widget);
  BLF_color4fv(style->widget.uifont_id, data->text_color);
  BLF_position(style->widget.uifont_id,
               rect.xmin + (PADDING + LINE_PADDING + 22.0f) * UI_SCALE_FAC,
               rect.ymin + (10 * UI_SCALE_FAC),
               0.0f);
  BLF_draw(style->widget.uifont_id, data->message.c_str(), data->message.size());

  region->runtime->do_draw = true;
}

static void notification_region_free_fn(ARegion *region)
{
  NotificationData *data = static_cast<NotificationData *>(region->regiondata);
  MEM_delete(data);
  region->regiondata = nullptr;
}

static void notification_region_layout_fn(const bContext *C, ARegion *region)
{
  NotificationData *data = static_cast<NotificationData *>(region->regiondata);

  const uiStyle *style = style_get_dpi();
  fontstyle_set(&style->widget);
  int text_width = BLF_width(style->widget.uifont_id, data->message.c_str(), data->message.size());
  const int width = text_width + (MARGIN + PADDING + 22.0f + PADDING + MARGIN) * UI_SCALE_FAC;

  wmWindow *win = CTX_wm_window(C);

  int pos_x = (win->sizex - (MARGIN * UI_SCALE_FAC / 2)) - width;
  const int initial_y = data->initial_y;

  region->winrct.xmin = pos_x;
  region->winrct.xmax = region->winrct.xmin + width;
  region->winrct.ymin = -(HEIGHT * UI_SCALE_FAC);
  region->winrct.ymin = -(HEIGHT * UI_SCALE_FAC) + initial_y;
  region->winrct.ymax = region->winrct.ymin + (MARGIN * UI_SCALE_FAC * MAX_SHOWN) +
                        (HEIGHT * UI_SCALE_FAC * (MAX_SHOWN + 1));
  ED_region_update_rect(region);
}

int event_handler(bContext *C, ARegion *region, wmEvent *event)
{
  NotificationData *data = static_cast<NotificationData *>(region->regiondata);

  if (event->type == MOUSEMOVE) {
    data->time_hide = BLI_time_now_seconds() + 10;
    data->time_end = data->time_hide + FADE_OUT_TIME;
  }

  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    region->flag |= RGN_FLAG_HIDDEN;
    WM_window_open_temp(C, IFACE_("Blender Info Log"), SPACE_INFO, false);
  }

  return WM_UI_HANDLER_BREAK;
}

void show(bScreen *screen, StringRef message, int icon, eReportType report_type)
{
  NotificationData *data = MEM_new<NotificationData>(__func__);
  data->icon = icon;
  data->initial_y = (screen->flag & SCREEN_COLLAPSE_STATUSBAR) ? (MARGIN * UI_SCALE_FAC) : 0;
  data->opacity = 0.0f;
  data->pos = 0.0f;

  data->message = message;

  const uiStyle *style = style_get_dpi();
  fontstyle_set(&style->widget);

  /* Clip the message if it is too long to fit within the maximum width. */
  const size_t max_bytes = BLF_width_to_strlen(style->widget.uifont_id,
                                               data->message.c_str(),
                                               data->message.size(),
                                               MAX_TEXT_WIDTH * UI_SCALE_FAC,
                                               nullptr);
  if (max_bytes < data->message.size() - 1) {
    data->message = data->message.substr(0, max_bytes) + BLI_STR_UTF8_HORIZONTAL_ELLIPSIS;
  }

  /* Display seconds based on characters, not bytes. */
  const size_t num_chars = BLI_strnlen_utf8(data->message.c_str(), data->message.size());
  const float display_seconds = std::max(U.notification_seconds, num_chars * SECONDS_PER_CHAR);

  data->time_start = BLI_time_now_seconds();
  data->time_display = data->time_start + FADE_IN_TIME;
  data->time_hide = data->time_display + display_seconds;
  data->time_end = data->time_hide + FADE_OUT_TIME;

  bTheme *btheme = theme::theme_get();
  rgba_uchar_to_float(data->text_color, btheme->tui.wcol_menu_back.text_sel);
  rgba_uchar_to_float(data->bg_color, btheme->tui.wcol_menu_back.inner);

  theme::get_color_shade_4fv(icon_colorid_from_report_type(report_type), 20, data->icon_color);

  const float blend_factor = 0.2f;
  interp_v4_v4v4(data->bg_color, data->bg_color, data->icon_color, blend_factor);

  ARegion *region = BKE_area_region_new();
  BLI_addtail(&screen->regionbase, region);
  region->regiontype = RGN_TYPE_NOTIFICATION;
  region->alignment = RGN_ALIGN_FLOAT;
  region->regiondata = data;

  static ARegionType type = []() {
    ARegionType type = {};
    type.layout = notification_region_layout_fn;
    type.free = notification_region_free_fn;
    type.regionid = RGN_TYPE_NOTIFICATION;
    type.draw_overlay = notification_region_draw_overlay_fn;
    return type;
  }();

  region->runtime->type = &type;
  region->runtime->visible = true;
}

/** \} */

}  // namespace blender::ui::notification
