/* SPDX-FileCopyrightText: 2005-2007 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Cursor pixmap and cursor utility functions to change the cursor.
 */

#include <cstring>

#include "GHOST_C-api.h"

#include "BLI_utildefines.h"

#include "DNA_listBase.h"
#include "DNA_userdef_types.h"
#include "DNA_workspace_types.h"

#include "BKE_global.hh"
#include "BKE_main.hh"

#include "nanosvgrast.h"
#include "svg_cursors.h"

#include "WM_api.hh"
#include "WM_types.hh"
#include "wm_cursors.hh"
#include "wm_window.hh"

/* Blender custom cursor. */
struct BCursor {
  const char *svg_source;
  float hotspot_x;
  float hotspot_y;
  float size;
};

static BCursor BlenderCursor[WM_CURSOR_NUM] = {0};

/* Blender cursor to GHOST standard cursor conversion. */
static GHOST_TStandardCursor convert_to_ghost_standard_cursor(WMCursorType curs)
{
  switch (curs) {
    case WM_CURSOR_DEFAULT:
      return GHOST_kStandardCursorDefault;
    case WM_CURSOR_WAIT:
      return GHOST_kStandardCursorWait;
    case WM_CURSOR_EDIT:
    case WM_CURSOR_CROSS:
      return GHOST_kStandardCursorCrosshair;
    case WM_CURSOR_MOVE:
      return GHOST_kStandardCursorMove;
    case WM_CURSOR_X_MOVE:
      return GHOST_kStandardCursorLeftRight;
    case WM_CURSOR_Y_MOVE:
      return GHOST_kStandardCursorUpDown;
    case WM_CURSOR_COPY:
      return GHOST_kStandardCursorCopy;
    case WM_CURSOR_HAND:
      return GHOST_kStandardCursorHandOpen;
    case WM_CURSOR_HAND_CLOSED:
      return GHOST_kStandardCursorHandClosed;
    case WM_CURSOR_HAND_POINT:
      return GHOST_kStandardCursorHandPoint;
    case WM_CURSOR_H_SPLIT:
      return GHOST_kStandardCursorHorizontalSplit;
    case WM_CURSOR_V_SPLIT:
      return GHOST_kStandardCursorVerticalSplit;
    case WM_CURSOR_STOP:
      return GHOST_kStandardCursorStop;
    case WM_CURSOR_KNIFE:
      return GHOST_kStandardCursorKnife;
    case WM_CURSOR_NSEW_SCROLL:
      return GHOST_kStandardCursorNSEWScroll;
    case WM_CURSOR_NS_SCROLL:
      return GHOST_kStandardCursorNSScroll;
    case WM_CURSOR_EW_SCROLL:
      return GHOST_kStandardCursorEWScroll;
    case WM_CURSOR_EYEDROPPER:
      return GHOST_kStandardCursorEyedropper;
    case WM_CURSOR_N_ARROW:
      return GHOST_kStandardCursorUpArrow;
    case WM_CURSOR_S_ARROW:
      return GHOST_kStandardCursorDownArrow;
    case WM_CURSOR_PAINT:
      return GHOST_kStandardCursorCrosshairA;
    case WM_CURSOR_DOT:
      return GHOST_kStandardCursorCrosshairB;
    case WM_CURSOR_CROSSC:
      return GHOST_kStandardCursorCrosshairC;
    case WM_CURSOR_ERASER:
      return GHOST_kStandardCursorEraser;
    case WM_CURSOR_ZOOM_IN:
      return GHOST_kStandardCursorZoomIn;
    case WM_CURSOR_ZOOM_OUT:
      return GHOST_kStandardCursorZoomOut;
    case WM_CURSOR_TEXT_EDIT:
      return GHOST_kStandardCursorText;
    case WM_CURSOR_PAINT_BRUSH:
      return GHOST_kStandardCursorPencil;
    case WM_CURSOR_E_ARROW:
      return GHOST_kStandardCursorRightArrow;
    case WM_CURSOR_W_ARROW:
      return GHOST_kStandardCursorLeftArrow;
    case WM_CURSOR_LEFT_HANDLE:
      return GHOST_kStandardCursorLeftHandle;
    case WM_CURSOR_RIGHT_HANDLE:
      return GHOST_kStandardCursorRightHandle;
    case WM_CURSOR_BOTH_HANDLES:
      return GHOST_kStandardCursorBothHandles;
    case WM_CURSOR_BLADE:
      return GHOST_kStandardCursorBlade;
    default:
      return GHOST_kStandardCursorCustom;
  }
}

