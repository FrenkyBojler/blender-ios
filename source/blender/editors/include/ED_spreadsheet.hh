/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

struct ID;
struct SpaceSpreadsheet;

namespace blender::ed::spreadsheet {

ID *get_current_id(const SpaceSpreadsheet *sspreadsheet);

int get_visible_rows_num(const SpaceSpreadsheet &sspreadsheet);
int get_total_rows_num(const SpaceSpreadsheet &sspreadsheet);
int get_columns_num(const SpaceSpreadsheet &sspreadsheet);

}  // namespace blender::ed::spreadsheet
