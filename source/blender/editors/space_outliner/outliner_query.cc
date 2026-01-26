/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include <functional>

#include "BLI_listbase.h"

#include "DNA_space_types.h"

#include "outliner_intern.hh"
#include "tree/tree_display.hh"

namespace blender::ed::outliner {

bool outliner_shows_mode_column(const SpaceOutliner &space_outliner)
{
  const AbstractTreeDisplay &tree_display = *space_outliner.runtime->tree_display;

  return tree_display.supports_mode_column() && (space_outliner.flag & SO_MODE_COLUMN);
}

static bool outliner_has_element_warnings_recursive(const ListBaseT<TreeElement> &lb)
{
  for (const TreeElement &te : lb) {
    if (te.abstract_element && te.abstract_element->have_warning()) {
      return true;
    }

    if (outliner_has_element_warnings_recursive(te.subtree)) {
      return true;
    }
  }
  return false;
}

bool outliner_has_element_warnings(const SpaceOutliner &space_outliner)
{
  return outliner_has_element_warnings_recursive(space_outliner.tree);
}

}  // namespace blender::ed::outliner