static float cursor_size()
{
#if (OS_MAC)
  /* MacOS always scales up this type of cursor for high-dpi displays.
   * The mid-sized 24x24 versions are a nice compromise size. */
  return 24.0f;
#endif

  // const uint32_t cursor_size = WM_cursor_preferred_logical_size();
  //  WM_CURSOR_DEFAULT_LOGICAL_SIZE)
  return 22.0f * UI_SCALE_FAC;
}

static blender::Array<uchar> cursor_bitmap_from_svg(const char *svg,
                                                    float size,
                                                    int &width,
                                                    int &height)
{
  /* Nano alters the source string. */
  std::string svg_source = svg;

  /* Can edit the source here if we want. */

  NSVGimage *image = nsvgParse(svg_source.data(), "px", 96.0f);
  if (image == nullptr) {
    return {};
  }
  if (image->width == 0 || image->height == 0) {
    nsvgDelete(image);
    return {};
  }
  NSVGrasterizer *rast = nsvgCreateRasterizer();
  if (rast == nullptr) {
    nsvgDelete(image);
    return {};
  }

  float scale = (size / 1600.0f);
  const int dest_w = int(ceil(image->width * scale));
  const int dest_h = int(ceil(image->height * scale));
  scale = float(dest_w) / image->width;

  blender::Array<uchar> render_bmp(dest_w * dest_h * 4);

  nsvgRasterize(rast, image, 0.0f, 0.0f, scale, render_bmp.data(), dest_w, dest_h, dest_w * 4);
  nsvgDeleteRasterizer(rast);
  nsvgDelete(image);

  width = dest_w;
  height = dest_h;

  return render_bmp;
}

static bool window_set_custom_cursor(wmWindow *win, BCursor *cursor)
{
  float size = cursor_size();

  if (size > 32.0f) {
    size = 32.0f;  // Larger cursors not supported yet.
  }

  int dest_w;
  int dest_h;
  blender::Array<uchar> render_bmp = cursor_bitmap_from_svg(
      cursor->svg_source, size, dest_w, dest_h);

  /* If we give GHOST_SetCustomCursorShape bit depth arguments
   * we could try sending full-color to it first. */

  /* Monochrome. */

  char width = std::min(dest_w, 32);
  char height = std::min(dest_h, 32);
  char bitmap[4 * 32] = {0};
  char mask[4 * 32] = {0};

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int i = (y * width * 4) + (x * 4);
      int j = (y * 4) + (x >> 3);
      int k = (x % 8);
      if (render_bmp[i + 3] > 128) {
        if (render_bmp[i] > 128) {
          bitmap[j] |= (1 << k);
        }
        mask[j] |= (1 << k);
      }
    }
  }

  int hotspot_x = int(cursor->hotspot_x * (dest_w - 1));
  int hotspot_y = int(cursor->hotspot_y * (dest_h - 1));

  return GHOST_SetCustomCursorShape(static_cast<GHOST_WindowHandle>(win->ghostwin),
                                    (uint8_t *)bitmap,
                                    (uint8_t *)mask,
                                    32,
                                    32,
                                    hotspot_x,
                                    hotspot_y,
                                    false) == GHOST_kSuccess;
}

