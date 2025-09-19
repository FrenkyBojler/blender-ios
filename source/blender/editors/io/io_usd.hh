/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup editor/io
 */

struct wmOperatorType;

void WM_OT_usd_export(wmOperatorType *ot);
void WM_OT_usd_import(wmOperatorType *ot);
void UI_OT_usd_hook_descriptor_add(wmOperatorType *ot);
void UI_OT_usd_hook_descriptor_remove(wmOperatorType *ot);
void UI_OT_usd_hook_descriptor_move(wmOperatorType *ot);

namespace blender::ed::io {
void usd_file_handler_add();
}
