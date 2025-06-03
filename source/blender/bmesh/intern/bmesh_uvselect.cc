/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * Overview
 * ========
 *
 * The `BM_uvselect_*` API deals with synchronizing selection
 * between UV's and selected vertices edges & faces.
 *
 * \note A short-hand term for vertex/edge/face selection used
 * in this file is View3D abbreviated to `v3d`, since this is the section
 * manipulated in the viewport, e.g. #BM_mesh_uvselect_flush_to_v3d.
 *
 * UV Selection Flags
 * ==================
 *
 * - UV selection uses:
 *   - #BM_ELEM_SELECT_UV & #BM_ELEM_SELECT_UV_EDGE for #BMLoop
 *     to define selected vertices & edges.
 *   - #BM_ELEM_SELECT_UV for #BMFace.
 *
 * Valid State
 * -----------
 *
 * - When vertex is selected in the viewport at least one of the UV's must be selected.
 *
 * Hidden Flags
 * ------------
 *
 * Unlike viewport selection there is no requirement for hidden elements not to be selected.
 * Therefor, UV selection checks must check the underlying geometry is not hidden.
 * In practice this means hidden faces must be assumed unselected,
 * since UV's are part of the faces (there is no such thing as a hidden face-corner)
 * and any hidden edge or vertex causes connected faces to be hidden.
 *
 * TODO:
 * - Document when a UV face is unselected when the underlying face is selected.
 */

#include "MEM_guardedalloc.h"

#include "DNA_scene_types.h"

#include "BLI_listbase.h"
#include "BLI_math_bits.h"

#include "bmesh.hh"
#include "bmesh_structure.hh"

/* -------------------------------------------------------------------- */
/** \name UV Selection Functions (low level)
 * \{ */

bool BM_loop_vert_uvselect_test(const BMLoop *l)
{
  return (!BM_elem_flag_test(l->f, BM_ELEM_HIDDEN) && BM_elem_flag_test(l, BM_ELEM_SELECT_UV));
}
bool BM_loop_edge_uvselect_test(const BMLoop *l)
{
  return (!BM_elem_flag_test(l->f, BM_ELEM_HIDDEN) &&
          BM_elem_flag_test(l, BM_ELEM_SELECT_UV_EDGE));
}

bool BM_face_uvselect_test(const BMFace *f)
{
  return (!BM_elem_flag_test(f, BM_ELEM_HIDDEN) && BM_elem_flag_test(f, BM_ELEM_SELECT_UV));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV Selection Functions
 * \{ */

bool BM_loop_vert_uvselect_check_other_loop_vert(BMLoop *l,
                                                 const char hflag,
                                                 const int cd_loop_uv_offset)
{
  BLI_assert(ELEM(hflag, BM_ELEM_SELECT_UV, BM_ELEM_TAG));
  BMVert *v = l->v;
  BLI_assert(v->e);
  const BMEdge *e_iter, *e_first;
  e_iter = e_first = v->e;
  do {
    if (e_iter->l) {
      BMLoop *l_first = e_iter->l;
      BMLoop *l_iter = l_first;
      do {
        if (l_iter->v == v) {
          if (!BM_elem_flag_test(l_iter->f, BM_ELEM_HIDDEN)) {
            if (l_iter != l) {
              if (BM_elem_flag_test(l_iter, hflag)) {
                if (BM_loop_uv_share_vert_check(l, l_iter, cd_loop_uv_offset)) {
                  return true;
                }
              }
            }
          }
        }
      } while ((l_iter = l_iter->radial_next) != l_first);
    }
  } while ((e_iter = bmesh_disk_edge_next(e_iter, v)) != e_first);
  return false;
}

bool BM_loop_vert_uvselect_check_other_loop_edge(BMLoop *l,
                                                 const char hflag,
                                                 const int cd_loop_uv_offset)
{
  BLI_assert(ELEM(hflag, BM_ELEM_SELECT_UV_EDGE, BM_ELEM_TAG));
  BMVert *v = l->v;
  BLI_assert(v->e);
  const BMEdge *e_iter, *e_first;
  e_iter = e_first = v->e;
  do {
    if (e_iter->l) {
      BMLoop *l_first = e_iter->l;
      BMLoop *l_iter = l_first;
      do {
        if (l_iter->v == v) {
          /* Connected to a selected edge. */
          if (!BM_elem_flag_test(l_iter->f, BM_ELEM_HIDDEN)) {
            if (l_iter != l) {
              if (BM_elem_flag_test(l_iter, hflag) || BM_elem_flag_test(l_iter->prev, hflag)) {
                if (BM_loop_uv_share_vert_check(l, l_iter, cd_loop_uv_offset)) {
                  return true;
                }
              }
            }
          }
        }
      } while ((l_iter = l_iter->radial_next) != l_first);
    }
  } while ((e_iter = bmesh_disk_edge_next(e_iter, v)) != e_first);
  return false;
}

bool BM_loop_vert_uvselect_check_other_edge(BMLoop *l,
                                            const char hflag,
                                            const int cd_loop_uv_offset)
{
  BLI_assert(ELEM(hflag, BM_ELEM_SELECT, BM_ELEM_TAG));
  BMVert *v = l->v;
  BLI_assert(v->e);
  const BMEdge *e_iter, *e_first;
  e_iter = e_first = v->e;
  do {
    if (e_iter->l) {
      BMLoop *l_first = e_iter->l;
      BMLoop *l_iter = l_first;
      do {
        if (l_iter->v == v) {
          /* Connected to a selected edge. */
          if (!BM_elem_flag_test(l_iter->f, BM_ELEM_HIDDEN)) {
            if (l_iter != l) {
              if (((!BM_elem_flag_test(l_iter->e, BM_ELEM_HIDDEN)) &&
                   BM_elem_flag_test(l_iter->e, hflag)) ||
                  ((!BM_elem_flag_test(l_iter->prev->e, BM_ELEM_HIDDEN)) &&
                   BM_elem_flag_test(l_iter->prev->e, hflag)))
              {
                if (BM_loop_uv_share_vert_check(l, l_iter, cd_loop_uv_offset)) {
                  return true;
                }
              }
            }
          }
        }
      } while ((l_iter = l_iter->radial_next) != l_first);
    }
  } while ((e_iter = bmesh_disk_edge_next(e_iter, v)) != e_first);
  return false;
}

bool BM_loop_vert_uvselect_check_other_face(BMLoop *l,
                                            const char hflag,
                                            const int cd_loop_uv_offset)
{
  BLI_assert(ELEM(hflag, BM_ELEM_SELECT, BM_ELEM_SELECT_UV, BM_ELEM_TAG));
  BMVert *v = l->v;
  BLI_assert(v->e);
  const BMEdge *e_iter, *e_first;
  e_iter = e_first = v->e;
  do {
    if (e_iter->l) {
      BMLoop *l_first = e_iter->l;
      BMLoop *l_iter = l_first;
      do {
        if (l_iter->v == v) {
          if (!BM_elem_flag_test(l_iter->f, BM_ELEM_HIDDEN)) {
            if (l_iter != l) {
              if (BM_elem_flag_test(l_iter->f, hflag)) {
                if (BM_loop_uv_share_vert_check(l, l_iter, cd_loop_uv_offset)) {
                  return true;
                }
              }
            }
          }
        }
      } while ((l_iter = l_iter->radial_next) != l_first);
    }
  } while ((e_iter = bmesh_disk_edge_next(e_iter, v)) != e_first);
  return false;
}

bool BM_loop_edge_uvselect_check_other_loop_edge(BMLoop *l,
                                                 const char hflag,
                                                 const int cd_loop_uv_offset)
{
  BLI_assert(ELEM(hflag, BM_ELEM_SELECT, BM_ELEM_SELECT_UV_EDGE, BM_ELEM_TAG));
  BMLoop *l_iter = l;
  do {
    if (!BM_elem_flag_test(l_iter->f, BM_ELEM_HIDDEN)) {
      if (l_iter != l) {
        if (BM_elem_flag_test(l_iter, hflag)) {
          if (BM_loop_uv_share_edge_check(l, l_iter, cd_loop_uv_offset)) {
            return true;
          }
        }
      }
    }
  } while ((l_iter = l_iter->radial_next) != l);
  return false;
}

bool BM_loop_edge_uvselect_check_other_face(BMLoop *l,
                                            const char hflag,
                                            const int cd_loop_uv_offset)
{
  BLI_assert(ELEM(hflag, BM_ELEM_SELECT, BM_ELEM_SELECT_UV));
  BMLoop *l_iter = l;
  do {
    if (!BM_elem_flag_test(l_iter->f, BM_ELEM_HIDDEN)) {
      if (l_iter != l) {
        if (BM_elem_flag_test(l_iter->f, hflag)) {
          if (BM_loop_uv_share_edge_check(l, l_iter, cd_loop_uv_offset)) {
            return true;
          }
        }
      }
    }
  } while ((l_iter = l_iter->radial_next) != l);
  return false;
}

bool BM_face_uvselect_check_edges_all(BMFace *f)
{
  if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
    return false;
  }
  BMLoop *l_iter, *l_first;
  l_iter = l_first = BM_FACE_FIRST_LOOP(f);
  do {
    if (!BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV_EDGE)) {
      return false;
    }
  } while ((l_iter = l_iter->next) != l_first);
  return true;
}