void WM_cursor_set(wmWindow *win, int curs)
{
  if (win == nullptr || G.background) {
    return; /* Can't set custom cursor before Window init. */
  }

  if (curs == WM_CURSOR_DEFAULT && win->modalcursor) {
    curs = win->modalcursor;
  }

  if (curs == WM_CURSOR_NONE) {
    GHOST_SetCursorVisibility(static_cast<GHOST_WindowHandle>(win->ghostwin), false);
    return;
  }

  GHOST_SetCursorVisibility(static_cast<GHOST_WindowHandle>(win->ghostwin), true);

  if (win->cursor == curs) {
    return; /* Cursor is already set. */
  }

  win->cursor = curs;

  if (curs < 0 || curs >= WM_CURSOR_NUM) {
    BLI_assert_msg(0, "Invalid cursor number");
    return;
  }

  GHOST_TStandardCursor ghost_cursor = convert_to_ghost_standard_cursor(WMCursorType(curs));

  /* Turn off the retrieval of OS-supplied cursors for testing. */
  if (false && ghost_cursor != GHOST_kStandardCursorCustom &&
      GHOST_HasCursorShape(static_cast<GHOST_WindowHandle>(win->ghostwin), ghost_cursor))
  {
    /* Use native GHOST cursor when available. */
    GHOST_SetCursorShape(static_cast<GHOST_WindowHandle>(win->ghostwin), ghost_cursor);
  }
  else {
    BCursor *bcursor = &BlenderCursor[curs];
    if (curs == WM_CURSOR_EDIT) {
      printf("here");
    }
    if (!bcursor || !bcursor->svg_source || !window_set_custom_cursor(win, bcursor)) {
      /* Fall back to default cursor if no bitmap found. */
      GHOST_SetCursorShape(static_cast<GHOST_WindowHandle>(win->ghostwin),
                           GHOST_kStandardCursorDefault);
    }
  }
}

bool WM_cursor_set_from_tool(wmWindow *win, const ScrArea *area, const ARegion *region)
{
  if (region && !ELEM(region->regiontype, RGN_TYPE_WINDOW, RGN_TYPE_PREVIEW)) {
    return false;
  }

  bToolRef_Runtime *tref_rt = (area && area->runtime.tool) ? area->runtime.tool->runtime : nullptr;
  if (tref_rt && tref_rt->cursor != WM_CURSOR_DEFAULT) {
    if (win->modalcursor == 0) {
      WM_cursor_set(win, tref_rt->cursor);
      win->cursor = tref_rt->cursor;
      return true;
    }
  }
  return false;
}

void WM_cursor_modal_set(wmWindow *win, int val)
{
  if (win->lastcursor == 0) {
    win->lastcursor = win->cursor;
  }
  win->modalcursor = val;
  WM_cursor_set(win, val);
}

void WM_cursor_modal_restore(wmWindow *win)
{
  win->modalcursor = 0;
  if (win->lastcursor) {
    WM_cursor_set(win, win->lastcursor);
  }
  win->lastcursor = 0;
}

void WM_cursor_wait(bool val)
{
  if (!G.background) {
    wmWindowManager *wm = static_cast<wmWindowManager *>(G_MAIN->wm.first);
    wmWindow *win = static_cast<wmWindow *>(wm ? wm->windows.first : nullptr);

    for (; win; win = win->next) {
      if (val) {
        WM_cursor_modal_set(win, WM_CURSOR_WAIT);
      }
      else {
        WM_cursor_modal_restore(win);
      }
    }
  }
}

void WM_cursor_grab_enable(wmWindow *win,
                           const eWM_CursorWrapAxis wrap,
                           const rcti *wrap_region,
                           const bool hide)
{
  int _wrap_region_buf[4];
  int *wrap_region_screen = nullptr;

  /* Only grab cursor when not running debug.
   * It helps not to get a stuck WM when hitting a break-point. */
  GHOST_TGrabCursorMode mode = GHOST_kGrabNormal;
  GHOST_TAxisFlag mode_axis = GHOST_TAxisFlag(GHOST_kAxisX | GHOST_kAxisY);

  if (wrap_region) {
    wrap_region_screen = _wrap_region_buf;
    wrap_region_screen[0] = wrap_region->xmin;
    wrap_region_screen[1] = wrap_region->ymax;
    wrap_region_screen[2] = wrap_region->xmax;
    wrap_region_screen[3] = wrap_region->ymin;
    wm_cursor_position_to_ghost_screen_coords(win, &wrap_region_screen[0], &wrap_region_screen[1]);
    wm_cursor_position_to_ghost_screen_coords(win, &wrap_region_screen[2], &wrap_region_screen[3]);
  }

  if (hide) {
    mode = GHOST_kGrabHide;
  }
  else if (wrap != WM_CURSOR_WRAP_NONE) {
    mode = GHOST_kGrabWrap;

    if (wrap == WM_CURSOR_WRAP_X) {
      mode_axis = GHOST_kAxisX;
    }
    else if (wrap == WM_CURSOR_WRAP_Y) {
      mode_axis = GHOST_kAxisY;
    }
  }

  if ((G.debug & G_DEBUG) == 0) {
    if (win->ghostwin) {
      if (win->eventstate->tablet.is_motion_absolute == false) {
        GHOST_SetCursorGrab(static_cast<GHOST_WindowHandle>(win->ghostwin),
                            mode,
                            mode_axis,
                            wrap_region_screen,
                            nullptr);
      }

      win->grabcursor = mode;
    }
  }
}

