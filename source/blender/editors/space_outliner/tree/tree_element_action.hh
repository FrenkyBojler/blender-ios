/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#pragma once

#include "tree_element.hh"

struct bAction;

namespace blender::ed::outliner {

class TreeElementAction final : public AbstractTreeElement {
  bAction &action_;

 public:
  TreeElementAction(TreeElement &legacy_te, bAction &action);

  void expand(SpaceOutliner &space_outliner) const override;
};

}  // namespace blender::ed::outliner