void BM_loop_vert_uvselect_set_noflush(BMesh *bm, BMLoop *l, bool select)
{
  /* Only select if it's valid, otherwise the result wont be used. */
  BLI_assert(bm->uv_sync_select_valid);
  UNUSED_VARS_NDEBUG(bm);

  /* Selecting when hidden must be prevented by the caller.
   * Allow de-selecting as this may be useful at times. */
  BLI_assert(!BM_elem_flag_test(l->f, BM_ELEM_HIDDEN) || (select == false));

  /* NOTE: don't do any flushing here as it's too expensive to walk over connected geometry.
   * These can be handled in separate operations. */
  if (select) {
    BM_elem_flag_enable(l, BM_ELEM_SELECT_UV);
  }
  else {
    BM_elem_flag_disable(l, BM_ELEM_SELECT_UV);
  }
}

void BM_loop_vert_uvselect_set_shared(BMesh *bm,
                                      BMLoop *l,
                                      bool select,
                                      const int cd_loop_uv_offset)
{
  BM_loop_vert_uvselect_set_noflush(bm, l, select);

  BMVert *v = l->v;
  BLI_assert(v->e);
  const BMEdge *e_iter, *e_first;
  e_iter = e_first = v->e;
  do {
    if (e_iter->l) {
      BMLoop *l_first = e_iter->l;
      BMLoop *l_iter = l_first;
      do {
        if (l_iter->v == v) {
          if (!BM_elem_flag_test(l_iter->f, BM_ELEM_HIDDEN)) {
            if (l_iter != l) {
              if (BM_elem_flag_test_bool(l_iter, BM_ELEM_SELECT_UV) != select) {
                if (BM_loop_uv_share_vert_check(l, l_iter, cd_loop_uv_offset)) {
                  BM_loop_vert_uvselect_set_noflush(bm, l_iter, select);
                }
              }
            }
          }
        }
      } while ((l_iter = l_iter->radial_next) != l_first);
    }
  } while ((e_iter = bmesh_disk_edge_next(e_iter, v)) != e_first);
}

void BM_loop_edge_uvselect_set_noflush(BMesh *bm, BMLoop *l, bool select)
{
  /* Only select if it's valid, otherwise the result wont be used. */
  BLI_assert(bm->uv_sync_select_valid);
  UNUSED_VARS_NDEBUG(bm);

  /* Selecting when hidden must be prevented by the caller.
   * Allow de-selecting as this may be useful at times. */
  BLI_assert(!BM_elem_flag_test(l->f, BM_ELEM_HIDDEN) || (select == false));

  /* NOTE: don't do any flushing here as it's too expensive to walk over connected geometry.
   * These can be handled in separate operations. */
  if (select) {
    BM_elem_flag_enable(l, BM_ELEM_SELECT_UV_EDGE);
  }
  else {
    BM_elem_flag_disable(l, BM_ELEM_SELECT_UV_EDGE);
  }
}

void BM_loop_edge_uvselect_set(BMesh *bm, BMLoop *l, bool select)
{
  BM_loop_edge_uvselect_set_noflush(bm, l, select);

  BM_loop_vert_uvselect_set_noflush(bm, l, select);
  BM_loop_vert_uvselect_set_noflush(bm, l->next, select);
}

