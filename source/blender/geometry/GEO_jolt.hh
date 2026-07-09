/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

namespace blender::geometry::jolt {

/** Makes sure that Jolt is properly initialized so that its API can be used. */
void ensure_initialization();

}  // namespace blender::geometry::jolt