void WM_cursor_grab_disable(wmWindow *win, const int mouse_ungrab_xy[2])
{
  if ((G.debug & G_DEBUG) == 0) {
    if (win && win->ghostwin) {
      if (mouse_ungrab_xy) {
        int mouse_xy[2] = {mouse_ungrab_xy[0], mouse_ungrab_xy[1]};
        wm_cursor_position_to_ghost_screen_coords(win, &mouse_xy[0], &mouse_xy[1]);
        GHOST_SetCursorGrab(static_cast<GHOST_WindowHandle>(win->ghostwin),
                            GHOST_kGrabDisable,
                            GHOST_kAxisNone,
                            nullptr,
                            mouse_xy);
      }
      else {
        GHOST_SetCursorGrab(static_cast<GHOST_WindowHandle>(win->ghostwin),
                            GHOST_kGrabDisable,
                            GHOST_kAxisNone,
                            nullptr,
                            nullptr);
      }

      win->grabcursor = GHOST_kGrabDisable;
    }
  }
}

static void wm_cursor_warp_relative(wmWindow *win, int x, int y)
{
  /* NOTE: don't use wmEvent coords because of continuous grab #36409. */
  int cx, cy;
  if (wm_cursor_position_get(win, &cx, &cy)) {
    WM_cursor_warp(win, cx + x, cy + y);
  }
}

bool wm_cursor_arrow_move(wmWindow *win, const wmEvent *event)
{
  /* TODO: give it a modal keymap? Hard coded for now. */

  if (win && event->val == KM_PRESS) {
    /* Must move at least this much to avoid rounding in WM_cursor_warp. */
    float fac = GHOST_GetNativePixelSize(static_cast<GHOST_WindowHandle>(win->ghostwin));

    if (event->type == EVT_UPARROWKEY) {
      wm_cursor_warp_relative(win, 0, fac);
      return true;
    }
    if (event->type == EVT_DOWNARROWKEY) {
      wm_cursor_warp_relative(win, 0, -fac);
      return true;
    }
    if (event->type == EVT_LEFTARROWKEY) {
      wm_cursor_warp_relative(win, -fac, 0);
      return true;
    }
    if (event->type == EVT_RIGHTARROWKEY) {
      wm_cursor_warp_relative(win, fac, 0);
      return true;
    }
  }
  return false;
}

