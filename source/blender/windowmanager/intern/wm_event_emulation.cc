/* SPDX-FileCopyrightText: 2007 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Handle event emulation.
 */

#include <cstring>
#include <fmt/format.h>

#include "AS_asset_library.hh"

#include "DNA_userdef_types.h"

#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "BKE_customdata.hh"
#include "BKE_undo_system.hh"

#include "ED_asset.hh"

#include "UI_interface.hh"

#include "WM_types.hh"

#include "wm_event_emulation.hh" /* Own include. */

#include "wm_event_types.hh"
#include "wm_surface.hh"

/**
 * Translate any mouse button event from tablet pen as LMB.
 */
static void enforce_tablet_pen_stroke_lmb(wmEvent *event)
{
  if (event->tablet.active && ISMOUSE_BUTTON(event->type) &&
      (U.flag & USER_FLAG_PEN_BARREL_AS_LMB))
  {
    /* Mouse buttons are configured to be locked to left mouse button. */
    event->type = LEFTMOUSE;
  }
}

/**
 * Numeric-pad emulation.
 */
static void emulate_numpad(wmEvent *event)
{
  if (U.flag & USER_NONUMPAD) {
    switch (event->type) {
      case EVT_ZEROKEY:
        event->type = EVT_PAD0;
        break;
      case EVT_ONEKEY:
        event->type = EVT_PAD1;
        break;
      case EVT_TWOKEY:
        event->type = EVT_PAD2;
        break;
      case EVT_THREEKEY:
        event->type = EVT_PAD3;
        break;
      case EVT_FOURKEY:
        event->type = EVT_PAD4;
        break;
      case EVT_FIVEKEY:
        event->type = EVT_PAD5;
        break;
      case EVT_SIXKEY:
        event->type = EVT_PAD6;
        break;
      case EVT_SEVENKEY:
        event->type = EVT_PAD7;
        break;
      case EVT_EIGHTKEY:
        event->type = EVT_PAD8;
        break;
      case EVT_NINEKEY:
        event->type = EVT_PAD9;
        break;
      case EVT_MINUSKEY:
        event->type = EVT_PADMINUS;
        break;
      case EVT_EQUALKEY:
        event->type = EVT_PADPLUSKEY;
        break;
      case EVT_BACKSLASHKEY:
        event->type = EVT_PADSLASHKEY;
        break;
      default:
        /* pass */
        break;
    }
  }
}

/**
 * Track the last released button to look for double press.
 */
static bool check_double_press(wmEvent *event, bool test_only)
{
  using namespace std::chrono;

  static int last_pressed_button = EVENT_NONE;
  static time_point<steady_clock> last_pressed_time{};

  if (!event) {
    last_pressed_button = EVENT_NONE;
    return false;
  }

  const time_point<steady_clock> now = steady_clock::now();

  if (!ISKEYBOARD_OR_BUTTON(event->type) || event->val != KM_PRESS) {
    return false;
  }

  if (event->type == last_pressed_button) {
    const int elapsed = std::chrono::duration_cast<milliseconds>(now - last_pressed_time).count();
    if (elapsed <= U.dbl_click_time) {
      return true;
    }
  }

  if (!test_only) {
    last_pressed_button = event->type;
    last_pressed_time = now;
  }

  return false;
}

static uint8_t event_to_modifier(short event)
{
  switch (event) {
    case EVT_LEFTCTRLKEY:
    case EVT_RIGHTCTRLKEY:
      return KM_CTRL;
    case EVT_LEFTALTKEY:
    case EVT_RIGHTALTKEY:
      return KM_ALT;
    case EVT_LEFTSHIFTKEY:
    case EVT_RIGHTSHIFTKEY:
      return KM_SHIFT;
    case EVT_OSKEY:
      return KM_OSKEY;
    default:
      return KM_NOTHING;
  }
}

static uint8_t event_to_modifier(const wmEvent *event)
{
  return event_to_modifier(event->keymodifier) | event_to_modifier(event->type) | event->modifier;
}

static int test_emulate_mb(const wmEvent *event, bool is_double_press, int source1, int source2)
{
  BLI_assert(event->val == KM_PRESS);

  source1 = ISKEYBOARD_OR_BUTTON(source1) ? source1 : 0;
  source2 = ISKEYBOARD_OR_BUTTON(source2) ? source2 : 0;
  if (source1) {
    const int mod_event = event_to_modifier(event);
    const int mod_source1 = event_to_modifier(source1);
    const int mod_source2 = event_to_modifier(source2);
    const int mod_source = mod_source1 | mod_source2;
    if (source1 == source2) {
      if (is_double_press && event->type == source1) {
        return source1;
      }
    }
    else if (source2 && event->type == source2 &&
             (event->keymodifier == source1 || (mod_source && mod_event == mod_source)))
    {
      return source2;
    }
    else if (source2 && event->type == source1 &&
             (event->keymodifier == source2 || (mod_source && mod_event == mod_source)))
    {
      return source1;
    }
    else if (!source2 && event->type == source1) {
      return source1;
    }
  }
  else if (source2 && event->type == source2) {
    return source2;
  }

  return false;
}