void BM_loop_edge_uvselect_set_shared(BMesh *bm,
                                      BMLoop *l,
                                      bool select,
                                      const int cd_loop_uv_offset)
{
  BM_loop_edge_uvselect_set_noflush(bm, l, select);

  BMLoop *l_iter = l->radial_next;
  /* Check it's not a boundary. */
  if (l_iter != l) {
    do {
      if (BM_elem_flag_test_bool(l_iter, BM_ELEM_SELECT_UV_EDGE) != select) {
        if (BM_loop_uv_share_edge_check(l, l_iter, cd_loop_uv_offset)) {
          BM_loop_edge_uvselect_set_noflush(bm, l_iter, select);
        }
      }
    } while ((l_iter = l_iter->radial_next) != l);
  }
}

void BM_face_uvselect_set_shared(BMesh *bm, BMFace *f, bool select, const int cd_loop_uv_offset)
{
  BM_face_uvselect_set_noflush(bm, f, select);
  BMLoop *l_iter, *l_first;
  l_iter = l_first = BM_FACE_FIRST_LOOP(f);
  do {
    BM_loop_vert_uvselect_set_shared(bm, l_iter, select, cd_loop_uv_offset);
    BM_loop_edge_uvselect_set_shared(bm, l_iter, select, cd_loop_uv_offset);
  } while ((l_iter = l_iter->next) != l_first);
}

void BM_face_uvselect_set_noflush(BMesh *bm, BMFace *f, bool select)
{
  /* Only select if it's valid, otherwise the result wont be used. */
  BLI_assert(bm->uv_sync_select_valid);
  UNUSED_VARS_NDEBUG(bm);

  /* Selecting when hidden must be prevented by the caller.
   * Allow de-selecting as this may be useful at times. */
  BLI_assert(!BM_elem_flag_test(f, BM_ELEM_HIDDEN) || (select == false));

  /* NOTE: don't do any flushing here as it's too expensive to walk over connected geometry.
   * These can be handled in separate operations. */
  if (select) {
    BM_elem_flag_enable(f, BM_ELEM_SELECT_UV);
  }
  else {
    BM_elem_flag_disable(f, BM_ELEM_SELECT_UV);
  }
}

void BM_face_uvselect_set(BMesh *bm, BMFace *f, bool select)
{
  BM_face_uvselect_set_noflush(bm, f, select);
  BMLoop *l_iter, *l_first;
  l_iter = l_first = BM_FACE_FIRST_LOOP(f);
  do {
    BM_loop_vert_uvselect_set_noflush(bm, l_iter, select);
    BM_loop_edge_uvselect_set_noflush(bm, l_iter, select);
  } while ((l_iter = l_iter->next) != l_first);
}

void BM_mesh_uvselect_flush_from_loop_verts(BMesh *bm)
{
  BMIter iter;
  BMFace *f;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    bool select_all = true;
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      const bool select = (BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV) &&
                           BM_elem_flag_test(l_iter->next, BM_ELEM_SELECT_UV));
      BM_loop_edge_uvselect_set_noflush(bm, l_iter, select);
      if (select == false) {
        select_all = false;
      }
    } while ((l_iter = l_iter->next) != l_first);
    BM_face_uvselect_set_noflush(bm, f, select_all);
  }

  /* NOTE: caller may need to run #BM_mesh_uvselect_flush_shared. */
}

void BM_mesh_uvselect_flush_from_loop_verts_only_select(BMesh *bm)
{
  BMIter iter;
  BMFace *f;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    bool all_select = true;
    do {
      if (BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV) &&
          BM_elem_flag_test(l_iter->next, BM_ELEM_SELECT_UV))
      {
        BM_loop_edge_uvselect_set_noflush(bm, l_iter, true);
      }
      else {
        all_select = false;
      }
    } while ((l_iter = l_iter->next) != l_first);
    if (all_select) {
      BM_face_uvselect_set_noflush(bm, f, true);
    }
  }
}

void BM_mesh_uvselect_flush_from_loop_verts_only_deselect(BMesh *bm)
{
  BMIter iter;
  BMFace *f;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    bool all_select = true;
    do {
      if (BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV) &&
          BM_elem_flag_test(l_iter->next, BM_ELEM_SELECT_UV))
      {
        /* Pass. */
      }
      else {
        BM_loop_edge_uvselect_set_noflush(bm, l_iter, false);
        all_select = false;
      }
    } while ((l_iter = l_iter->next) != l_first);
    if (all_select == false) {
      BM_face_uvselect_set_noflush(bm, f, false);
    }
  }
}

void BM_mesh_uvselect_flush_from_loop_edges_only_select(BMesh *bm)
{
  BMIter iter;
  BMFace *f;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    bool all_select = true;
    do {
      if (BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV_EDGE)) {
        BM_loop_edge_uvselect_set(bm, l_iter, true);
      }
      else {
        all_select = false;
      }
    } while ((l_iter = l_iter->next) != l_first);
    if (all_select) {
      BM_face_uvselect_set_noflush(bm, f, true);
    }
  }
}

void BM_mesh_uvselect_flush_from_loop_edges_only_deselect(BMesh *bm)
{
  BMIter iter;
  BMFace *f;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    bool all_select = true;
    do {
      BM_loop_vert_uvselect_set_noflush(bm,
                                        l_iter,
                                        (BM_elem_flag_test(l_iter->prev, BM_ELEM_SELECT_UV_EDGE) ||
                                         BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV_EDGE)));

      if (BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV_EDGE)) {
        /* Pass. */
      }
      else {
        BM_loop_edge_uvselect_set_noflush(bm, l_iter, false);
        all_select = false;
      }
    } while ((l_iter = l_iter->next) != l_first);
    if (all_select == false) {
      BM_face_uvselect_set_noflush(bm, f, false);
    }
  }
}

void BM_mesh_uvselect_flush_from_loop_edges(BMesh *bm, bool flush_down)
{
  BMIter iter;
  BMFace *f;

  /* Clear vert/face select. */
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    if (flush_down) {
      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        BM_loop_vert_uvselect_set_noflush(bm, l_iter, false);
      } while ((l_iter = l_iter->next) != l_first);
    }
    BM_face_uvselect_set_noflush(bm, f, false);
  }

  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    bool select_all = true;
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      const bool select_edge = BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV_EDGE);
      if (select_edge) {
        if (flush_down) {
          BM_loop_vert_uvselect_set_noflush(bm, l_iter, true);
          BM_loop_vert_uvselect_set_noflush(bm, l_iter->next, true);
        }
      }
      else {
        select_all = false;
      }
    } while ((l_iter = l_iter->next) != l_first);
    if (select_all) {
      BM_face_uvselect_set_noflush(bm, f, true);
    }
  }

  /* NOTE: caller may need to run #BM_mesh_uvselect_flush_shared. */
}

