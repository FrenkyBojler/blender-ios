/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Share between interface_region_*.cc files.
 */

#pragma once

#include "BLI_string_ref.hh"

namespace blender {

struct ARegion;
struct bContext;
struct bScreen;
struct wmEvent;
struct wmWindow;

namespace ui {

/* interface_region_menu_popup.cc */

uint popup_menu_hash(StringRef str);

/* interface_regions.cc */

ARegion *region_temp_add(bScreen *screen);
void region_temp_remove(bContext *C, bScreen *screen, ARegion *region);
const wmEvent *window_eventstate_source_get(const wmWindow *win);
void region_temp_xr_register(bContext *C, ARegion *region, ARegion *source_region);

}  // namespace ui
}  // namespace blender
