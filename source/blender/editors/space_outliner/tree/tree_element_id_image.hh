/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#pragma once

#include "UI_interface_c.hh"
#include "tree_element_id.hh"

namespace blender {
struct ID;
struct bContext;
struct Image;

namespace ed::outliner {

ARegion *image_tooltip_fn(bContext *C, ID *id, const int xy[2])
{
  std::unique_ptr<ui::TooltipData> data = std::make_unique<ui::TooltipData>();
  tooltip_from_image(*id_cast<Image *>(id), *data);
  const float init_position[2] = {float(xy[0]) + 35.0f * UI_SCALE_FAC,
                                  float(xy[1]) + 35.0f * UI_SCALE_FAC};
  return tooltip_create_with_data(C, std::move(data), init_position, nullptr);
}

class TreeElementIDImage final : public TreeElementID {
 public:
  TreeElementIDImage(TreeElement &legacy_te, ID &id) : TreeElementID(legacy_te, id)
  {
    this->tooltip_fn = image_tooltip_fn;
  }
};

}  // namespace ed::outliner
}  // namespace blender
