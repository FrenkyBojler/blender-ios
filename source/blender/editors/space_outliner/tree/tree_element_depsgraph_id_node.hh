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
 private:
  Depsgraph *depsgraph_;
  const ID *orig_id_;

 public:
  TreeElementDepsgraphIDNode(TreeElement &legacy_te, const DepsgraphIDNodeData &data);

  void expand(SpaceOutliner & /*soops*/) const override;

  std::optional<double> node_evaluation_time() const;
  std::optional<float> node_evaluation_percent() const;

  private:
    void expand_scene(const Scene *scene) const;
};

}  // namespace blender::ed::outliner
