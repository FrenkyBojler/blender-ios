/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pybmesh
 *
 * This file defines the 'bmesh.query' module.
 * Utility functions for filtering 'bmesh.types'
 */

#include "bmesh.hh"

#include <Python.h>

#include "MEM_guardedalloc.h"

#include "../generic/python_compat.hh" /* IWYU pragma: keep. */

#include "../generic/py_capi_utils.hh"
#include "../generic/python_utildefines.hh"

#include "bmesh_py_types.hh" /* own include */
#include "bmesh_py_types_customdata.hh"

struct QueryFilter {
  int include_elem = 0;
  int exclude_elem = 0;
  int include_topo = 0;
  int exclude_topo = 0;

  bool match_all_include_elem_flags = false;
  bool match_all_exclude_elem_flags = false;
};

/* TODO: Replace in c++20 with std::bitset<N>::count */

static int count_set_bits(int n)
{
  uint u = static_cast<uint>(n);
  int count = 0;
  while (u) {
    count += u & 1u;
    u >>= 1;
  }
  return count;
}

/* HFLAG flags
 * ******************* */

enum {
  /* Continue BM_ELEM */
  FILTER_LOOP_SELECT_VERT = 1 << 8,
  FILTER_LOOP_SELECT_EDGE = 1 << 9,
  FILTER_LOOP_SELECT_FACE = 1 << 10,
  FILTER_LOOP_SELECT_VERT_UV = 1 << 11,
  FILTER_LOOP_SELECT_EDGE_UV = 1 << 12,
  FILTER_LOOP_SELECT_FACE_UV = 1 << 13,
  FILTER_LOOP_SMOOTH_EDGE = 1 << 14,
  FILTER_LOOP_SMOOTH_FACE = 1 << 15,
};

PyC_FlagSet bpy_bm_filter_verts_flags[] = {
    {BM_ELEM_SELECT, "SELECT"},
    {BM_ELEM_HIDDEN, "HIDDEN"},
    {BM_ELEM_TAG, "TAG"},
    {0, nullptr},
};

PyC_FlagSet bpy_bm_filter_edges_flags[] = {
    {BM_ELEM_SELECT, "SELECT"},
    {BM_ELEM_HIDDEN, "HIDDEN"},
    {BM_ELEM_SEAM, "SEAM"},
    {BM_ELEM_SMOOTH, "SMOOTH"},
    {BM_ELEM_TAG, "TAG"},
    {0, nullptr},
};

PyC_FlagSet bpy_bm_filter_faces_elem_flags[] = {
    {BM_ELEM_SELECT, "SELECT"},
    {BM_ELEM_HIDDEN, "HIDDEN"},
    {BM_ELEM_SMOOTH, "SMOOTH"},
    {BM_ELEM_TAG, "TAG"},
    {0, nullptr},
};
#define BPY_BM_HFLAG_FILTER_FLAGS_VERTS_STR "('SELECT', 'HIDDEN', 'TAG')"
#define BPY_BM_HFLAG_FILTER_FLAGS_EDGES_STR "('SELECT', 'HIDDEN', 'SEAM', 'SMOOTH', 'TAG')"
#define BPY_BM_HFLAG_FILTER_FLAGS_FACES_STR "('SELECT', 'HIDDEN', 'SMOOTH', 'TAG')"
#define BPY_BM_HFLAG_FILTER_FLAGS_LOOPS_STR \
  "('HIDDEN', 'TAG', 'SELECT_VERT', 'SELECT_EDGE', 'SELECT_FACE', 'SMOOTH_EDGE', " \
  "'SMOOTH_FACE')"

/* Topology flags
 * ******************* */

enum {
  FILTER_TOPO_MANIFOLD = 1 << 0,
  FILTER_TOPO_MANIFOLD_UV = 1 << 1,
  FILTER_TOPO_CONTIGUOUS = 1 << 2,
  FILTER_TOPO_CONTIGUOUS_UV = 1 << 3,
  FILTER_TOPO_WIRE = 1 << 4,
  FILTER_TOPO_LOOSE = 1 << 5,
  FILTER_TOPO_BOUNDARY = 1 << 6,
  FILTER_TOPO_BOUNDARY_UV = 1 << 7,
  FILTER_TOPO_BOUNDARY_BY_FACE_SELECT = 1 << 8,
  FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN = 1 << 9,
  FILTER_TOPO_BOUNDARY_BY_FACE_SELECT_UV = 1 << 10,
  FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN_UV = 1 << 11,
  /* For BM_FACE */
  FILTER_TOPO_TRIS = 1 << 11,
  FILTER_TOPO_QUAD = 1 << 12,
  FILTER_TOPO_NGONE = 1 << 13,

};

