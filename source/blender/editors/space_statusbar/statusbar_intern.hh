/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

namespace blender {

struct ARegionType;

void panel_blender_updates_register(ARegionType *region_type);
void operator_check_for_updates(wmOperatorType *ot);

}  // namespace blender