/**
 * Emulate RMB/MMB from LMB/RMB+Mod1+Mod2.
 */
template<int MOUSEBUTTON>
static bool emulate_rmbmmb(wmEvent *event, bool test_only, bool is_double_press)
{
  BLI_STATIC_ASSERT(MOUSEBUTTON == RIGHTMOUSE || MOUSEBUTTON == MIDDLEMOUSE,
                    "Invalid MOUSEBUTTON value provided");

  const int16_t source_mouse = MOUSEBUTTON == RIGHTMOUSE ? U.rmb_emulate_source_type_mouse :
                                                           U.mmb_emulate_source_type_mouse;
  int16_t source_nonmouse_1 = MOUSEBUTTON == RIGHTMOUSE ? U.rmb_emulate_source_type_nonmouse_1 :
                                                          U.mmb_emulate_source_type_nonmouse_1;
  int16_t source_nonmouse_2 = MOUSEBUTTON == RIGHTMOUSE ? U.rmb_emulate_source_type_nonmouse_2 :
                                                          U.mmb_emulate_source_type_nonmouse_2;

  /* Store which event triggered the reinterpretation of the emulating event. */
  static int emulating_event_source = EVENT_NONE;

  /* Store which event triggered the reinterpretation of the upcoming event. */
  static int upcoming_event_source = EVENT_NONE;

  const int killed_modifiers = event_to_modifier(source_nonmouse_1) | event_to_modifier(source_nonmouse_2);

  bool kill_event = false;

  if (U.runtime.is_ui_button_waiting_key_event || !ISMOUSE_BUTTON(source_mouse) ||
      (!ISKEYBOARD_OR_BUTTON(source_nonmouse_1) && !ISKEYBOARD_OR_BUTTON(source_nonmouse_2)))
  {
    /* Either the feature is misconfigured,
     * or the user is entering a key to use as the modifier for this feature. */
    if (!test_only) {
      /* Temporarily disable this feature, so that the key goes through. */
      upcoming_event_source = EVENT_NONE;
    }

    return kill_event;
  }

  if (event->type == source_mouse) {
    /* Mouse buttons emulation. */
    if (emulating_event_source) {
      event->modifier &= ~killed_modifiers;
      if (event->val == KM_RELEASE) {
        event->type = MOUSEBUTTON;

        if (!test_only) {
          emulating_event_source = EVENT_NONE;
          /* Release tracked double-press state,
           * to force a new double press once this emulation is over.*/
          check_double_press(nullptr, test_only);
        }
      }
    }
    else if (event->val == KM_PRESS && upcoming_event_source) {
      event->type = MOUSEBUTTON;
      event->modifier &= ~killed_modifiers;

      if (!test_only) {
        emulating_event_source = upcoming_event_source;
      }
    }

    return kill_event;
  }

  if (!(ISKEYBOARD(event->type)) && !(ISTABLET_BUTTON(event->type))) {
    return kill_event;
  }

  /* Track pressed keys that are repurposed as modifier keys.
   * Standard flow for the repurposed key will be suppressed. */
  if (event->val == KM_PRESS && !upcoming_event_source) {
    if (int source_new = test_emulate_mb(
            event, is_double_press, source_nonmouse_1, source_nonmouse_2))
    {
      if (!test_only) {
        upcoming_event_source = source_new;
      }

      kill_event = true;
    }
  }
  else if (upcoming_event_source == event->type) {
    if (event->val == KM_RELEASE && !test_only) {
      upcoming_event_source = EVENT_NONE;
    }

    kill_event = true;
  }

  return kill_event;
}

void WM_eventemulation(wmEvent *event, bool test_only)
{
  const bool is_double_press = check_double_press(event, test_only);

  bool kill_event = false;

  emulate_numpad(event);
  enforce_tablet_pen_stroke_lmb(event);
  kill_event |= emulate_rmbmmb<RIGHTMOUSE>(event, test_only, is_double_press);
  kill_event |= emulate_rmbmmb<MIDDLEMOUSE>(event, test_only, is_double_press);
  if (kill_event) {
    /* Prevent the event from being processed any further. */
    event->type = EVENT_NONE;
    event->val = KM_NOTHING;
  }
}
