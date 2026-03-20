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

#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_time.h"

#include "BLT_translation.hh"

#include "ED_screen.hh"

#include "WM_api.hh"

#include "interface_intern.hh"
#include "interface_regions_intern.hh"

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Utility Functions
 * \{ */

#define NOTIFICATION_MAX_SHOWN 3

#define NOTIFICATION_MAX_CHARACTERS 70
#define NOTIFICATION_SECONDS_PER_CHAR 0.05f

#define NOTIFICATION_HEIGHT (1.4f * UI_UNIT_Y)
/* Vertical distance separating notifications. */
#define NOTIFICATION_MARGIN (0.2f * UI_UNIT_X)
/* Padding inside the notification box. */
#define NOTIFICATION_PADDING (0.5f * UI_UNIT_X)

#define NOTIFICATION_INITIAL_Y (HEADERY * UI_SCALE_FAC)
#define NOTIFICATION_EXIT_SCROLL (NOTIFICATION_HEIGHT * 0.3f)

#define NOTIFICATION_FADE_IN (0.1f)
#define NOTIFICATION_FADE_OUT (0.45f)

#define NOTIFICATION_LINE_WIDTH (0.2f * UI_UNIT_X)
#define NOTIFICATION_LINE_PADDING (0.1f * UI_UNIT_X)

struct NotificationData {
  std::string message;
  int icon;
  int initial_y;
  float opacity;
  float pos;
  float bg_color[4];
  float line_color[4];
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
    if (region_iter->flag & RGN_FLAG_NOTIFICATION) {
      NotificationData *previous = static_cast<NotificationData *>(region_iter->regiondata);
      num_visible++;
      prior_pos = std::max(prior_pos, previous->pos);
    }
  }

  if (now > (data->time_end) || num_visible > NOTIFICATION_MAX_SHOWN) {
    /* Hide. This will cause removal. */
    region->flag |= RGN_FLAG_HIDDEN;
    return;
  }

  if (now < data->time_display) {
    /* Scroll up and fade in. */
    const float prop = ((data->time_display - now) / NOTIFICATION_FADE_IN);
    data->pos = prior_pos + NOTIFICATION_MARGIN + ((1.0f - prop) * NOTIFICATION_HEIGHT);
    data->opacity = 1.0f - prop;
  }
  else if (now > data->time_hide) {
    /* Scroll up and fade out. */
    const float prop = (now - data->time_hide) / NOTIFICATION_FADE_OUT;
    data->pos = prior_pos + NOTIFICATION_HEIGHT + NOTIFICATION_MARGIN +
                (prop * NOTIFICATION_EXIT_SCROLL);
    data->opacity = 1.0f - prop;
  }
  else {
    /* Display the notification. */
    data->pos = prior_pos + NOTIFICATION_HEIGHT + NOTIFICATION_MARGIN;
    data->opacity = 1.0f;
  }

  data->bg_color[3] = data->opacity;
  data->line_color[3] = data->opacity;
  data->text_color[3] = data->opacity;

  bTheme *btheme = theme::theme_get();
  const float corner_radius = btheme->tui.wcol_menu_back.roundness * U.widget_unit;
  rctf rect = {NOTIFICATION_MARGIN,
               float(region->winx) - NOTIFICATION_MARGIN,
               data->pos,
               data->pos + NOTIFICATION_HEIGHT};

  draw_roundbox_corner_set(CNR_ALL);

  /* Background. */
  draw_dropshadow(&rect,
                  corner_radius,
                  theme::get_menu_shadow_width(),
                  1.0f,
                  btheme->tui.menu_shadow_fac * data->opacity);
  draw_roundbox_4fv(&rect, true, corner_radius, data->bg_color);

  /* Indicator line. */
  rctf rect_line = {NOTIFICATION_MARGIN + NOTIFICATION_LINE_PADDING * 2,
                    NOTIFICATION_MARGIN + NOTIFICATION_LINE_WIDTH + NOTIFICATION_LINE_PADDING,
                    data->pos + 0.2f * UI_UNIT_Y,
                    data->pos - 0.2f * UI_UNIT_Y + NOTIFICATION_HEIGHT};
  draw_roundbox_4fv(&rect_line, true, 3.0f, data->line_color);

  /* Outline. */
  const uchar *outline_color_uchar = btheme->tui.wcol_menu_back.outline;
  float outline_color[4];
  rgba_uchar_to_float(outline_color, outline_color_uchar);
  outline_color[3] = outline_color[3] * data->opacity;
  const float outline_width = U.pixelsize;
  draw_roundbox_4fv_ex(&rect, nullptr, nullptr, 1.0f, outline_color, outline_width, corner_radius);

  /* Icon. */
  uchar icon_color[4];
  rgba_float_to_uchar(icon_color, data->line_color);
  icon_draw_ex(rect.xmin + NOTIFICATION_PADDING + NOTIFICATION_LINE_PADDING,
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
               rect.xmin + NOTIFICATION_PADDING + NOTIFICATION_LINE_PADDING +
                   (22.0f * UI_SCALE_FAC),
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
  const int width = NOTIFICATION_MARGIN + NOTIFICATION_PADDING + text_width +
                    (22.0f * UI_SCALE_FAC) + NOTIFICATION_PADDING + NOTIFICATION_MARGIN;

  wmWindow *win = CTX_wm_window(C);

  int pos_x;

  if (U.notification_position == USER_NOTIFICATION_POS_LEFT) {
    pos_x = (NOTIFICATION_MARGIN / 2);
  }
  else if (U.notification_position == USER_NOTIFICATION_POS_CENTER) {
    pos_x = (win->sizex / 2) - (width / 2);
  }
  else {
    pos_x = (win->sizex - (NOTIFICATION_MARGIN / 2)) - width;
  }

  const int initial_y = data->initial_y;

  region->winrct.xmin = pos_x;
  region->winrct.xmax = region->winrct.xmin + width;
  region->winrct.ymin = -NOTIFICATION_HEIGHT;
  region->winrct.ymin = -NOTIFICATION_HEIGHT + initial_y;
  region->winrct.ymax = region->winrct.ymin + (NOTIFICATION_MARGIN * NOTIFICATION_MAX_SHOWN) +
                        (NOTIFICATION_HEIGHT * (NOTIFICATION_MAX_SHOWN + 1));
  ED_region_update_rect(region);
}

int notification_event_handler(bContext *C, ARegion *region, wmEvent *event)
{
  NotificationData *data = static_cast<NotificationData *>(region->regiondata);

  if (event->type == MOUSEMOVE) {
    data->time_hide = BLI_time_now_seconds() + 10;
    data->time_end = data->time_hide + NOTIFICATION_FADE_OUT;
  }

  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    region->flag |= RGN_FLAG_HIDDEN;
    WM_window_open_temp(C, IFACE_("Blender Info Log"), SPACE_INFO, false);
  }

  return WM_UI_HANDLER_BREAK;
}