PyC_FlagSet bpy_bm_filter_topo_verts_flags[] = {
    {FILTER_TOPO_MANIFOLD, "MANIFOLD"},
    {FILTER_TOPO_WIRE, "WIRE"},
    {FILTER_TOPO_LOOSE, "LOOSE"},
    {FILTER_TOPO_BOUNDARY, "BOUNDARY"},
    {FILTER_TOPO_BOUNDARY_BY_FACE_SELECT, "BOUNDARY_BY_FACE_SELECT"},
    {FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN, "BOUNDARY_BY_FACE_HIDDEN"},
    {0, nullptr},

};
PyC_FlagSet bpy_bm_filter_topo_edges_flags[] = {
    {FILTER_TOPO_MANIFOLD, "MANIFOLD"},
    {FILTER_TOPO_CONTIGUOUS, "CONTIGUOUS"},
    {FILTER_TOPO_WIRE, "WIRE"},
    {FILTER_TOPO_BOUNDARY, "BOUNDARY"},
    {FILTER_TOPO_BOUNDARY_BY_FACE_SELECT, "BOUNDARY_BY_FACE_SELECT"},
    {FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN, "BOUNDARY_BY_FACE_HIDDEN"},
    {0, nullptr},
};

PyC_FlagSet bpy_bm_filter_topo_faces_flags[] = {
    {FILTER_TOPO_TRIS, "TRIS"},
    {FILTER_TOPO_QUAD, "QUAD"},
    {FILTER_TOPO_NGONE, "NGONE"},
    {0, nullptr},
};

PyC_FlagSet bpy_bm_filter_topo_loops_flags[] = {
    {FILTER_TOPO_MANIFOLD, "MANIFOLD"},
    {FILTER_TOPO_BOUNDARY, "CONTIGUOUS"},
    {FILTER_TOPO_WIRE, "WIRE"},
    {FILTER_TOPO_BOUNDARY, "BOUNDARY"},
    {FILTER_TOPO_BOUNDARY_BY_FACE_SELECT, "BOUNDARY_BY_FACE_SELECT"},
    {FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN, "BOUNDARY_BY_FACE_HIDDEN"},
    {0, nullptr},
};

#define BPY_BM_HFLAG_FILTER_TOPOLOGY_VERTS_STR \
  "('MANIFOLD', 'WIRE', 'LOOSE', 'BOUNDARY', 'BOUNDARY_BY_FACE_SELECT', " \
  "'BOUNDARY_BY_FACE_HIDDEN')"
#define BPY_BM_HFLAG_FILTER_TOPOLOGY_EDGES_STR \
  "('MANIFOLD', 'CONTIGUOUS', 'WIRE', 'BOUNDARY', 'BOUNDARY_BY_FACE_SELECT', " \
  "'BOUNDARY_BY_FACE_HIDDEN')"
#define BPY_BM_HFLAG_FILTER_TOPOLOGY_FACES_STR "('TRIS', 'QUAD', 'NGONE')"
#define BPY_BM_HFLAG_FILTER_TOPOLOGY_LOOPS_STR \
  "('MANIFOLD', 'CONTIGUOUS', 'BOUNDARY', 'BOUNDARY_BY_FACE_SELECT', 'BOUNDARY_BY_FACE_HIDDEN')"

/* Arg Parser */

