/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <string>

#include "BLI_string_ref.hh"

struct bContext;
struct uiLayout;

/** #uiBlock.emboss and #uiBut.emboss */
enum eUIEmbossType {
  /** Use widget style for drawing. */
  UI_EMBOSS = 0,
  /** Nothing, only icon and/or text */
  UI_EMBOSS_NONE = 1,
  /** Pull-down menu style */
  UI_EMBOSS_PULLDOWN = 2,
  /** Pie Menu */
  UI_EMBOSS_PIE_MENU = 3,
  /**
   * The same as #UI_EMBOSS_NONE, unless the button has
   * a coloring status like an animation state or red alert.
   */
  UI_EMBOSS_NONE_OR_STATUS = 4,
  /** For layout engine, use emboss from block. */
  UI_EMBOSS_UNDEFINED = 255,
};

/* Menu Callbacks */

using uiMenuCreateFunc = void (*)(bContext *C, uiLayout *layout, void *arg1);
using uiMenuHandleFunc = void (*)(bContext *C, void *arg, int event);

/**
 * Used for cycling menu values without opening the menu (Ctrl-Wheel).
 * \param direction: forward or backwards [1 / -1].
 * \param arg1: uiBut.poin (as with #uiMenuCreateFunc).
 * \return true when the button was changed.
 */
using uiMenuStepFunc = bool (*)(bContext *C, int direction, void *arg1);

using uiCopyArgFunc = void *(*)(const void *arg);
using uiFreeArgFunc = void (*)(void *arg);

/** Must return an allocated string. */
using uiButToolTipFunc = std::string (*)(bContext *C, void *argN, blender::StringRef tip);
