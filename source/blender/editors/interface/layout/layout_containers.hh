/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#pragma once

#include "UI_interface_layout.hh"
#include "layout_intern.hh"

namespace blender::ui {

struct ButtonItem : public Item {
  Button *but = nullptr;
  ButtonItem() : Item(ItemType::Button) {}
};

struct LayoutRow : public Layout {
  LayoutRow(LayoutRoot *root) : Layout(ItemType::LayoutRow, root) {}
  LayoutRow(ItemType type, LayoutRoot *root) : Layout(type, root) {}

  void estimate_impl() override;
  void resolve_impl() override;
};

struct LayoutColumn : public Layout {
  LayoutColumn(LayoutRoot *root) : Layout(ItemType::LayoutColumn, root) {}
  LayoutColumn(ItemType type, LayoutRoot *root) : Layout(type, root) {}

  void estimate_impl() override;
  void resolve_impl() override;
};

struct LayoutRootPieMenu : public Layout {
  LayoutRootPieMenu(LayoutRoot *root) : Layout(ItemType::LayoutRoot, root) {}
  void resolve_impl() override;
};

struct LayoutOverlap : public Layout {
  LayoutOverlap() : Layout(ItemType::LayoutOverlap, nullptr) {}

  void estimate_impl() override;
  void resolve_impl() override;
};

struct LayoutRadial : public Layout {
  LayoutRadial() : Layout(ItemType::LayoutRadial, nullptr) {}

  void estimate_impl() override {};
  void resolve_impl() override;
};

struct LayoutAbsolute : public Layout {
  LayoutAbsolute() : Layout(ItemType::LayoutAbsolute, nullptr) {}

  void estimate_impl() override;
  void resolve_impl() override;
};

struct LayoutItemFlow : public Layout {
  int number = 0;
  int totcol = 0;
  LayoutItemFlow() : Layout(ItemType::LayoutColumnFlow, nullptr) {}

  void estimate_impl() override;
  void resolve_impl() override;
};

struct LayoutItemGridFlow : public Layout {
  /* Extra parameters */
  bool row_major = false;    /* Fill first row first, instead of filling first column first. */
  bool even_columns = false; /* Same width for all columns. */
  bool even_rows = false;    /* Same height for all rows. */
  /**
   * - If positive, absolute fixed number of columns.
   * - If 0, fully automatic (based on available width).
   * - If negative, automatic but only generates number of columns/rows
   *   multiple of given (absolute) value.
   */
  int columns_len = 0;

  /* Pure internal runtime storage. */
  int tot_items = 0, tot_columns = 0, tot_rows = 0;

  LayoutItemGridFlow() : Layout(ItemType::LayoutGridFlow, nullptr) {}

  void estimate_impl() override;
  void resolve_impl() override;
};

struct LayoutItemBx : public LayoutColumn {
  Button *roundbox = nullptr;
  LayoutItemBx() : LayoutColumn(ItemType::LayoutBox, nullptr) {}

  void estimate_impl() override;
  void resolve_impl() override;
};

struct LayoutItemPanelHeader : public Layout {
  PointerRNA open_prop_owner;
  std::string open_prop_name;
  LayoutItemPanelHeader() : Layout(ItemType::LayoutPanelHeader, nullptr) {}

  void estimate_impl() override;
  void resolve_impl() override;
};

struct LayoutItemPanelBody : public LayoutColumn {
  LayoutItemPanelBody() : LayoutColumn(ItemType::LayoutPanelBody, nullptr) {}
  void resolve_impl() override;
};

struct LayoutItemSplit : public LayoutRow {
  float percentage = 0.0f;
  LayoutItemSplit() : LayoutRow(ItemType::LayoutSplit, nullptr) {}

  void estimate_impl() override;
  void resolve_impl() override;
};

}