static bool wm_cursor_time_large(wmWindow *win, int nr)
{
  /* 10 16x16 digits. */
  const uchar number_bitmaps[][32] = {
      {0x00, 0x00, 0xf0, 0x0f, 0xf8, 0x1f, 0x1c, 0x38, 0x0c, 0x30, 0x0c,
       0x30, 0x0c, 0x30, 0x0c, 0x30, 0x0c, 0x30, 0x0c, 0x30, 0x0c, 0x30,
       0x0c, 0x30, 0x1c, 0x38, 0xf8, 0x1f, 0xf0, 0x0f, 0x00, 0x00},
      {0x00, 0x00, 0x80, 0x01, 0xc0, 0x01, 0xf0, 0x01, 0xbc, 0x01, 0x8c,
       0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01,
       0x80, 0x01, 0x80, 0x01, 0xfc, 0x3f, 0xfc, 0x3f, 0x00, 0x00},
      {0x00, 0x00, 0xf0, 0x1f, 0xf8, 0x3f, 0x1c, 0x30, 0x0c, 0x30, 0x00,
       0x30, 0x00, 0x30, 0xe0, 0x3f, 0xf0, 0x1f, 0x38, 0x00, 0x1c, 0x00,
       0x0c, 0x00, 0x0c, 0x00, 0xfc, 0x3f, 0xfc, 0x3f, 0x00, 0x00},
      {0x00, 0x00, 0xf0, 0x0f, 0xf8, 0x1f, 0x1c, 0x38, 0x00, 0x30, 0x00,
       0x30, 0x00, 0x38, 0xf0, 0x1f, 0xf0, 0x1f, 0x00, 0x38, 0x00, 0x30,
       0x00, 0x30, 0x1c, 0x38, 0xf8, 0x1f, 0xf0, 0x0f, 0x00, 0x00},
      {0x00, 0x00, 0x00, 0x0f, 0x80, 0x0f, 0xc0, 0x0d, 0xe0, 0x0c, 0x70,
       0x0c, 0x38, 0x0c, 0x1c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c,
       0xfc, 0x3f, 0xfc, 0x3f, 0x00, 0x0c, 0x00, 0x0c, 0x00, 0x00},
      {0x00, 0x00, 0xfc, 0x3f, 0xfc, 0x3f, 0x0c, 0x00, 0x0c, 0x00, 0x0c,
       0x00, 0xfc, 0x0f, 0xfc, 0x1f, 0x00, 0x38, 0x00, 0x30, 0x00, 0x30,
       0x00, 0x30, 0x0c, 0x38, 0xfc, 0x1f, 0xf8, 0x0f, 0x00, 0x00},
      {0x00, 0x00, 0xc0, 0x3f, 0xe0, 0x3f, 0x70, 0x00, 0x38, 0x00, 0x1c,
       0x00, 0xfc, 0x0f, 0xfc, 0x1f, 0x0c, 0x38, 0x0c, 0x30, 0x0c, 0x30,
       0x0c, 0x30, 0x1c, 0x38, 0xf8, 0x1f, 0xf0, 0x0f, 0x00, 0x00},
      {0x00, 0x00, 0xfc, 0x3f, 0xfc, 0x3f, 0x0c, 0x30, 0x0c, 0x38, 0x00,
       0x18, 0x00, 0x1c, 0x00, 0x0c, 0x00, 0x0e, 0x00, 0x06, 0x00, 0x07,
       0x00, 0x03, 0x80, 0x03, 0x80, 0x01, 0x80, 0x01, 0x00, 0x00},
      {0x00, 0x00, 0xf0, 0x0f, 0xf8, 0x1f, 0x1c, 0x38, 0x0c, 0x30, 0x0c,
       0x30, 0x1c, 0x38, 0xf8, 0x1f, 0xf8, 0x1f, 0x1c, 0x38, 0x0c, 0x30,
       0x0c, 0x30, 0x1c, 0x38, 0xf8, 0x1f, 0xf0, 0x0f, 0x00, 0x00},
      {0x00, 0x00, 0xf0, 0x0f, 0xf8, 0x1f, 0x1c, 0x38, 0x0c, 0x30, 0x0c,
       0x30, 0x0c, 0x30, 0x1c, 0x30, 0xf8, 0x3f, 0xf0, 0x3f, 0x00, 0x38,
       0x00, 0x1c, 0x00, 0x0e, 0xfc, 0x07, 0xfc, 0x03, 0x00, 0x00},
  };
  uchar mask[32][4] = {{0}};
  uchar bitmap[32][4] = {{0}};

  /* Print number bottom right justified. */
  for (int idx = 3; nr && idx >= 0; idx--) {
    const uchar *digit = number_bitmaps[nr % 10];
    int x = idx % 2;
    int y = idx / 2;

    for (int i = 0; i < 16; i++) {
      bitmap[i + y * 16][x * 2] = digit[i * 2];
      bitmap[i + y * 16][(x * 2) + 1] = digit[(i * 2) + 1];
    }
    for (int i = 0; i < 16; i++) {
      mask[i + y * 16][x * 2] = 0xFF;
      mask[i + y * 16][(x * 2) + 1] = 0xFF;
    }

    nr /= 10;
  }

  return GHOST_SetCustomCursorShape(static_cast<GHOST_WindowHandle>(win->ghostwin),
                                    (uint8_t *)bitmap,
                                    (uint8_t *)mask,
                                    32,
                                    32,
                                    15,
                                    15,
                                    false) == GHOST_kSuccess;
}