void BM_mesh_uvselect_flush_from_faces(BMesh *bm, bool flush_down)
{
  if (!flush_down) {
    return; /* NOP. */
  }

  BMIter iter;
  BMFace *f;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    const bool select_face = BM_elem_flag_test(f, BM_ELEM_SELECT_UV);
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      BM_loop_vert_uvselect_set_noflush(bm, l_iter, select_face);
      BM_loop_edge_uvselect_set_noflush(bm, l_iter, select_face);
    } while ((l_iter = l_iter->next) != l_first);
  }

  /* NOTE: caller may need to run #BM_mesh_uvselect_flush_shared. */
}

void BM_mesh_uvselect_flush_from_faces_only_select(BMesh *bm)
{
  BMIter iter;
  BMFace *f;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    if (!BM_elem_flag_test(f, BM_ELEM_SELECT_UV)) {
      continue;
    }
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      BM_loop_vert_uvselect_set_noflush(bm, l_iter, true);
      BM_loop_edge_uvselect_set_noflush(bm, l_iter, true);
    } while ((l_iter = l_iter->next) != l_first);
  }

  /* NOTE: caller may need to run #BM_mesh_uvselect_flush_shared. */
}

void BM_mesh_uvselect_flush_from_faces_only_deselect(BMesh *bm)
{
  BMIter iter;
  BMFace *f;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    if (BM_elem_flag_test(f, BM_ELEM_SELECT_UV)) {
      continue;
    }
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      BM_loop_vert_uvselect_set_noflush(bm, l_iter, false);
      BM_loop_edge_uvselect_set_noflush(bm, l_iter, false);
    } while ((l_iter = l_iter->next) != l_first);
  }

  /* NOTE: caller may need to run #BM_mesh_uvselect_flush_shared. */
}

void BM_mesh_uvselect_flush_mode(BMesh *bm)
{
  if (bm->selectmode & SCE_SELECT_VERTEX) {
    BM_mesh_uvselect_flush_from_loop_verts(bm);
  }
  else if (bm->selectmode & SCE_SELECT_EDGE) {
    BM_mesh_uvselect_flush_from_loop_edges(bm, false);
  }
  else {
    BM_mesh_uvselect_flush_from_faces(bm, false);
  }
}

void BM_mesh_uvselect_flush_shared(BMesh *bm, const int cd_loop_uv_offset)
{
  /* NOTE: this could be faster if we know the vertex. */
  BLI_assert(cd_loop_uv_offset >= 0);
  BMIter iter;
  BMFace *f;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      if (!BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV)) {
        if (BM_loop_vert_uvselect_check_other_loop_vert(
                l_iter, BM_ELEM_SELECT_UV, cd_loop_uv_offset))
        {
          BM_loop_vert_uvselect_set_noflush(bm, l_iter, true);
        }
      }
      if (!BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV_EDGE)) {
        if (BM_loop_edge_uvselect_check_other_loop_edge(
                l_iter, BM_ELEM_SELECT_UV_EDGE, cd_loop_uv_offset))
        {
          BM_loop_edge_uvselect_set_noflush(bm, l_iter, true);
        }
      }
    } while ((l_iter = l_iter->next) != l_first);
  }
}

void BM_mesh_uvselect_selectmode_update(BMesh *bm,
                                        const short selectmode_old,
                                        const short selectmode_new,
                                        const int cd_loop_uv_offset)
{

  if (highest_order_bit_s(selectmode_old) >= highest_order_bit_s(selectmode_new)) {

    if ((selectmode_old & SCE_SELECT_VERTEX) == 0 && (selectmode_new & SCE_SELECT_VERTEX)) {
      /* When changing from edge/face to vertex selection,
       * new edges/faces may be selected based on the vertex selection. */
      BM_mesh_uvselect_flush_from_loop_verts(bm);
    }
    else if ((selectmode_old & SCE_SELECT_EDGE) == 0 && (selectmode_new & SCE_SELECT_EDGE)) {
      /* When changing from face to edge selection,
       * new faces may be selected based on the edge selection. */
      BM_mesh_uvselect_flush_from_loop_edges(bm, false);
    }

    /* Pass, no need to do anything when moving from edge to vertex mode (for e.g.). */
    return;
  }

  bool do_flush_deselect_down = false;
  if (selectmode_old & SCE_SELECT_VERTEX) {
    if ((selectmode_new & SCE_SELECT_VERTEX) == 0) {
      do_flush_deselect_down = true;
    }
  }
  else if (selectmode_old & SCE_SELECT_EDGE) {
    if ((selectmode_new & SCE_SELECT_EDGE) == 0) {
      do_flush_deselect_down = true;
    }
  }

  /* Rely on the selection mode switching to have de-selected isolated verts/edges,
   * simply de-select elements where the underlying mesh is not selected.
   *
   * An alternative solution would be to apply the same flushing logic here,
   * de-selecting isolated vertices when switching to edge/face select mode for e.g.
   * however this is more involved. */
  if (do_flush_deselect_down) {
    BMIter iter;
    BMFace *f;
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      bool select_face = true;
      do {
        if (BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV)) {
          if (!BM_elem_flag_test(l_iter->v, BM_ELEM_SELECT)) {
            BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV);
          }
        }
        if (BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV_EDGE)) {
          if (!BM_elem_flag_test(l_iter->e, BM_ELEM_SELECT)) {
            BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV_EDGE);
            select_face = false;
          }
        }
        else {
          select_face = false;
        }
      } while ((l_iter = l_iter->next) != l_first);

      if (select_face == false) {
        BM_elem_flag_disable(f, BM_ELEM_SELECT_UV);
      }
    }

    /* Ensure isolated elements are not selected (can happen with disconnected islands).
     * Note that it's quite unlikely UV's are unset with a UV selection,
     * check all the same as this pass is mainly a cleanup operation that isn't essential. */
    if (cd_loop_uv_offset != -1) {
      if (selectmode_new & SCE_SELECT_EDGE) {
        BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
          if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
            continue;
          }

          if (BM_elem_flag_test(f, BM_ELEM_SELECT_UV)) {
            /* When faces are selected, no need to search for isolated vertices. */
            continue;
          }
          BMLoop *l_iter, *l_first;
          l_iter = l_first = BM_FACE_FIRST_LOOP(f);
          bool e_prev_select = BM_elem_flag_test(l_iter->prev, BM_ELEM_SELECT_UV_EDGE);
          do {
            const bool e_iter_select = BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV_EDGE);
            /* Avoid unnecessary checks by first looking at the underlying mesh. */
            if (BM_elem_flag_test(l_iter->v, BM_ELEM_SELECT)) {
              if (BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV)) {
                if (!(e_prev_select || e_iter_select)) {
                  if (!BM_loop_vert_uvselect_check_other_loop_edge(
                          l_iter, BM_ELEM_SELECT_UV_EDGE, cd_loop_uv_offset))
                  {
                    BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV);
                  }
                }
              }
            }
            e_prev_select = e_iter_select;
          } while ((l_iter = l_iter->next) != l_first);
        }
      }
      else if (selectmode_new & SCE_SELECT_FACE) {
        BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
          if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
            continue;
          }

          if (BM_elem_flag_test(f, BM_ELEM_SELECT_UV)) {
            /* When faces are selected, no need to search for isolated edges. */
            continue;
          }
          BMLoop *l_iter, *l_first;
          l_iter = l_first = BM_FACE_FIRST_LOOP(f);
          do {
            /* Avoid unnecessary checks by first looking at the underlying mesh. */
            if (BM_elem_flag_test(l_iter->v, BM_ELEM_SELECT)) {
              if (BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV)) {
                if (!BM_loop_vert_uvselect_check_other_face(
                        l_iter, BM_ELEM_SELECT_UV, cd_loop_uv_offset))
                {
                  BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV);
                }
              }
            }

            /* Avoid unnecessary checks by first looking at the underlying mesh. */
            if (BM_elem_flag_test(l_iter->e, BM_ELEM_SELECT)) {
              if (BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV_EDGE)) {
                if (!BM_loop_edge_uvselect_check_other_face(
                        l_iter, BM_ELEM_SELECT_UV, cd_loop_uv_offset))
                {
                  BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV_EDGE);
                }
              }
            }
          } while ((l_iter = l_iter->next) != l_first);
        }
      }
    }
  }
}

