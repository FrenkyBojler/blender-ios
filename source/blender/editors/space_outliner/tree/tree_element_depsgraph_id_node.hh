/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#pragma once

#include "tree_element.hh"

namespace blender::ed::outliner {

struct DepsgraphIDNodeData {
  Depsgraph *depsgraph;
  ID *orig_id;
};

/**
 * TODO!
 */
class TreeElementDepsgraphIDNode final : public AbstractTreeElement {
  Depsgraph *depsgraph_;
  const ID &orig_id_;

  BIFIconID icon_ = ICON_NONE;

 public:
  TreeElementDepsgraphIDNode(TreeElement &legacy_te, const DepsgraphIDNodeData &data);

  void expand(SpaceOutliner & /*soops*/) const override;
};

}  // namespace blender::ed::outliner
