/* SPDX-FileCopyrightText: 2013 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include "BLI_vector.hh"

#include "DNA_space_types.h"

namespace blender {

struct ScrArea;
struct SpaceProperties;
struct bContext;
struct PointerRNA;

namespace ui {
struct Layout;
}  // namespace ui

/**
 * Fills an array with the tab context values for the properties editor. -1 signals a separator.
 *
 * \return The total number of items in the array returned.
 */
Vector<eSpaceButtons_Context> ED_buttons_tabs_list(const SpaceProperties *sbuts,
                                                   bool apply_filter = true);
void ED_buttons_visible_tabs_menu(bContext *C, ui::Layout *layout, void * /*arg*/);
void ED_buttons_navbar_menu(bContext *C, ui::Layout *layout, void * /*arg*/);
bool ED_buttons_tab_has_search_result(SpaceProperties *sbuts, int index);

void ED_buttons_search_string_set(SpaceProperties *sbuts, const char *value);
int ED_buttons_search_string_length(SpaceProperties *sbuts);
const char *ED_buttons_search_string_get(SpaceProperties *sbuts);

bool ED_buttons_use_property_search_get(const SpaceProperties *sbuts);
void ED_buttons_use_property_search_set(SpaceProperties *sbuts, bool value);

bool ED_buttons_should_sync_with_outliner(const bContext *C,
                                          const SpaceProperties *sbuts,
                                          ScrArea *area);
void ED_buttons_set_context(const bContext *C,
                            SpaceProperties *sbuts,
                            PointerRNA *ptr,
                            int context);

/**
 * Draw the context breadcrumb path (icon + name entries separated by right-arrow) for the
 * Properties Editor into \a layout. Does nothing when the Tool context is active or the path
 * is empty. Intended for use in the header region.
 */
void ED_space_properties_context_path_draw(ui::Layout *layout, const bContext *C);

}  // namespace blender
