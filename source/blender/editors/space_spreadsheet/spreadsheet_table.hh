/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_space_types.h"

namespace blender::ed::spreadsheet {

SpreadsheetTableIDGeometry *spreadsheet_table_id_new_geometry();
SpreadsheetTableID *spreadsheet_table_id_copy(const SpreadsheetTableID &src_table_id);
void spreadsheet_table_id_free(SpreadsheetTableID *table_id);
void spreadsheet_table_id_blend_write(BlendWriter *writer, const SpreadsheetTableID *table_id);
void spreadsheet_table_id_blend_read(BlendDataReader *reader, SpreadsheetTableID *table_id);

SpreadsheetTable *spreadsheet_table_new(SpreadsheetTableID *table_id);
SpreadsheetTable *spreadsheet_table_copy(const SpreadsheetTable &src_table);
void spreadsheet_table_free(SpreadsheetTable *table);
void spreadsheet_table_blend_write(BlendWriter *writer, const SpreadsheetTable *table);
void spreadsheet_table_blend_read(BlendDataReader *reader, SpreadsheetTable *table);

}  // namespace blender::ed::spreadsheet
