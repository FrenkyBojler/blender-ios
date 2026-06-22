/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

namespace blender {

struct Depsgraph;
struct Mesh;
struct Object;

namespace io {

/**
 * Conditionally coerce objects to mesh for export.
 * For use by formats that only support meshes.
 */
struct MeshCoerceForExport {
  /**
   * Mesh to read geometry from, referencing evaluated, pre-modified or `owned` data.
   *
   * The same as `owned` when a conversion was performed.
   */
  const Mesh *mesh = nullptr;
  /**
   * Mesh converted from a non-mesh, or null.
   * Freed by #mesh_coerce_for_export_end.
   */
  Mesh *owned = nullptr;
};

/**
 * Return the mesh to export for `obj_eval`.
 *
 * The mesh may be converted on demand and stored in #MeshCoerceForExport::owned.
 *
 * \return The mesh to export.
 */
const Mesh *mesh_coerce_for_export_begin(MeshCoerceForExport &coerce,
                                         Depsgraph *depsgraph,
                                         Object *obj_eval,
                                         bool apply_modifiers);

/**
 * Free any temporary mesh allocated by #mesh_coerce_for_export_begin.
 */
void mesh_coerce_for_export_end(MeshCoerceForExport &coerce);

}  // namespace io
}  // namespace blender
