/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_string_ref.hh"

struct uiLayout;
struct PointerRNA;
struct uiList;

namespace blender::ui {

struct ILayout {
 private:
  uiLayout *layout_;

 public:
  constexpr ILayout() : layout_{} {};
  constexpr ILayout(uiLayout *layout) : layout_{layout} {};

  constexpr operator bool() const
  {
    return layout_;
  }

  constexpr operator uiLayout *() const
  {
    return layout_;
  }

  ILayout split(float percentage, bool align);
  ILayout box();
  ILayout list_box(uiList *ui_list, PointerRNA *actptr, PropertyRNA *actprop);
  ILayout radial();
  ILayout row(bool align);
  ILayout column(bool align);
  ILayout column_flow(int number, bool align);
  ILayout column_with_heading(bool align, const StringRef heading);

  ILayout grid_flow(
      bool row_major, int columns_len, bool even_columns, bool even_rows, bool align);
};

}  // namespace blender::ui
