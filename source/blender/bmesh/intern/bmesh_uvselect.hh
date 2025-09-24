/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "bmesh_class.hh"

#include "BLI_vector_list.hh"

/** \file
 * \ingroup bmesh
 *
 * Manage UV sync-select,
 * where a selected vertex in the 3D viewport may only have some of it's
 * UV vertices selected in the UV editor.
 *
 * Supporting this involves flushing in both directions depending on the selection being edited.
 *
 * \note This is quite involved, as a last resort the UV selection can always be cleared
 * and re-set from the mesh (v3d) selection, however it's good to keep UV selection
 * if possible because resetting may extend vertex selection to other UV islands.
 */

/* -------------------------------------------------------------------- */
/** \name UV Selection Functions (low level)
 *
 * Selection checking functions.
 * These should be used instead of checking #BM_ELEM_SELECT_UV,
 * so hidden geometry is never considered selected.
 * \{ */

bool BM_face_uvselect_test(const BMFace *f);
bool BM_loop_vert_uvselect_test(const BMLoop *l);
bool BM_loop_edge_uvselect_test(const BMLoop *l);

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV Selection Connectivity Checks
 * \{ */

bool BM_loop_vert_uvselect_check_other_loop_vert(BMLoop *l, char hflag, int cd_loop_uv_offset);
bool BM_loop_vert_uvselect_check_other_loop_edge(BMLoop *l, char hflag, int cd_loop_uv_offset);
bool BM_loop_vert_uvselect_check_other_edge(BMLoop *l, char hflag, int cd_loop_uv_offset);
bool BM_loop_vert_uvselect_check_other_face(BMLoop *l, char hflag, int cd_loop_uv_offset);
bool BM_loop_edge_uvselect_check_other_loop_edge(BMLoop *l, char hflag, int cd_loop_uv_offset);
bool BM_loop_edge_uvselect_check_other_face(BMLoop *l, char hflag, int cd_loop_uv_offset);
bool BM_face_uvselect_check_edges_all(BMFace *f);

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV Selection Functions
 * \{ */

void BM_face_uvselect_set_noflush(BMesh *bm, BMFace *f, bool select);
void BM_face_uvselect_set(BMesh *bm, BMFace *f, bool select);
void BM_loop_edge_uvselect_set_noflush(BMesh *bm, BMLoop *l, bool select);
void BM_loop_edge_uvselect_set(BMesh *bm, BMLoop *l, bool select);
void BM_loop_vert_uvselect_set_noflush(BMesh *bm, BMLoop *l, bool select);

/**
 * Call this function when selecting mesh elements in the viewport and
 * the relationship with UV's is lost.
 *
 * By convention place this immediately after selection flushing.
 *
 * \return True if UV select is cleared (a change was made).
 */
bool BM_mesh_uvselect_clear(BMesh *bm);

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV Selection Functions (Shared)
 * \{ */

void BM_loop_vert_uvselect_set_shared(BMesh *bm,
                                      BMLoop *l,
                                      bool select,
                                      const int cd_loop_uv_offset);
void BM_loop_edge_uvselect_set_shared(BMesh *bm,
                                      BMLoop *l,
                                      bool select,
                                      const int cd_loop_uv_offset);
void BM_face_uvselect_set_shared(BMesh *bm, BMFace *f, bool select, const int cd_loop_uv_offset);

void BM_mesh_uvselect_set_elem_shared(BMesh *bm,
                                      bool select,
                                      const int cd_loop_uv_offset,
                                      const blender::Span<BMLoop *> loop_verts,
                                      const blender::Span<BMLoop *> loop_edges,
                                      const blender::Span<BMFace *> faces);

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV Selection Picking
 * \{ */

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

void BM_mesh_uvselect_set_elem_from_v3d(BMesh *bm,
                                        bool select,
                                        const BMUVSelectPickParams &params,
                                        const blender::VectorList<BMVert *> &verts,
                                        const blender::VectorList<BMEdge *> &edges,
                                        const blender::VectorList<BMFace *> &faces);
void BM_mesh_uvselect_set_elem_from_v3d(BMesh *bm,
                                        bool select,
                                        const BMUVSelectPickParams &params,
                                        const blender::Span<BMVert *> verts,
                                        const blender::Span<BMEdge *> edges,
                                        const blender::Span<BMFace *> faces);

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV Selection Flushing
 *
 * \note In most cases flushing assuming selection has already been flushed down.
 *
 * This means:
 * - A selected edge must have both UV vertices selected.
 * - A selected faces has all it's edges & vertices selected.
 * \{ */

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
/**
 * Mode independent UV selection/de-selection flush from UV vertices.
 *
 * \param select: When true, flush the selection state to de-selected elements,
 * otherwise perform the opposite, flushing de-selection.
 */
void BM_mesh_uvselect_flush_from_verts(BMesh *bm, bool select);

/**
 * Select elements based on the selection mode.
 * (flushes the selection *up* based on the mode).
 *
 * - With vertex selection mode enabled: flush up to edges and faces.
 * - With edge selection mode enabled: flush to faces.
 * - With *only* face selection mode enabled: do nothing.
 */
void BM_mesh_uvselect_flush_mode_only_select(BMesh *bm);
void BM_mesh_uvselect_flush_shared(BMesh *bm, int cd_loop_uv_offset);

/**
 * When the select mode changes, update to ensure the selection is valid.
 * So single vertices aren't selected in edge-select mode for example.
 *
 * The mesh selection flushing must have already run.
 */
void BM_mesh_uvselect_flush_mode_update(BMesh *bm,
                                        const short selectmode_old,
                                        const short selectmode_new,
                                        const int cd_loop_uv_offset);

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
 * A specialized flushing that fills in selection information after subdividing.
 *
 * It's important this runs:
 * - After subdivision.
 * - After the mesh selection has already been flushed.
 *
 * \note Intended to be a generic utility to be used in any situation
 * new geometry is created by splitting existing geometry.
 */
void BM_mesh_uvselect_flush_post_subdivide(BMesh *bm, const int cd_loop_uv_offset);

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV Selection Validation
 * \{ */

/** Between UV's and mesh selection. */
struct UVSelectValidateInfo_Sync {
  /** When a vertex is unselected none of it's UV's may be selected. */
  uint count_uv_vert_any_selected_with_vert_unselected = 0;
  /** When a vertex is selected at least one UV must be selected. */
  uint count_uv_vert_none_selected_with_vert_selected = 0;

  /** When a edge is unselected none of it's UV's may be selected. */
  uint count_uv_edge_any_selected_with_edge_unselected = 0;
  /** When a edge is selected at least one UV must be selected. */
  uint count_uv_edge_none_selected_with_edge_selected = 0;
};

/** Flushing between elements. */
struct UVSelectValidateInfo_Flush {
  /** Edges are selected without selected vertices. */
  uint count_uv_edge_selected_with_any_verts_unselected = 0;
  /** Edges are unselected with all selected vertices. */
  uint count_uv_edge_unselected_with_all_verts_selected = 0;

  /** Faces are selected without selected vertices. */
  uint count_uv_face_selected_with_any_verts_unselected = 0;
  /** Faces  are unselected with all selected vertices. */
  uint count_uv_face_unselected_with_all_verts_selected = 0;

  /** Faces are selected without selected edges. */
  uint count_uv_face_selected_with_any_edges_unselected = 0;
  /** Faces  are unselected with all selected edges. */
  uint count_uv_face_unselected_with_all_edges_selected = 0;
};

/** Flush & contiguous. */
struct UVSelectValidateInfo_Contiguous {
  /** When a vertices connected UV's are co-located without matching selection. */
  uint count_uv_vert_non_contiguous_selected = 0;
  /** When a edges connected UV's are co-located without matching selection. */
  uint count_uv_edge_non_contiguous_selected = 0;
};

struct UVSelectValidateInfo_FlushAndContiguous {
  /** A vertex is selected in edge/face modes without being part of a selected edge/face. */
  uint count_uv_vert_isolated_in_edge_or_face_mode = 0;
  /** A vertex is selected in face modes without being part of a selected face. */
  uint count_uv_vert_isolated_in_face_mode = 0;
  /** An edge is selected in face modes without being part of a selected face. */
  uint count_uv_edge_isolated_in_face_mode = 0;
};

struct UVSelectValidateInfo {
  UVSelectValidateInfo_Sync sync;

  /* These are optional. */

  UVSelectValidateInfo_Flush flush;
  UVSelectValidateInfo_Contiguous contiguous;
  UVSelectValidateInfo_FlushAndContiguous flush_contiguous;
};

/**
 * \param cd_loop_uv_offset: The UV custom-data layer to check.
 * Ignored when -1 (UV checks wont be used).
 *
 * \param check_sync: When true, check the selection is synchronized
 * between the UV and mesh selection. This should practically always be true,
 * as it doesn't make sense to check the UV selection if valid otherwise,
 * unless the UV selection is being set and has not yet been synchronized.
 * \param check_flush: When true, check the selection is flushed based on #BMesh::selectmode.
 * \param check_contiguous: When true, check that UV selection is contiguous.
 * Note that this is not considered an *error* since users may cause this to happen and
 * tools are expected to work properly, however some operations are expected to maintain
 * a contiguous selection. This check is included to ensure those operations are working.
 */
bool BM_mesh_uvselect_check(BMesh *bm,
                            int cd_loop_uv_offset,
                            bool check_sync,
                            bool check_flush,
                            bool check_contiguous,
                            UVSelectValidateInfo *info);

/** \} */
