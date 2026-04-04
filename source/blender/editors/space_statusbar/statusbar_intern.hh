/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

namespace blender {

struct ARegionType;

#ifdef WITH_BLENDER_UPDATES_NOTIFICATIONS
void panel_blender_updates_register(ARegionType *region_type);
#endif

}  // namespace blender