static int parse_and_check_vertex_filter_args(PyObject *args,
                                              PyObject *kw,
                                              PyObject **r_py_bm_seq,
                                              QueryFilter *filter,
                                              const PyC_FlagSet *elem_flags,
                                              const PyC_FlagSet *topo_flags)
{
  static const char *kwlist[] = {"verts",
                                 "include_flags",
                                 "exclude_flags",
                                 "include_topology",
                                 "exclude_topology",
                                 "match_all_include_elem_flags",
                                 "match_all_exclude_elem_flags",
                                 nullptr};

  PyObject *py_include_flags = nullptr;
  PyObject *py_exclude_flags = nullptr;
  PyObject *py_include_topo = nullptr;
  PyObject *py_exclude_topo = nullptr;

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kw,
                                   "O|$O!O!O!O!O&O&:filter_verts",
                                   (char **)kwlist,
                                   r_py_bm_seq,
                                   &PySet_Type,
                                   &py_include_flags,
                                   &PySet_Type,
                                   &py_exclude_flags,
                                   &PySet_Type,
                                   &py_include_topo,
                                   &PySet_Type,
                                   &py_exclude_topo,
                                   PyC_ParseBool,
                                   &filter->match_all_include_elem_flags,
                                   PyC_ParseBool,
                                   &filter->match_all_exclude_elem_flags))
  {
    return -1;
  }

  if (py_include_flags &&
      PyC_FlagSet_ToBitfield(
          elem_flags, py_include_flags, &filter->include_elem, "include_flags") == -1)
  {
    return -1;
  }

  if (py_exclude_flags &&
      PyC_FlagSet_ToBitfield(
          elem_flags, py_exclude_flags, &filter->exclude_elem, "exclude_flags") == -1)
  {
    return -1;
  }

  if (py_include_topo &&
      PyC_FlagSet_ToBitfield(
          topo_flags, py_include_topo, &filter->include_topo, "include_topology") == -1)
  {
    return -1;
  }

  if (py_exclude_topo &&
      PyC_FlagSet_ToBitfield(
          topo_flags, py_exclude_topo, &filter->exclude_topo, "exclude_topology") == -1)
  {
    return -1;
  }

  /* Reduce selection flags */
  if (filter->include_topo & FILTER_TOPO_BOUNDARY_BY_FACE_SELECT) {
    filter->include_topo &= ~BM_ELEM_SELECT;
  }
  if (filter->include_topo & FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN) {
    filter->include_elem &= ~BM_ELEM_HIDDEN;
  }
  if (filter->exclude_topo & FILTER_TOPO_BOUNDARY_BY_FACE_SELECT) {
    filter->exclude_elem &= ~BM_ELEM_SELECT;
  }
  if (filter->exclude_topo & FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN) {
    filter->exclude_elem &= ~BM_ELEM_HIDDEN;
  }
  /* Reduce hidden flags */
  if (filter->include_topo &
      (FILTER_TOPO_BOUNDARY_BY_FACE_SELECT | FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN))
  {
    filter->include_topo &= ~FILTER_TOPO_BOUNDARY;
  }
  if (filter->exclude_topo &
      (FILTER_TOPO_BOUNDARY_BY_FACE_SELECT | FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN))
  {
    filter->exclude_topo &= ~FILTER_TOPO_BOUNDARY;
  }

  /* Check compatible flags */
  static const int select_and_hidden_flags = (BM_ELEM_SELECT | BM_ELEM_HIDDEN);
  static const int select_and_hidden_topo_flags = (FILTER_TOPO_BOUNDARY_BY_FACE_SELECT |
                                                   FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN);
  /* Check selection collision */
  if (filter->match_all_include_elem_flags) {
    if (count_set_bits(filter->include_elem & select_and_hidden_flags) +
            count_set_bits(filter->include_topo & select_and_hidden_topo_flags) >=
        2)
    {
      PyErr_SetString(
          PyExc_ValueError,
          "include_flags contain SELECT and HIDDEN which are incompatible with match all");
      return -1;
    }
  }
  if (filter->match_all_exclude_elem_flags) {
    if (count_set_bits((filter->exclude_elem & select_and_hidden_flags) +
                       count_set_bits((filter->exclude_topo & select_and_hidden_topo_flags))) >= 2)
    {
      PyErr_SetString(
          PyExc_ValueError,
          "exclude_flags contain SELECT and HIDDEN which are incompatible with match all");
      return -1;
    }
  }

  /* Check intersection flags*/
  if (filter->include_elem & filter->exclude_elem) {
    PyErr_SetString(PyExc_ValueError,
                    "include_flags and exclude_flags contain intersecting flags");
    return -1;
  }

  if (filter->include_topo & filter->exclude_topo) {
    PyErr_SetString(PyExc_ValueError,
                    "include_topology and exclude_topology contain intersecting flags");
    return -1;
  }

  static const int all_face_len_flags = FILTER_TOPO_TRIS | FILTER_TOPO_QUAD | FILTER_TOPO_NGONE;
  if ((filter->exclude_topo & all_face_len_flags) == all_face_len_flags) {
    PyErr_SetString(
        PyExc_ValueError,
        "exclude_flags contains TRIS, QUAD, and NGON at the same time, which is not allowed");
    return -1;
  }
  /* Disable face len flags */
  if ((filter->include_topo & all_face_len_flags) == all_face_len_flags) {
    filter->include_topo &= ~all_face_len_flags;
  }

  return 0;
}

inline static bool is_elem_match_filter_flags(BMHeader *ele,
                                              const int flags,
                                              const bool match_all_flags)
{
  const char bl_elem_flags = static_cast<char>(flags);
  if (match_all_flags) {
    return _bm_elem_flag_test(ele, flags) == bl_elem_flags;
  }
  else {
    return _bm_elem_flag_test(ele, flags);
  }
}

