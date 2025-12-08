/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

struct Panel;

namespace blender::ui {
bool panel_is_hidden_from_search(const Panel *panel);
void panel_is_hidden_from_search_set(Panel *panel, bool value);
}  // namespace blender::ui
