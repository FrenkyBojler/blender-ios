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

}  // namespace blender::ed::outliner