void BM_mesh_uvselect_flush_from_v3d_sticky_location(BMesh *bm, const int cd_loop_uv_offset)
{
  if (bm->selectmode & SCE_SELECT_VERTEX) {
    BMIter iter;
    BMFace *f;

    /* UV select flags may be dirty, overwrite all. */
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        if (BM_elem_flag_test(l_iter->v, BM_ELEM_SELECT)) {
          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV);
        }
        else {
          BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV);
        }

        if (BM_elem_flag_test(l_iter->e, BM_ELEM_SELECT)) {
          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV_EDGE);
        }
        else {
          BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV_EDGE);
        }
      } while ((l_iter = l_iter->next) != l_first);

      if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
        BM_elem_flag_enable(f, BM_ELEM_SELECT_UV);
      }
      else {
        BM_elem_flag_disable(f, BM_ELEM_SELECT_UV);
      }
    }
  }
  else if (bm->selectmode & SCE_SELECT_EDGE) {
    BMIter iter;
    BMFace *f;

    /* UV select flags may be dirty, overwrite all. */
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      bool e_prev_select = BM_elem_flag_test(l_iter->prev->e, BM_ELEM_SELECT);
      do {
        const bool e_iter_select = BM_elem_flag_test(l_iter->e, BM_ELEM_SELECT);
        if (e_iter_select) {
          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV_EDGE);
        }
        else {
          BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV_EDGE);
        }

        if (BM_elem_flag_test(l_iter->v, BM_ELEM_SELECT) &&
            ((e_prev_select || e_iter_select) ||
             /* This is a more expensive check, order last. */
             BM_loop_vert_uvselect_check_other_edge(l_iter, BM_ELEM_SELECT, cd_loop_uv_offset)))
        {
          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV);
        }
        else {
          BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV);
        }
        e_prev_select = e_iter_select;
      } while ((l_iter = l_iter->next) != l_first);

      if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
        BM_elem_flag_enable(f, BM_ELEM_SELECT_UV);
      }
      else {
        BM_elem_flag_disable(f, BM_ELEM_SELECT_UV);
      }
    }
  }
  else { /* `SCE_SELECT_FACE` */
    BMIter iter;
    BMFace *f;

    /* UV select flags may be dirty, overwrite all. */
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
        BMLoop *l_iter, *l_first;
        l_iter = l_first = BM_FACE_FIRST_LOOP(f);
        do {
          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV | BM_ELEM_SELECT_UV_EDGE);
        } while ((l_iter = l_iter->next) != l_first);
        BM_elem_flag_enable(f, BM_ELEM_SELECT_UV);
      }
      else {
        BMLoop *l_iter, *l_first;
        l_iter = l_first = BM_FACE_FIRST_LOOP(f);
        do {
          if (BM_elem_flag_test(l_iter->v, BM_ELEM_SELECT) &&
              BM_loop_vert_uvselect_check_other_face(l_iter, BM_ELEM_SELECT, cd_loop_uv_offset))
          {
            BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV);
          }
          else {
            BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV);
          }

          if (BM_elem_flag_test(l_iter->e, BM_ELEM_SELECT) &&
              BM_loop_edge_uvselect_check_other_face(l_iter, BM_ELEM_SELECT, cd_loop_uv_offset))
          {
            BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV_EDGE);
          }
          else {
            BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV_EDGE);
          }
        } while ((l_iter = l_iter->next) != l_first);
        BM_elem_flag_disable(f, BM_ELEM_SELECT_UV);
      }
    }
  }

  bm->uv_sync_select_valid = true;
}