inline static bool is_vert_match_filter_topology_flags(BMVert *v, const int topology_flags)
{
  if (topology_flags & FILTER_TOPO_MANIFOLD) {
    if (BM_vert_is_manifold(v)) {
      return true;
    }
  }

  if (topology_flags & FILTER_TOPO_WIRE) {
    if (BM_vert_is_wire(v)) {
      return true;
    }
  }

  if (topology_flags & FILTER_TOPO_LOOSE) {
    if (!v->e) {
      return true;
    }
  }

  if (topology_flags & (FILTER_TOPO_BOUNDARY_BY_FACE_SELECT | FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN |
                        FILTER_TOPO_BOUNDARY))
  {
    if (!v->e) {
      return false;
    }
  }

  if (topology_flags & FILTER_TOPO_BOUNDARY) {
    if (BM_vert_is_boundary(v)) {
      return true;
    }
  }

  if (topology_flags & (FILTER_TOPO_BOUNDARY_BY_FACE_SELECT | FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN))
  {
    char test_flag;

    if (topology_flags & FILTER_TOPO_BOUNDARY_BY_FACE_SELECT) {
      test_flag = BM_ELEM_SELECT;
      if (!BM_elem_flag_test(v, BM_ELEM_SELECT)) {
        return false;
      }
    }
    else {
      test_flag = BM_ELEM_HIDDEN;
      if (BM_elem_flag_test(v, BM_ELEM_HIDDEN)) {
        return false;
      }
    }

    /* Count target flags, and if this number does not match the number of vertices, then it is a
     * boundary vertex */
    BMFace *f_other;
    BMIter face_iter;
    bool has_target_flag = false;
    bool has_non_target_flag = false;
    BM_ITER_ELEM (f_other, &face_iter, v, BM_FACES_OF_VERT) {
      if (BM_elem_flag_test(f_other, test_flag)) {
        has_target_flag = true;
      }
      else {
        has_non_target_flag = true;
      }
      if (has_target_flag && has_non_target_flag) {
        return true;
      }
    }
  }

  return false;
}

