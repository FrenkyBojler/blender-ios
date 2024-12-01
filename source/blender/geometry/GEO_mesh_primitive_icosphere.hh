/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

struct Mesh;
namespace blender {
class StringRef;
}  // namespace blender

namespace blender::geometry {

Mesh *create_icosphere_mesh(int resolution, float radius, const std::optional<StringRef> &uv_id);

}  // namespace blender::geometry