void BM_mesh_uvselect_flush_from_v3d_sticky_disabled(BMesh *bm)
{
  if (bm->selectmode & SCE_SELECT_VERTEX) {
    BMIter iter;
    BMFace *f;
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        if (BM_elem_flag_test(l_iter->v, BM_ELEM_SELECT)) {
          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV);
        }
        else {
          BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV);
        }

        if (BM_elem_flag_test(l_iter->e, BM_ELEM_SELECT)) {
          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV_EDGE);
        }
        else {
          BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV_EDGE);
        }
      } while ((l_iter = l_iter->next) != l_first);

      if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
        BM_elem_flag_enable(f, BM_ELEM_SELECT_UV);
      }
      else {
        BM_elem_flag_disable(f, BM_ELEM_SELECT_UV);
      }
    }
  }
  else if (bm->selectmode & SCE_SELECT_EDGE) {

    BMIter iter;
    BMFace *f;

    /* Clearing all makes the the following logic simpler as
     * since we only need to select UV's connected to selected edges. */
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV | BM_ELEM_SELECT_UV_EDGE);
      } while ((l_iter = l_iter->next) != l_first);
      BM_elem_flag_disable(f, BM_ELEM_SELECT_UV);
    }

    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        if (BM_elem_flag_test(l_iter->e, BM_ELEM_SELECT)) {
          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV_EDGE);

          for (BMLoop *l_edge_vert : {l_iter, l_iter->next}) {
            if (!BM_elem_flag_test(l_edge_vert, BM_ELEM_SELECT_UV)) {
              BM_elem_flag_enable(l_edge_vert, BM_ELEM_SELECT_UV);
            }
          }
        }
      } while ((l_iter = l_iter->next) != l_first);

      if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
        BM_elem_flag_enable(f, BM_ELEM_SELECT_UV);
      }
    }
  }
  else { /* `SCE_SELECT_FACE` */
    BMIter iter;
    BMFace *f;

    /* Clearing all makes the the following logic simpler as
     * since we only need to select UV's connected to selected edges. */
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV | BM_ELEM_SELECT_UV_EDGE);
      } while ((l_iter = l_iter->next) != l_first);
      BM_elem_flag_disable(f, BM_ELEM_SELECT_UV);
    }

    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
        BMLoop *l_iter, *l_first;
        l_iter = l_first = BM_FACE_FIRST_LOOP(f);
        do {
          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV);

          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV_EDGE);

        } while ((l_iter = l_iter->next) != l_first);

        BM_elem_flag_enable(f, BM_ELEM_SELECT_UV);
      }
    }
  }

  bm->uv_sync_select_valid = true;
}

void BM_mesh_uvselect_flush_from_v3d_sticky_vertex(BMesh *bm)
{
  if (bm->selectmode & SCE_SELECT_VERTEX) {
    BMIter iter;
    BMFace *f;
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        if (BM_elem_flag_test(l_iter->v, BM_ELEM_SELECT)) {
          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV);
        }
        else {
          BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV);
        }

        if (BM_elem_flag_test(l_iter->e, BM_ELEM_SELECT)) {
          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV_EDGE);
        }
        else {
          BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV_EDGE);
        }
      } while ((l_iter = l_iter->next) != l_first);

      if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
        BM_elem_flag_enable(f, BM_ELEM_SELECT_UV);
      }
      else {
        BM_elem_flag_disable(f, BM_ELEM_SELECT_UV);
      }
    }
  }
  else if (bm->selectmode & SCE_SELECT_EDGE) {

    BMIter iter;
    BMFace *f;

    /* Clearing all makes the the following logic simpler as
     * since we only need to select UV's connected to selected edges. */
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV | BM_ELEM_SELECT_UV_EDGE);
      } while ((l_iter = l_iter->next) != l_first);
      BM_elem_flag_disable(f, BM_ELEM_SELECT_UV);
    }

    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        if (BM_elem_flag_test(l_iter->e, BM_ELEM_SELECT)) {
          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV_EDGE);

          for (BMLoop *l_edge_vert : {l_iter, l_iter->next}) {
            if (!BM_elem_flag_test(l_edge_vert, BM_ELEM_SELECT_UV)) {
              BM_elem_flag_enable(l_edge_vert, BM_ELEM_SELECT_UV);
            }
          }
        }
      } while ((l_iter = l_iter->next) != l_first);

      if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
        BM_elem_flag_enable(f, BM_ELEM_SELECT_UV);
      }
    }
  }
  else { /* `SCE_SELECT_FACE` */
    BMIter iter;
    BMFace *f;

    /* Clearing all makes the the following logic simpler as
     * since we only need to select UV's connected to selected edges. */
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        BM_elem_flag_disable(l_iter, BM_ELEM_SELECT_UV | BM_ELEM_SELECT_UV_EDGE);
      } while ((l_iter = l_iter->next) != l_first);
      BM_elem_flag_disable(f, BM_ELEM_SELECT_UV);
    }

    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
        BMLoop *l_iter, *l_first;
        l_iter = l_first = BM_FACE_FIRST_LOOP(f);
        do {
          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV);

          BM_elem_flag_enable(l_iter, BM_ELEM_SELECT_UV_EDGE);

        } while ((l_iter = l_iter->next) != l_first);

        BM_elem_flag_enable(f, BM_ELEM_SELECT_UV);
      }
    }
  }

  bm->uv_sync_select_valid = true;
}

void BM_mesh_uvselect_flush_to_v3d(BMesh *bm)
{
  BLI_assert(bm->uv_sync_select_valid);

  /* Prevent clearing the selection from removing all selection history.
   * This will be validated after flushing. */
  BM_SELECT_HISTORY_BACKUP(bm);

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);

  if (bm->selectmode & SCE_SELECT_VERTEX) {
    /* Simple, no need to worry about edge selection. */

    /* Copy loop-vert to vert, then flush. */
    BMIter iter;
    BMFace *f;
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        if (BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV)) {
          BM_vert_select_set(bm, l_iter->v, true);
        }
      } while ((l_iter = l_iter->next) != l_first);
    }

    BM_mesh_select_flush(bm);
  }
  else if (bm->selectmode & SCE_SELECT_EDGE) {
    BMIter iter;
    BMFace *f;
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      /* Technically this should only need to check the edge
       * because when a vertex isn't selected, it's connected edges shouldn't be.
       * Check both in the unlikely case of an invalid selection. */
      bool face_select = true;

      do {
        /* This requires the edges to have already been flushed to the vertices (assert next). */
        if (BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV)) {
          BM_vert_select_set(bm, l_iter->v, true);
        }
        else {
          face_select = false;
        }

        if (BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV_EDGE)) {
          /* If this fails, we've missed flushing. */
          BLI_assert(BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV) &&
                     BM_elem_flag_test(l_iter->next, BM_ELEM_SELECT_UV));
          BM_edge_select_set(bm, l_iter->e, true);
        }
        else {
          face_select = false;
        }
      } while ((l_iter = l_iter->next) != l_first);
      if (face_select) {
        BM_face_select_set_noflush(bm, f, true);
      }
    }

    /* It's possible that a face which is *not* UV-selected
     * ends up with all it's edges selected.
     * Perform the edge to face flush inline. */
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      /* If the face is hidden, we can't selected,
       * If the face is already selected, it can be skipped here. */
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN | BM_ELEM_SELECT)) {
        continue;
      }
      bool face_select = true;
      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        if (!BM_elem_flag_test(l_iter->e, BM_ELEM_SELECT)) {
          face_select = false;
          break;
        }
      } while ((l_iter = l_iter->next) != l_first);

      if (face_select) {
        BM_face_select_set_noflush(bm, f, true);
      }
    }
  }
  else { /* `bm->selectmode & SCE_SELECT_FACE` */
    BMIter iter;
    BMFace *f;
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        continue;
      }

      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      bool face_select = true;
      do {
        if (!BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV) ||
            !BM_elem_flag_test(l_iter, BM_ELEM_SELECT_UV_EDGE))
        {
          face_select = false;
          break;
        }
      } while ((l_iter = l_iter->next) != l_first);
      if (face_select) {
        BM_face_select_set(bm, f, true);
      }
    }
  }

  BM_SELECT_HISTORY_RESTORE(bm);

  BM_select_history_validate(bm);
}