inline bool is_vert_match_filter(BMVert *v, QueryFilter *filter)
{
  if (filter->include_elem) {
    if (!is_elem_match_filter_flags(
            (BMHeader *)v, filter->include_elem, filter->match_all_include_elem_flags))
    {
      return false;
    }
  }
  if (filter->exclude_elem) {
    if (is_elem_match_filter_flags(
            (BMHeader *)v, filter->exclude_elem, filter->match_all_exclude_elem_flags))
    {
      return false;
    }
  }
  if (filter->include_topo) {
    if (!is_vert_match_filter_topology_flags(v, filter->include_topo)) {
      return false;
    }
  }
  if (filter->exclude_topo) {
    if ((is_vert_match_filter_topology_flags(v, filter->exclude_topo))) {
      return false;
    }
  }
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Module Doc String
 * \{ */

PyDoc_STRVAR(
    /* Wrap. */
    QUERY_filter_doc,
    "Quick functions for filtering Faces, Edges, Vertices, and Loops by flags and element "
    "topology.");

/** \} */

/* -------------------------------------------------------------------- */
/** \name Python Functions
 * \{ */

PyDoc_STRVAR(
    /* Wrap. */
    bpy_bm_query_filter_verts_doc,
    ".. method:: filter_verts(verts, *, include_flags={}, exclude_flags={}, "
    "include_topology={}, exclude_topology={}, match_all_include_elem_flags=False, "
    "match_all_exclude_elem_flags=False)\n"
    "\n"

    "   Filter vertices by flags and topology.\n"
    "\n"
    "   :arg verts: The vertices to filter.\n"
    "   :type face: :class:`bmesh.types.BMVertSeq` or Sequence[:class:`bmesh.types.BMVert`]\n"

    "   :return: Filtered vertices.\n"
    "   :rtype: list[:class:`bmesh.types.BMVert`]\n");
static PyObject *bpy_bm_query_filter_verts(PyObject * /*self*/, PyObject *args, PyObject *kw)
{
  PyObject *list;
  PyObject *py_vert;
  PyObject *py_vert_seq;

  QueryFilter filter; /* optional */

  if (UNLIKELY(parse_and_check_vertex_filter_args(args,
                                                  kw,
                                                  &py_vert_seq,
                                                  &filter,
                                                  bpy_bm_filter_verts_flags,
                                                  bpy_bm_filter_topo_verts_flags) == -1))
  {
    return nullptr;
  }

  if BPy_BMVertSeq_Check (py_vert_seq) {
    BPY_BM_CHECK_OBJ(py_vert_seq);
    BMesh *bm = ((BPy_BMGeneric *)(py_vert_seq))->bm;

    /* Checking selection for a quick O(1) return. */
    if (bm_is_full_vert_selected(bm)) {
      if (filter.match_all_include_elem_flags) {
        if (filter.include_elem & BM_ELEM_HIDDEN) {
          return PyList_New(0);
        }
      }
      else {
        if (filter.include_elem == BM_ELEM_HIDDEN) {
          return PyList_New(0);
        }
        if (filter.include_elem == BM_ELEM_SELECT) {
          filter.include_elem = 0;
        }
      }
    }

    if (bm_is_full_vert_deselected(bm)) {
      if (filter.match_all_exclude_elem_flags) {
        if (filter.exclude_elem & BM_ELEM_SELECT) {
          return PyList_New(0);
        }
      }
      else {
        if (filter.exclude_elem == BM_ELEM_SELECT) {
          return PyList_New(0);
        }
      }
    }

    if (bm_is_full_face_selected(bm)) {
      if (filter.include_topo == FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN) {
        return PyList_New(0);
      }
      /* Avoid BM_FACES_OF_VERT iteration if all faces are selected.*/
      if (filter.include_topo & FILTER_TOPO_BOUNDARY_BY_FACE_SELECT) {
        filter.include_topo &= ~FILTER_TOPO_BOUNDARY_BY_FACE_SELECT;
        filter.include_topo |= FILTER_TOPO_BOUNDARY;
      }
    }

    if (bm_is_full_face_deselected(bm)) {
      if (filter.include_topo == FILTER_TOPO_BOUNDARY_BY_FACE_SELECT) {
        return PyList_New(0);
      }
    }

    BMVert **vertices = static_cast<BMVert **>(
        MEM_mallocN(sizeof(*vertices) * bm->totvert, __func__));
    if (vertices == nullptr) {
      PyErr_SetString(PyExc_MemoryError, "failed to create a sequence with filtered vertices");
      return nullptr;
    }

    /* BM_mesh_elem_table_ensure(bm, BM_VERT); */

    BMIter iter;
    BMVert *v;
    int verts_len = 0;
    BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
      if (is_vert_match_filter(v, &filter)) {
        vertices[verts_len++] = v;
      }
    }
    list = PyList_New(verts_len);
    if (list == nullptr) {
      MEM_freeN(vertices);
      return nullptr;
    }

    for (int i = 0; i < verts_len; i++) {
      py_vert = BPy_BMVert_CreatePyObject(bm, vertices[i]);
      PyList_SetItem(list, i, py_vert);
    }

    MEM_freeN(vertices);
    return list;
  }
  else {
    PyObject *it;
    PyObject *(*iternext)(PyObject *);

    it = PyObject_GetIter(py_vert_seq);
    if (it == nullptr)
      return nullptr;

    list = PyList_New(0);
    iternext = *Py_TYPE(it)->tp_iternext;

    for (;;) {
      py_vert = iternext(it);
      if (py_vert == nullptr)
        break;

      if (!BPy_BMVert_Check(py_vert)) {
        PyErr_Format(PyExc_TypeError,
                     "bmesh.query.filter_verts: BMVert expected, not '%.200s'",
                     Py_TYPE(py_vert)->tp_name);
        Py_DECREF(it);
        Py_DECREF(list);
        return nullptr;
      }
      if (bpy_bm_generic_valid_check((BPy_BMGeneric *)py_vert) == -1) {
        Py_DECREF(it);
        Py_DECREF(list);
        return nullptr;
      }
      if (is_vert_match_filter(((BPy_BMVert *)py_vert)->v, &filter)) {
        PyList_APPEND(list, py_vert);
      }
    }

    Py_DECREF(it);
    if (PyErr_Occurred()) {
      if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
        PyErr_Clear();
      }
      else {
        Py_DECREF(list);
        return nullptr;
      }
    }
    return list;
  }
}

inline static bool is_edge_match_filter_topology_flags(BMEdge *e, const int topology_flags)
{
  if (topology_flags & FILTER_TOPO_MANIFOLD) {
    if (BM_edge_is_manifold(e)) {
      return true;
    }
  }

  if (topology_flags & FILTER_TOPO_CONTIGUOUS) {
    if (BM_edge_is_contiguous(e)) {
      return true;
    }
  }

  if (topology_flags & FILTER_TOPO_WIRE) {
    if (BM_edge_is_wire(e)) {
      return true;
    }
  }

  if (topology_flags & (FILTER_TOPO_BOUNDARY_BY_FACE_SELECT | FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN |
                        FILTER_TOPO_BOUNDARY))
  {
    if (BM_edge_is_wire(e)) {
      return false;
    }
  }

  if (topology_flags & FILTER_TOPO_BOUNDARY) {
    if (BM_edge_is_boundary(e)) {
      return true;
    }
  }

  if (topology_flags & (FILTER_TOPO_BOUNDARY_BY_FACE_SELECT | FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN))
  {
    char test_flag;

    if (topology_flags & FILTER_TOPO_BOUNDARY_BY_FACE_SELECT) {
      test_flag = BM_ELEM_SELECT;
      if (!BM_elem_flag_test(e, BM_ELEM_SELECT)) {
        return false;
      }
    }
    else {
      test_flag = BM_ELEM_HIDDEN;
      if (BM_elem_flag_test(e, BM_ELEM_HIDDEN)) {
        return false;
      }
    }

    /* Count target flags, and if this number does not match the number of vertices, then it is a
     * boundary vertex */
    BMFace *f_other;
    BMIter face_iter;
    bool has_target_flag = false;
    bool has_non_target_flag = false;
    BM_ITER_ELEM (f_other, &face_iter, e, BM_FACES_OF_EDGE) {
      if (BM_elem_flag_test(f_other, test_flag)) {
        has_target_flag = true;
      }
      else {
        has_non_target_flag = true;
      }
      if (has_target_flag && has_non_target_flag) {
        return true;
      }
    }
  }

  return false;
}

