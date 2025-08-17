/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#pragma once

#include "tree_element.hh"

namespace blender::ed::outliner {

/**
 * TODO!
 */
class TreeElementDepsgraphIDNode final : public AbstractTreeElement {
  const ID &orig_id_;
  BIFIconID icon_ = ICON_NONE;

 public:
  TreeElementDepsgraphIDNode(TreeElement &legacy_te,  const ID &orig_id);
};

}  // namespace blender::ed::outliner