void notification(bScreen *screen, StringRef message, int icon, eReportType report_type)
{
  NotificationData *data = MEM_new<NotificationData>(__func__);
  data->icon = icon;
  data->initial_y = (screen->flag & SCREEN_COLLAPSE_STATUSBAR) ? NOTIFICATION_MARGIN : 0;
  data->opacity = 0.0f;
  data->pos = 0.0f;

  data->message = message;
  if (data->message.size() > NOTIFICATION_MAX_CHARACTERS) {
    data->message = data->message.substr(0, NOTIFICATION_MAX_CHARACTERS) +
                    BLI_STR_UTF8_HORIZONTAL_ELLIPSIS;
  }

  const float display_seconds = std::max(U.notification_seconds,
                                         data->message.size() * NOTIFICATION_SECONDS_PER_CHAR);
  data->time_start = BLI_time_now_seconds();
  data->time_display = data->time_start + NOTIFICATION_FADE_IN;
  data->time_hide = data->time_display + display_seconds;
  data->time_end = data->time_hide + NOTIFICATION_FADE_OUT;

  bTheme *btheme = theme::theme_get();
  rgba_uchar_to_float(data->text_color, btheme->tui.wcol_menu_back.text_sel);
  rgba_uchar_to_float(data->bg_color, btheme->tui.wcol_menu_back.inner);

  theme::get_color_shade_4fv(icon_colorid_from_report_type(report_type), 20, data->line_color);

  const float blend_factor = btheme->tui.notification_blend;
  interp_v4_v4v4(data->bg_color, data->bg_color, data->line_color, blend_factor);

  ARegion *region = region_temp_add(screen);
  region->regiondata = data;
  region->flag |= RGN_FLAG_NOTIFICATION;

  static ARegionType type = []() {
    ARegionType type = {};
    type.layout = notification_region_layout_fn;
    type.free = notification_region_free_fn;
    type.regionid = RGN_TYPE_TEMPORARY;
    type.draw_overlay = notification_region_draw_overlay_fn;
    return type;
  }();

  region->runtime->type = &type;
  region->runtime->visible = true;
}

/** \} */

}  // namespace blender::ui