inline bool is_edge_match_filter(BMEdge *e, QueryFilter *filter)
{
  if (filter->include_elem) {
    if (!is_elem_match_filter_flags(
            (BMHeader *)e, filter->include_elem, filter->match_all_include_elem_flags))
    {
      return false;
    }
  }
  if (filter->exclude_elem) {
    if (is_elem_match_filter_flags(
            (BMHeader *)e, filter->exclude_elem, filter->match_all_exclude_elem_flags))
    {
      return false;
    }
  }
  if (filter->include_topo) {
    if (!is_edge_match_filter_topology_flags(e, filter->include_topo)) {
      return false;
    }
  }
  if (filter->exclude_topo) {
    if ((is_edge_match_filter_topology_flags(e, filter->exclude_topo))) {
      return false;
    }
  }
  return true;
}

PyDoc_STRVAR(
    /* Wrap. */
    bpy_bm_query_filter_edges_doc,
    ".. method:: filter_edges(edges, *, include_flags={}, exclude_flags={}, "
    "include_topology={}, exclude_topology={}, match_all_include_elem_flags=False, "
    "match_all_exclude_elem_flags=False)\n"
    "\n"

    "   Filter edges by flags and topology.\n"
    "\n"
    "   :arg edges: The edges to filter.\n"
    "   :type face: :class:`bmesh.types.BMEdgeSeq` or Sequence[:class:`bmesh.types.BMEdge`]\n"

    "   :return: Filtered edges.\n"
    "   :rtype: list[:class:`bmesh.types.BMEdge`]\n");
static PyObject *bpy_bm_query_filter_edges(PyObject * /*self*/, PyObject *args, PyObject *kw)
{
  PyObject *list;
  PyObject *py_edge;
  PyObject *py_edge_seq;

  QueryFilter filter; /* optional */

  if (UNLIKELY(parse_and_check_vertex_filter_args(args,
                                                  kw,
                                                  &py_edge_seq,
                                                  &filter,
                                                  bpy_bm_filter_edges_flags,
                                                  bpy_bm_filter_topo_edges_flags) == -1))
  {
    return nullptr;
  }

  if BPy_BMEdgeSeq_Check (py_edge_seq) {
    BPY_BM_CHECK_OBJ(py_edge_seq);
    BMesh *bm = ((BPy_BMGeneric *)(py_edge_seq))->bm;

    /* Checking selection for a quick O(1) return. */
    if (bm_is_full_edge_selected(bm)) {
      if (filter.match_all_include_elem_flags) {
        if (filter.include_elem & BM_ELEM_HIDDEN) {
          return PyList_New(0);
        }
      }
      else {
        if (filter.include_elem == BM_ELEM_HIDDEN) {
          return PyList_New(0);
        }
        if (filter.include_elem == BM_ELEM_SELECT) {
          filter.include_elem = 0;
        }
      }
    }

    if (bm_is_full_edge_deselected(bm)) {
      if (filter.match_all_exclude_elem_flags) {
        if (filter.exclude_elem & BM_ELEM_SELECT) {
          return PyList_New(0);
        }
      }
      else {
        if (filter.exclude_elem == BM_ELEM_SELECT) {
          return PyList_New(0);
        }
      }
    }

    if (bm_is_full_face_selected(bm)) {
      if (filter.include_topo == FILTER_TOPO_BOUNDARY_BY_FACE_HIDDEN) {
        return PyList_New(0);
      }
      /* Avoid BM_FACES_OF_EDGE iteration if all faces are selected.*/
      if (filter.include_topo & FILTER_TOPO_BOUNDARY_BY_FACE_SELECT) {
        filter.include_topo &= ~FILTER_TOPO_BOUNDARY_BY_FACE_SELECT;
        filter.include_topo |= FILTER_TOPO_BOUNDARY;
      }
    }

    if (bm_is_full_face_deselected(bm)) {
      if (filter.include_topo == FILTER_TOPO_BOUNDARY_BY_FACE_SELECT) {
        return PyList_New(0);
      }
    }

    BMEdge **edges = static_cast<BMEdge **>(MEM_mallocN(sizeof(*edges) * bm->totedge, __func__));
    if (edges == nullptr) {
      PyErr_SetString(PyExc_MemoryError, "failed to create a sequence with filtered edges");
      return nullptr;
    }

    BMIter iter;
    BMEdge *e;
    int edges_len = 0;
    BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
      if (is_edge_match_filter(e, &filter)) {
        edges[edges_len++] = e;
      }
    }
    list = PyList_New(edges_len);
    if (list == nullptr) {
      MEM_freeN(edges);
      return nullptr;
    }

    for (int i = 0; i < edges_len; i++) {
      py_edge = BPy_BMEdge_CreatePyObject(bm, edges[i]);
      PyList_SetItem(list, i, py_edge);
    }

    MEM_freeN(edges);
    return list;
  }
  else {
    PyObject *it;
    PyObject *(*iternext)(PyObject *);

    it = PyObject_GetIter(py_edge_seq);
    if (it == nullptr)
      return nullptr;

    list = PyList_New(0);
    iternext = *Py_TYPE(it)->tp_iternext;

    for (;;) {
      py_edge = iternext(it);
      if (py_edge == nullptr)
        break;

      if (!BPy_BMEdge_Check(py_edge)) {
        PyErr_Format(PyExc_TypeError,
                     "bmesh.query.filter_edges: BMEdge expected, not '%.200s'",
                     Py_TYPE(py_edge)->tp_name);
        Py_DECREF(it);
        Py_DECREF(list);
        return nullptr;
      }
      if (bpy_bm_generic_valid_check((BPy_BMGeneric *)py_edge) == -1) {
        Py_DECREF(it);
        Py_DECREF(list);
        return nullptr;
      }
      if (is_edge_match_filter(((BPy_BMEdge *)py_edge)->e, &filter)) {
        PyList_APPEND(list, py_edge);
      }
    }

    Py_DECREF(it);
    if (PyErr_Occurred()) {
      if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
        PyErr_Clear();
      }
      else {
        Py_DECREF(list);
        return nullptr;
      }
    }
    return list;
  }
}