bool BM_mesh_uvselect_clear(BMesh *bm)
{
  if (bm->uv_sync_select_valid == false) {
    return false;
  }
  bm->uv_sync_select_valid = false;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Picking Versions of Selection Functions
 *
 * These functions differ in that they perform all necessary flushing but do so only on
 * local elements. This is only practical with a small number of elements since it'd
 * be inefficient on large selections.
 *
 * Note that we *could* also support selecting face-corners from the UV viewport
 * using these functions, however that's not yet supported.
 * \{ */

void BM_vert_uvselect_set_pick(BMesh *bm,
                               BMVert *v,
                               const bool select,
                               const BMUVSelectPickParams & /*uv_pick_params*/)
{
  if (BM_elem_flag_test(v, BM_ELEM_HIDDEN)) {
    return;
  }

  /* Must be connected to edges. */
  if (v->e == nullptr) {
    return;
  }

  if (select) {
    const BMEdge *e_iter, *e_first;
    e_iter = e_first = v->e;
    do {
      if (e_iter->l) {
        BMLoop *l_radial_iter, *l_radial_first;
        l_radial_iter = l_radial_first = e_iter->l;
        do {
          if (v == l_radial_iter->v) {
            /* Select vertex. */
            BM_loop_vert_uvselect_set_noflush(bm, l_radial_iter, true);

            /* Select edges if adjacent vertices are selected. */
            if (BM_elem_flag_test(l_radial_iter->next, BM_ELEM_SELECT_UV)) {
              BM_loop_edge_uvselect_set_noflush(bm, l_radial_iter, true);
            }
            if (BM_elem_flag_test(l_radial_iter->prev, BM_ELEM_SELECT_UV)) {
              BM_loop_edge_uvselect_set_noflush(bm, l_radial_iter->prev, true);
            }
            /* Select face if all edges are selected. */
            if (!BM_elem_flag_test(l_radial_iter->f, BM_ELEM_HIDDEN) &&
                !BM_elem_flag_test(l_radial_iter->f, BM_ELEM_SELECT_UV))
            {
              if (BM_face_uvselect_check_edges_all(l_radial_iter->f)) {
                BM_face_uvselect_set_noflush(bm, l_radial_iter->f, true);
              }
            }
          }
        } while ((l_radial_iter = l_radial_iter->radial_next) != l_radial_first);
      }
    } while ((e_iter = bmesh_disk_edge_next(e_iter, v)) != e_first);
  }
  else {
    const BMEdge *e_iter, *e_first;
    e_iter = e_first = v->e;
    do {
      if (e_iter->l) {
        BMLoop *l_radial_iter, *l_radial_first;
        l_radial_iter = l_radial_first = e_iter->l;
        do {
          if (v == l_radial_iter->v) {
            /* Deselect vertex. */
            BM_loop_vert_uvselect_set_noflush(bm, l_radial_iter, false);
            /* Deselect edges. */
            BM_loop_edge_uvselect_set_noflush(bm, l_radial_iter, false);
            BM_loop_edge_uvselect_set_noflush(bm, l_radial_iter->prev, false);
            /* Deselect connected face. */
            BM_face_uvselect_set_noflush(bm, l_radial_iter->f, false);
          }
        } while ((l_radial_iter = l_radial_iter->radial_next) != l_radial_first);
      }
    } while ((e_iter = bmesh_disk_edge_next(e_iter, v)) != e_first);
  }
}

void BM_edge_uvselect_set_pick(BMesh *bm,
                               BMEdge *e,
                               const bool select,
                               const BMUVSelectPickParams &uv_pick_params)
{
  if (BM_elem_flag_test(e, BM_ELEM_HIDDEN)) {
    return;
  }

  /* Must be connected to faces. */
  if (e->l == nullptr) {
    return;
  }

  if (uv_pick_params.shared == false) {
    BMLoop *l_iter, *l_first;

    if (select) {
      bool any_faces_unselected = false;
      l_iter = l_first = e->l;
      do {
        BM_loop_edge_uvselect_set_noflush(bm, l_iter, true);

        BM_loop_vert_uvselect_set_noflush(bm, l_iter, true);
        BM_loop_vert_uvselect_set_noflush(bm, l_iter->next, true);

        if (any_faces_unselected == false) {
          if (!BM_elem_flag_test(l_iter->f, BM_ELEM_SELECT_UV)) {
            any_faces_unselected = true;
          }
        }
      } while ((l_iter = l_iter->radial_next) != l_first);

      /* Flush selection to faces when all edges in connected faces are now selected. */
      if (any_faces_unselected) {
        l_iter = l_first = e->l;
        do {
          if (!BM_elem_flag_test(l_iter->f, BM_ELEM_SELECT_UV)) {
            if (BM_face_uvselect_check_edges_all(l_iter->f)) {
              BM_face_uvselect_set_noflush(bm, l_iter->f, true);
            }
          }
        } while ((l_iter = l_iter->radial_next) != l_first);
      }
    }
    else {
      l_iter = l_first = e->l;
      do {
        BM_loop_edge_uvselect_set_noflush(bm, l_iter, false);
        if (!BM_elem_flag_test(l_iter->prev, BM_ELEM_SELECT_UV_EDGE)) {
          BM_loop_vert_uvselect_set_noflush(bm, l_iter, false);
        }
        if (!BM_elem_flag_test(l_iter->next, BM_ELEM_SELECT_UV_EDGE)) {
          BM_loop_vert_uvselect_set_noflush(bm, l_iter->next, false);
        }
        BM_face_uvselect_set_noflush(bm, l_iter->f, false);
      } while ((l_iter = l_iter->radial_next) != l_first);
    }
    return;
  }

  /* NOTE(@ideasman42): this is awkward as the edge may reference multiple island bounds.
   * - De-selecting will de-select all which makes sense.
   * - Selecting will also select all which is not likely to be all that useful for users.
   *
   * We could attempt to use the surrounding to *guess* which UV island selection to extend
   * but this seems error prone as it only works in some situations.
   * Users will most likely prefer face selection in these situations. */

  BMLoop *l_iter, *l_first;

  if (select) {
    bool any_faces_unselected = false;
    l_iter = l_first = e->l;
    do {
      BM_loop_edge_uvselect_set_noflush(bm, l_iter, true);

      BM_loop_vert_uvselect_set_noflush(bm, l_iter, true);
      BM_loop_vert_uvselect_set_noflush(bm, l_iter->next, true);

      if (any_faces_unselected == false) {
        if (!BM_elem_flag_test(l_iter->f, BM_ELEM_HIDDEN)) {
          if (!BM_elem_flag_test(l_iter->f, BM_ELEM_SELECT_UV)) {
            any_faces_unselected = true;
          }
        }
      }
    } while ((l_iter = l_iter->radial_next) != l_first);

    /* Flush selection to faces when all edges in connected faces are now selected. */
    if (any_faces_unselected) {
      l_iter = l_first = e->l;
      do {
        if (!BM_elem_flag_test(l_iter->f, BM_ELEM_HIDDEN)) {
          if (!BM_elem_flag_test(l_iter->f, BM_ELEM_SELECT_UV)) {
            if (BM_face_uvselect_check_edges_all(l_iter->f)) {
              BM_face_uvselect_set_noflush(bm, l_iter->f, true);
            }
          }
        }
      } while ((l_iter = l_iter->radial_next) != l_first);
    }
  }
  else {
    l_iter = l_first = e->l;
    do {
      BM_loop_edge_uvselect_set_noflush(bm, l_iter, false);
      if (!BM_elem_flag_test(l_iter->prev, BM_ELEM_SELECT_UV_EDGE)) {
        BM_loop_vert_uvselect_set_noflush(bm, l_iter, false);
      }
      if (!BM_elem_flag_test(l_iter->next, BM_ELEM_SELECT_UV_EDGE)) {
        BM_loop_vert_uvselect_set_noflush(bm, l_iter->next, false);
      }
      BM_face_uvselect_set_noflush(bm, l_iter->f, false);
    } while ((l_iter = l_iter->radial_next) != l_first);

    /* Ensure connected vertices remain selected when they are connected to selected edges. */
    l_iter = l_first = e->l;
    do {
      for (BMLoop *l_edge_vert : {l_iter, l_iter->next}) {
        if (BM_elem_flag_test(l_edge_vert, BM_ELEM_SELECT_UV)) {
          /* This was not de-selected. */
          continue;
        }
        if (BM_loop_vert_uvselect_check_other_loop_edge(
                l_edge_vert, BM_ELEM_SELECT_UV_EDGE, uv_pick_params.cd_loop_uv_offset))
        {
          BM_loop_vert_uvselect_set_noflush(bm, l_edge_vert, true);
        }
        else {
          /* It's possible there are isolated selected vertices,
           * although in edge select mode this should not happen. */
          BM_loop_vert_uvselect_set_shared(
              bm, l_edge_vert, false, uv_pick_params.cd_loop_uv_offset);
        }
      }
    } while ((l_iter = l_iter->radial_next) != l_first);
  }
}

void BM_face_uvselect_set_pick(BMesh *bm,
                               BMFace *f,
                               const bool select,
                               const BMUVSelectPickParams &uv_pick_params)
{
  if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
    return;
  }

  BMLoop *l_iter, *l_first;

  if (uv_pick_params.shared == false) {
    BM_face_uvselect_set(bm, f, select);
    return;
  }

  if (select) {
    BM_face_uvselect_set_noflush(bm, f, true);

    /* Setting these values first. */
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      BM_loop_vert_uvselect_set_noflush(bm, l_iter, true);
      BM_loop_edge_uvselect_set_noflush(bm, l_iter, true);
    } while ((l_iter = l_iter->next) != l_first);

    /* Set other values. */
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      BM_loop_vert_uvselect_set_shared(bm, l_iter, true, uv_pick_params.cd_loop_uv_offset);
      BM_loop_edge_uvselect_set_shared(bm, l_iter, true, uv_pick_params.cd_loop_uv_offset);
    } while ((l_iter = l_iter->next) != l_first);
  }
  else {
    BM_face_uvselect_set_noflush(bm, f, false);

    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      BM_loop_vert_uvselect_set_noflush(bm, l_iter, false);
      BM_loop_edge_uvselect_set_noflush(bm, l_iter, false);
      /* Vertex. */
      if (BM_loop_vert_uvselect_check_other_face(
              l_iter, BM_ELEM_SELECT_UV, uv_pick_params.cd_loop_uv_offset))
      {
        BM_loop_vert_uvselect_set_noflush(bm, l_iter, true);
      }
      else {
        BM_loop_vert_uvselect_set_shared(bm, l_iter, false, uv_pick_params.cd_loop_uv_offset);
      }
      /* Edge. */
      if (BM_loop_edge_uvselect_check_other_face(
              l_iter, BM_ELEM_SELECT_UV, uv_pick_params.cd_loop_uv_offset))
      {
        BM_loop_edge_uvselect_set_noflush(bm, l_iter, true);
      }
      else {
        BM_loop_edge_uvselect_set_shared(bm, l_iter, false, uv_pick_params.cd_loop_uv_offset);
      }
    } while ((l_iter = l_iter->next) != l_first);
  }
}

/** \} */