static void wm_cursor_time_small(wmWindow *win, int nr)
{
  /* 10 8x8 digits. */
  const char number_bitmaps[10][8] = {
      {0, 56, 68, 68, 68, 68, 68, 56},
      {0, 24, 16, 16, 16, 16, 16, 56},
      {0, 60, 66, 32, 16, 8, 4, 126},
      {0, 124, 32, 16, 56, 64, 66, 60},
      {0, 32, 48, 40, 36, 126, 32, 32},
      {0, 124, 4, 60, 64, 64, 68, 56},
      {0, 56, 4, 4, 60, 68, 68, 56},
      {0, 124, 64, 32, 16, 8, 8, 8},
      {0, 60, 66, 66, 60, 66, 66, 60},
      {0, 56, 68, 68, 120, 64, 68, 56},
  };
  uchar mask[16][2] = {{0}};
  uchar bitmap[16][2] = {{0}};

  /* Print number bottom right justified. */
  for (int idx = 3; nr && idx >= 0; idx--) {
    const char *digit = number_bitmaps[nr % 10];
    int x = idx % 2;
    int y = idx / 2;

    for (int i = 0; i < 8; i++) {
      bitmap[i + y * 8][x] = digit[i];
    }
    for (int i = 0; i < 8; i++) {
      mask[i + y * 8][x] = 0xFF;
    }
    nr /= 10;
  }

  GHOST_SetCustomCursorShape(static_cast<GHOST_WindowHandle>(win->ghostwin),
                             (uint8_t *)bitmap,
                             (uint8_t *)mask,
                             16,
                             16,
                             7,
                             7,
                             false);
}

void WM_cursor_time(wmWindow *win, int nr)
{
  if (win->lastcursor == 0) {
    win->lastcursor = win->cursor;
  }

  /* Use `U.ui_scale` instead of `UI_SCALE_FAC` here to ignore HiDPI/Retina scaling. */
  if (U.ui_scale < 1.45f || !wm_cursor_time_large(win, nr)) {
    wm_cursor_time_small(win, nr);
  }

  /* Unset current cursor value so it's properly reset to wmWindow.lastcursor. */
  win->cursor = 0;
}

static void wm_add_cursor(
    WMCursorType cursor, const char *svg_source, float hotspot_x, float hotspot_y, float size)
{
  BlenderCursor[cursor].svg_source = svg_source;
  BlenderCursor[cursor].hotspot_x = hotspot_x;
  BlenderCursor[cursor].hotspot_y = hotspot_y;
  BlenderCursor[cursor].size = size;
}

void wm_init_cursor_data()
{
  wm_add_cursor(WM_CURSOR_DEFAULT, datatoc_cursor_pointer_svg, 0.0f, 0.0f, 1.0f);
  wm_add_cursor(WM_CURSOR_TEXT_EDIT, datatoc_cursor_text_edit_svg, 0.5f, 0.5f, 0.7f);
  wm_add_cursor(WM_CURSOR_STOP, datatoc_cursor_stop_svg, 0.5f, 0.5f, 0.9f);
  wm_add_cursor(WM_CURSOR_PAINT_BRUSH, datatoc_cursor_pencil_svg, 0.0f, 1.0f, 1.0f);
  wm_add_cursor(WM_CURSOR_EYEDROPPER, datatoc_cursor_eyedropper_svg, 0.0f, 1.0f, 1.0f);
  wm_add_cursor(WM_CURSOR_ERASER, datatoc_cursor_eraser_svg, 0.0f, 1.0f, 1.0f);
  wm_add_cursor(WM_CURSOR_EDIT, datatoc_cursor_crosshair_svg, 0.5f, 0.5f, 1.0f);
  wm_add_cursor(WM_CURSOR_CROSS, datatoc_cursor_crosshair_svg, 0.5f, 0.5f, 1.0f);
}