inline static bool is_face_match_filter_topology_flags(BMFace *f, const int topology_flags)
{
  if (topology_flags & FILTER_TOPO_TRIS && f->len == 3) {
    return true;
  }
  if (topology_flags & FILTER_TOPO_QUAD && f->len == 4) {
    return true;
  }
  if (topology_flags & FILTER_TOPO_NGONE && f->len >= 5) {
    return true;
  }
  return false;
}

inline bool is_face_match_filter(BMFace *f, QueryFilter *filter)
{
  if (filter->include_elem) {
    if (!is_elem_match_filter_flags(
            (BMHeader *)f, filter->include_elem, filter->match_all_include_elem_flags))
    {
      return false;
    }
  }
  if (filter->exclude_elem) {
    if (is_elem_match_filter_flags(
            (BMHeader *)f, filter->exclude_elem, filter->match_all_exclude_elem_flags))
    {
      return false;
    }
  }
  if (filter->include_topo) {
    if (!is_face_match_filter_topology_flags(f, filter->include_topo)) {
      return false;
    }
  }
  if (filter->exclude_topo) {
    if ((is_face_match_filter_topology_flags(f, filter->exclude_topo))) {
      return false;
    }
  }
  return true;
}

PyDoc_STRVAR(
    /* Wrap. */
    bpy_bm_query_filter_faces_doc,
    ".. method:: filter_faces(faces, *, include_flags={}, exclude_flags={}, "
    "include_topology={}, exclude_topology={}, match_all_include_elem_flags=False, "
    "match_all_exclude_elem_flags=False)\n"
    "\n"

    "   Filter faces by flags and topology.\n"
    "\n"
    "   :arg faces: The faces to filter.\n"
    "   :type face: :class:`bmesh.types.BMFaceSeq` or Sequence[:class:`bmesh.types.BMFace`]\n"

    "   :return: Filtered faces.\n"
    "   :rtype: list[:class:`bmesh.types.BMFace`]\n");
