/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "bmesh_class.hh"

/** \file
 * \ingroup bmesh
 *
 * Manage UV sync-select,
 * where a selected vertex in the 3D viewport may only have some of it's
 * UV vertices selected in the UV editor.
 *
 * Supporting this involves flushing in both direction.
 */

void BM_mesh_uvselect_flush_from_loop_verts(BMesh *bm);
void BM_mesh_uvselect_flush_from_loop_verts_only_select(BMesh *bm);
void BM_mesh_uvselect_flush_from_loop_verts_only_deselect(BMesh *bm);
void BM_mesh_uvselect_flush_from_loop_edges_only_select(BMesh *bm);
void BM_mesh_uvselect_flush_from_loop_edges_only_deselect(BMesh *bm);
void BM_mesh_uvselect_flush_from_loop_edges(BMesh *bm, bool flush_down);
void BM_mesh_uvselect_flush_from_faces(BMesh *bm, bool flush_down);
void BM_mesh_uvselect_flush_from_faces_only_select(BMesh *bm);
void BM_mesh_uvselect_flush_from_faces_only_deselect(BMesh *bm);
void BM_mesh_uvselect_flush_mode(BMesh *bm);
void BM_mesh_uvselect_flush_shared(BMesh *bm, int cd_loop_uv_offset);

/**
 * When the select mode changes, update to ensure the selection is valid.
 * So single vertices aren't selected in edge-select mode for example.
 *
 * The mesh selection flushing must have already run.
 */
void BM_mesh_uvselect_selectmode_update(BMesh *bm,
                                        const short selectmode_old,
                                        const short selectmode_new,
                                        const int cd_loop_uv_offset);

bool BM_loop_vert_uvselect_check_other_loop_vert(BMLoop *l, char hflag, int cd_loop_uv_offset);
bool BM_loop_vert_uvselect_check_other_loop_edge(BMLoop *l, char hflag, int cd_loop_uv_offset);
bool BM_loop_vert_uvselect_check_other_edge(BMLoop *l, char hflag, int cd_loop_uv_offset);
bool BM_loop_vert_uvselect_check_other_face(BMLoop *l, char hflag, int cd_loop_uv_offset);
bool BM_loop_edge_uvselect_check_other_loop_edge(BMLoop *l, char hflag, int cd_loop_uv_offset);
bool BM_loop_edge_uvselect_check_other_face(BMLoop *l, char hflag, int cd_loop_uv_offset);

/* Selection checking functions.
 * These should be used instead of checking #BM_ELEM_SELECT_UV,
 * so hidden geometry is never considered selected.
 */

bool BM_face_uvselect_test(const BMFace *f);
bool BM_loop_vert_uvselect_test(const BMLoop *l);
bool BM_loop_edge_uvselect_test(const BMLoop *l);

bool BM_face_uvselect_check_edges_all(BMFace *f);

struct BMUVSelectPickParams {
  int cd_loop_uv_offset = -1;
  /**
   * Derived from #ToolSettings::uv_sticky
   * A boolean can be used since "Shared Vertex"
   * doesn't require #BM_ELEM_SELECT_UV at all.
   */
  bool shared = true;
};

void BM_vert_uvselect_set_pick(BMesh *bm,
                               BMVert *v,
                               bool select,
                               const BMUVSelectPickParams &params);
void BM_edge_uvselect_set_pick(BMesh *bm,
                               BMEdge *e,
                               bool select,
                               const BMUVSelectPickParams &params);
void BM_face_uvselect_set_pick(BMesh *bm,
                               BMFace *f,
                               bool select,
                               const BMUVSelectPickParams &params);

void BM_face_uvselect_set_noflush(BMesh *bm, BMFace *f, bool select);
void BM_face_uvselect_set(BMesh *bm, BMFace *f, bool select);
void BM_loop_edge_uvselect_set_noflush(BMesh *bm, BMLoop *l, bool select);
void BM_loop_edge_uvselect_set(BMesh *bm, BMLoop *l, bool select);
void BM_loop_vert_uvselect_set_noflush(BMesh *bm, BMLoop *l, bool select);

void BM_loop_vert_uvselect_set_shared(BMesh *bm,
                                      BMLoop *l,
                                      bool select,
                                      const int cd_loop_uv_offset);
void BM_loop_edge_uvselect_set_shared(BMesh *bm,
                                      BMLoop *l,
                                      bool select,
                                      const int cd_loop_uv_offset);
void BM_face_uvselect_set_shared(BMesh *bm, BMFace *f, bool select, const int cd_loop_uv_offset);

/**
 * From 3D viewport to UV selection.
 */
void BM_mesh_uvselect_flush_from_v3d_sticky_location(BMesh *bm, const int cd_loop_uv_offset);
void BM_mesh_uvselect_flush_from_v3d_sticky_disabled(BMesh *bm);
void BM_mesh_uvselect_flush_from_v3d_sticky_vertex(BMesh *bm);

/**
 * From the UV selection to the 3D viewport.
 */
void BM_mesh_uvselect_flush_to_v3d(BMesh *bm);

/**
 * Call this function when selecting mesh elements in the viewport and
 * the relationship with UV's is lost.
 *
 * By convention place this immediately after selection flushing.
 *
 * \return True if UV select is cleared (a change was made).
 */
bool BM_mesh_uvselect_clear(BMesh *bm);