static PyObject *bpy_bm_query_filter_faces(PyObject * /*self*/, PyObject *args, PyObject *kw)
{
  PyObject *list;
  PyObject *py_face;
  PyObject *py_face_seq;

  QueryFilter filter; /* optional */

  if (UNLIKELY(parse_and_check_vertex_filter_args(args,
                                                  kw,
                                                  &py_face_seq,
                                                  &filter,
                                                  bpy_bm_filter_faces_elem_flags,
                                                  bpy_bm_filter_topo_faces_flags) == -1))
  {
    return nullptr;
  }

  if BPy_BMFaceSeq_Check (py_face_seq) {
    BPY_BM_CHECK_OBJ(py_face_seq);
    BMesh *bm = ((BPy_BMGeneric *)(py_face_seq))->bm;

    /* Checking selection for a quick O(1) return. */
    if (bm_is_full_face_selected(bm)) {
      if (filter.match_all_include_elem_flags) {
        if (filter.include_elem & BM_ELEM_HIDDEN) {
          return PyList_New(0);
        }
      }
      else {
        if (filter.include_elem == BM_ELEM_HIDDEN) {
          return PyList_New(0);
        }
      }
    }

    if (bm_is_full_face_deselected(bm)) {
      if (filter.match_all_include_elem_flags) {
        if (filter.include_elem & BM_ELEM_SELECT) {
          return PyList_New(0);
        }
      }
      else {
        if (filter.include_elem == BM_ELEM_SELECT) {
          return PyList_New(0);
        }
      }
    }

    /* Full triangulation check. */
    if (bm->totloop == bm->totface * 3) {
      if (filter.exclude_topo & FILTER_TOPO_TRIS) {
        return PyList_New(0);
      }
      /* Avoid the include-topology check for TRIS when fully triangulated. */
      if (filter.include_topo & FILTER_TOPO_TRIS) {
        filter.include_topo = 0;
      }
    }

    BMFace **faces = static_cast<BMFace **>(MEM_mallocN(sizeof(*faces) * bm->totface, __func__));
    if (faces == nullptr) {
      PyErr_SetString(PyExc_MemoryError, "failed to create a sequence with filtered faces");
      return nullptr;
    }

    BMIter iter;
    BMFace *f;
    int faces_len = 0;
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (is_face_match_filter(f, &filter)) {
        faces[faces_len++] = f;
      }
    }
    list = PyList_New(faces_len);
    if (list == nullptr) {
      MEM_freeN(faces);
      return nullptr;
    }

    for (int i = 0; i < faces_len; i++) {
      py_face = BPy_BMFace_CreatePyObject(bm, faces[i]);
      PyList_SetItem(list, i, py_face);
    }

    MEM_freeN(faces);
    return list;
  }
  else {
    PyObject *it;
    PyObject *(*iternext)(PyObject *);

    it = PyObject_GetIter(py_face_seq);
    if (it == nullptr)
      return nullptr;

    list = PyList_New(0);
    iternext = *Py_TYPE(it)->tp_iternext;

    for (;;) {
      py_face = iternext(it);
      if (py_face == nullptr)
        break;

      if (!BPy_BMFace_Check(py_face)) {
        PyErr_Format(PyExc_TypeError,
                     "bmesh.query.filter_faces: BMFace expected, not '%.200s'",
                     Py_TYPE(py_face)->tp_name);
        Py_DECREF(it);
        Py_DECREF(list);
        return nullptr;
      }
      if (bpy_bm_generic_valid_check((BPy_BMGeneric *)py_face) == -1) {
        Py_DECREF(it);
        Py_DECREF(list);
        return nullptr;
      }
      if (is_face_match_filter(((BPy_BMFace *)py_face)->f, &filter)) {
        PyList_APPEND(list, py_face);
      }
    }

    Py_DECREF(it);
    if (PyErr_Occurred()) {
      if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
        PyErr_Clear();
      }
      else {
        Py_DECREF(list);
        return nullptr;
      }
    }
    return list;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Module Definition
 * \{ */

static PyMethodDef QUERY_filter_methods[] = {
    {"filter_verts",
     (PyCFunction)bpy_bm_query_filter_verts,
     METH_VARARGS | METH_KEYWORDS,
     bpy_bm_query_filter_verts_doc},
    {"filter_edges",
     (PyCFunction)bpy_bm_query_filter_edges,
     METH_VARARGS | METH_KEYWORDS,
     bpy_bm_query_filter_edges_doc},
    {"filter_faces",
     (PyCFunction)bpy_bm_query_filter_faces,
     METH_VARARGS | METH_KEYWORDS,
     bpy_bm_query_filter_faces_doc},
    {nullptr, nullptr, 0, nullptr},
};

static PyModuleDef QUERY_filter_module_def = {
    /*m_base*/ PyModuleDef_HEAD_INIT,
    /*m_name*/ "bmesh.query",
    /*m_doc*/ QUERY_filter_doc,
    /*m_size*/ 0,
    /*m_methods*/ QUERY_filter_methods,
    /*m_slots*/ nullptr,
    /*m_traverse*/ nullptr,
    /*m_clear*/ nullptr,
    /*m_free*/ nullptr,
};

PyMODINIT_FUNC BPyInit_bmesh_query()
{
  PyObject *submodule = PyModule_Create(&QUERY_filter_module_def);
  return submodule;
}

/** \} */
