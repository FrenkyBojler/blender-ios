/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pymathutils
 */

#include <Python.h>

#include "mathutils.hh"
#include "mathutils_geometry.hh"

/* Used for PolyFill */
#ifndef MATH_STANDALONE /* define when building outside blender */
#  include "BLI_boxpack_2d.hh"
#  include "BLI_convexhull_2d.hh"
#  include "BLI_delaunay_2d.hh"
#  include "BLI_listbase.hh"

#  include "BKE_curve.hh"

#  include "MEM_guardedalloc.h"
#endif /* !MATH_STANDALONE */

#include "BLI_math_geom_c.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_offset_indices.hh"
#include "BLI_utildefines.hh"

#include "../generic/py_capi_utils.hh"
#include "../generic/python_compat.hh" /* IWYU pragma: keep. */
#include "../generic/python_utildefines.hh"

namespace blender {

/* ---------------------------------INTERSECTION FUNCTIONS-------------------- */

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_intersect_ray_tri_doc,
    ".. function:: intersect_ray_tri(v1, v2, v3, ray, orig, clip=True, /)\n"
    "\n"
    "   Returns the intersection between a ray and a triangle, if possible, returns None "
    "otherwise.\n"
    "\n"
    "   :param v1: Point1\n"
    "   :type v1: :class:`mathutils.Vector`\n"
    "   :param v2: Point2\n"
    "   :type v2: :class:`mathutils.Vector`\n"
    "   :param v3: Point3\n"
    "   :type v3: :class:`mathutils.Vector`\n"
    "   :param ray: Direction of the ray\n"
    "   :type ray: :class:`mathutils.Vector`\n"
    "   :param orig: Origin\n"
    "   :type orig: :class:`mathutils.Vector`\n"
    "   :param clip: When False, don't restrict the intersection to the area of the "
    "triangle, use the infinite plane defined by the triangle.\n"
    "   :type clip: bool\n"
    "   :return: The point of intersection or None if no intersection is found\n"
    "   :rtype: :class:`mathutils.Vector` | None\n");
static PyObject *M_Geometry_intersect_ray_tri(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "intersect_ray_tri";
  PyObject *py_ray, *py_ray_off, *py_tri[3];
  float dir[3], orig[3], tri[3][3], e1[3], e2[3], pvec[3], tvec[3], qvec[3];
  float det, inv_det, u, v, t;
  bool clip = true;
  int i;

  if (!PyArg_ParseTuple(args,
                        "O"  /* `v1` */
                        "O"  /* `v2` */
                        "O"  /* `v3` */
                        "O"  /* `ray` */
                        "O"  /* `orig` */
                        "|"  /* Optional arguments. */
                        "O&" /* `clip` */
                        ":intersect_ray_tri",
                        UNPACK3_EX(&, py_tri, ),
                        &py_ray,
                        &py_ray_off,
                        PyC_ParseBool,
                        &clip))
  {
    return nullptr;
  }

  if (((mathutils_array_parse(dir, 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_ray, error_prefix) !=
        -1) &&
       (mathutils_array_parse(
            orig, 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_ray_off, error_prefix) != -1)) == 0)
  {
    return nullptr;
  }

  for (i = 0; i < ARRAY_SIZE(tri); i++) {
    if (mathutils_array_parse(
            tri[i], 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_tri[i], error_prefix) == -1)
    {
      return nullptr;
    }
  }

  normalize_v3(dir);

  /* find vectors for two edges sharing v1 */
  sub_v3_v3v3(e1, tri[1], tri[0]);
  sub_v3_v3v3(e2, tri[2], tri[0]);

  /* begin calculating determinant - also used to calculated U parameter */
  cross_v3_v3v3(pvec, dir, e2);

  /* if determinant is near zero, ray lies in plane of triangle */
  det = dot_v3v3(e1, pvec);

  if (det > -0.000001f && det < 0.000001f) {
    Py_RETURN_NONE;
  }

  inv_det = 1.0f / det;

  /* calculate distance from v1 to ray origin */
  sub_v3_v3v3(tvec, orig, tri[0]);

  /* calculate U parameter and test bounds */
  u = dot_v3v3(tvec, pvec) * inv_det;
  if (clip && (u < 0.0f || u > 1.0f)) {
    Py_RETURN_NONE;
  }

  /* prepare to test the V parameter */
  cross_v3_v3v3(qvec, tvec, e1);

  /* calculate V parameter and test bounds */
  v = dot_v3v3(dir, qvec) * inv_det;

  if (clip && (v < 0.0f || u + v > 1.0f)) {
    Py_RETURN_NONE;
  }

  /* calculate t, ray intersects triangle */
  t = dot_v3v3(e2, qvec) * inv_det;

  /* ray hit behind */
  if (t < 0.0f) {
    Py_RETURN_NONE;
  }

  mul_v3_fl(dir, t);
  add_v3_v3v3(pvec, orig, dir);

  return Vector_CreatePyObject(pvec, 3, nullptr);
}

/* Line-Line intersection using algorithm from mathworld.wolfram.com */

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_intersect_line_line_doc,
    ".. function:: intersect_line_line(v1, v2, v3, v4, /)\n"
    "\n"
    "   Returns a tuple with the points on each line respectively closest to the other.\n"
    "\n"
    "   :param v1: First point of the first line\n"
    "   :type v1: :class:`mathutils.Vector`\n"
    "   :param v2: Second point of the first line\n"
    "   :type v2: :class:`mathutils.Vector`\n"
    "   :param v3: First point of the second line\n"
    "   :type v3: :class:`mathutils.Vector`\n"
    "   :param v4: Second point of the second line\n"
    "   :type v4: :class:`mathutils.Vector`\n"
    "   :return: The intersection on each line or None when the lines are parallel.\n"
    "   :rtype: tuple[:class:`mathutils.Vector`, :class:`mathutils.Vector`] | None\n");
static PyObject *M_Geometry_intersect_line_line(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "intersect_line_line";
  PyObject *tuple;
  PyObject *py_lines[4];
  float lines[4][3], i1[3], i2[3];
  int ix_vec_num;
  int result;

  if (!PyArg_ParseTuple(args,
                        "O" /* `v1` */
                        "O" /* `v2` */
                        "O" /* `v3` */
                        "O" /* `v4` */
                        ":intersect_line_line",
                        UNPACK4_EX(&, py_lines, )))
  {
    return nullptr;
  }

  if ((((ix_vec_num = mathutils_array_parse(
             lines[0], 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_lines[0], error_prefix)) != -1) &&
       (mathutils_array_parse(lines[1],
                              ix_vec_num,
                              ix_vec_num | MU_ARRAY_SPILL | MU_ARRAY_ZERO,
                              py_lines[1],
                              error_prefix) != -1) &&
       (mathutils_array_parse(lines[2],
                              ix_vec_num,
                              ix_vec_num | MU_ARRAY_SPILL | MU_ARRAY_ZERO,
                              py_lines[2],
                              error_prefix) != -1) &&
       (mathutils_array_parse(lines[3],
                              ix_vec_num,
                              ix_vec_num | MU_ARRAY_SPILL | MU_ARRAY_ZERO,
                              py_lines[3],
                              error_prefix) != -1)) == 0)
  {
    return nullptr;
  }

  /* Zero 3rd axis of 2D vectors. */
  if (ix_vec_num == 2) {
    lines[1][2] = 0.0f;
    lines[2][2] = 0.0f;
    lines[3][2] = 0.0f;
  }

  result = isect_line_line_v3(UNPACK4(lines), i1, i2);
  /* The return-code isn't exposed,
   * this way we can check know how close the lines are. */
  if (result == 1) {
    closest_to_line_v3(i2, i1, lines[2], lines[3]);
  }

  if (result == 0) {
    /* Parallel. */
    Py_RETURN_NONE;
  }

  tuple = PyTuple_New(2);
  PyTuple_SET_ITEMS(tuple,
                    Vector_CreatePyObject(i1, ix_vec_num, nullptr),
                    Vector_CreatePyObject(i2, ix_vec_num, nullptr));
  return tuple;
}

/* Line-Line intersection using algorithm from mathworld.wolfram.com */

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_intersect_sphere_sphere_2d_doc,
    ".. function:: intersect_sphere_sphere_2d(p_a, radius_a, p_b, radius_b, /)\n"
    "\n"
    "   Returns the 2 intersection points of two circles.\n"
    "\n"
    "   :param p_a: Center of the first circle\n"
    "   :type p_a: :class:`mathutils.Vector`\n"
    "   :param radius_a: Radius of the first circle\n"
    "   :type radius_a: float\n"
    "   :param p_b: Center of the second circle\n"
    "   :type p_b: :class:`mathutils.Vector`\n"
    "   :param radius_b: Radius of the second circle\n"
    "   :type radius_b: float\n"
    "   :return: The 2 intersection points or None when there is no intersection.\n"
    "   :rtype: tuple[:class:`mathutils.Vector`, :class:`mathutils.Vector`] | "
    "tuple[None, None]\n");
static PyObject *M_Geometry_intersect_sphere_sphere_2d(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "intersect_sphere_sphere_2d";
  PyObject *ret;
  PyObject *py_v_a, *py_v_b;
  float v_a[2], v_b[2];
  float rad_a, rad_b;
  float v_ab[2];
  float dist;

  if (!PyArg_ParseTuple(args,
                        "O" /* `p_a` */
                        "f" /* `radius_a` */
                        "O" /* `p_b` */
                        "f" /* `radius_b` */
                        ":intersect_sphere_sphere_2d",
                        &py_v_a,
                        &rad_a,
                        &py_v_b,
                        &rad_b))
  {
    return nullptr;
  }

  if (((mathutils_array_parse(v_a, 2, 2, py_v_a, error_prefix) != -1) &&
       (mathutils_array_parse(v_b, 2, 2, py_v_b, error_prefix) != -1)) == 0)
  {
    return nullptr;
  }

  ret = PyTuple_New(2);

  sub_v2_v2v2(v_ab, v_b, v_a);
  dist = len_v2(v_ab);

  if (/* out of range */
      (dist > rad_a + rad_b) ||
      /* fully-contained in the other */
      (dist < fabsf(rad_a - rad_b)) ||
      /* co-incident */
      (dist < FLT_EPSILON))
  {
    /* out of range */
    PyTuple_SET_ITEMS(ret, Py_NewRef(Py_None), Py_NewRef(Py_None));
  }
  else {
    const float dist_delta = ((rad_a * rad_a) - (rad_b * rad_b) + (dist * dist)) / (2.0f * dist);
    const float h = powf(fabsf((rad_a * rad_a) - (dist_delta * dist_delta)), 0.5f);
    float i_cent[2];
    float i1[2], i2[2];

    i_cent[0] = v_a[0] + ((v_ab[0] * dist_delta) / dist);
    i_cent[1] = v_a[1] + ((v_ab[1] * dist_delta) / dist);

    i1[0] = i_cent[0] + h * v_ab[1] / dist;
    i1[1] = i_cent[1] - h * v_ab[0] / dist;

    i2[0] = i_cent[0] - h * v_ab[1] / dist;
    i2[1] = i_cent[1] + h * v_ab[0] / dist;

    PyTuple_SET_ITEMS(
        ret, Vector_CreatePyObject(i1, 2, nullptr), Vector_CreatePyObject(i2, 2, nullptr));
  }

  return ret;
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_intersect_tri_tri_2d_doc,
    ".. function:: intersect_tri_tri_2d(tri_a1, tri_a2, tri_a3, tri_b1, tri_b2, tri_b3, /)\n"
    "\n"
    "   Check if two 2D triangles intersect.\n"
    "\n"
    "   :param tri_a1: First vertex of the first triangle.\n"
    "   :type tri_a1: :class:`mathutils.Vector`\n"
    "   :param tri_a2: Second vertex of the first triangle.\n"
    "   :type tri_a2: :class:`mathutils.Vector`\n"
    "   :param tri_a3: Third vertex of the first triangle.\n"
    "   :type tri_a3: :class:`mathutils.Vector`\n"
    "   :param tri_b1: First vertex of the second triangle.\n"
    "   :type tri_b1: :class:`mathutils.Vector`\n"
    "   :param tri_b2: Second vertex of the second triangle.\n"
    "   :type tri_b2: :class:`mathutils.Vector`\n"
    "   :param tri_b3: Third vertex of the second triangle.\n"
    "   :type tri_b3: :class:`mathutils.Vector`\n"
    "   :return: True if the triangles intersect.\n"
    "   :rtype: bool\n");
static PyObject *M_Geometry_intersect_tri_tri_2d(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "intersect_tri_tri_2d";
  PyObject *tri_pair_py[2][3];
  float tri_pair[2][3][2];

  if (!PyArg_ParseTuple(args,
                        "O" /* `tri_a1` */
                        "O" /* `tri_a2` */
                        "O" /* `tri_a3` */
                        "O" /* `tri_b1` */
                        "O" /* `tri_b2` */
                        "O" /* `tri_b3` */
                        ":intersect_tri_tri_2d",
                        &tri_pair_py[0][0],
                        &tri_pair_py[0][1],
                        &tri_pair_py[0][2],
                        &tri_pair_py[1][0],
                        &tri_pair_py[1][1],
                        &tri_pair_py[1][2]))
  {
    return nullptr;
  }

  for (int i = 0; i < 2; i++) {
    for (int j = 0; j < 3; j++) {
      if (mathutils_array_parse(
              tri_pair[i][j], 2, 2 | MU_ARRAY_SPILL, tri_pair_py[i][j], error_prefix) == -1)
      {
        return nullptr;
      }
    }
  }

  const bool ret = isect_tri_tri_v2(UNPACK3(tri_pair[0]), UNPACK3(tri_pair[1]));
  return PyBool_FromLong(ret);
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_normal_doc,
    ".. function:: normal(*vectors)\n"
    "\n"
    "   Returns the normal of a 3D polygon.\n"
    "\n"
    "   :param vectors: 3 or more vectors to calculate normals.\n"
    "   :type vectors: Sequence[Sequence[float]]\n"
    "   :return: The normal vector.\n"
    "   :rtype: :class:`mathutils.Vector`\n");
static PyObject *M_Geometry_normal(PyObject * /*self*/, PyObject *args)
{
  float (*coords)[3];
  int coords_len;
  float n[3];
  PyObject *ret = nullptr;

  /* use */
  if (PyTuple_GET_SIZE(args) == 1) {
    args = PyTuple_GET_ITEM(args, 0);
  }

  if ((coords_len = mathutils_array_parse_alloc_v(
           reinterpret_cast<float **>(&coords), 3 | MU_ARRAY_SPILL, args, "normal")) == -1)
  {
    return nullptr;
  }

  if (coords_len < 3) {
    PyErr_SetString(PyExc_ValueError, "Expected 3 or more vectors");
  }
  else {
    normal_poly_v3(n, coords, coords_len);
    ret = Vector_CreatePyObject(n, 3, nullptr);
  }

  PyMem_Free(coords);
  return ret;
}

/* --------------------------------- AREA FUNCTIONS-------------------- */

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_area_tri_doc,
    ".. function:: area_tri(v1, v2, v3, /)\n"
    "\n"
    "   Returns the area of the 2D or 3D triangle defined.\n"
    "\n"
    "   :param v1: Point1\n"
    "   :type v1: :class:`mathutils.Vector`\n"
    "   :param v2: Point2\n"
    "   :type v2: :class:`mathutils.Vector`\n"
    "   :param v3: Point3\n"
    "   :type v3: :class:`mathutils.Vector`\n"
    "   :return: The area of the triangle.\n"
    "   :rtype: float\n");
static PyObject *M_Geometry_area_tri(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "area_tri";
  PyObject *py_tri[3];
  float tri[3][3];
  int len;

  if (!PyArg_ParseTuple(args,
                        "O" /* `v1` */
                        "O" /* `v2` */
                        "O" /* `v3` */
                        ":area_tri",
                        UNPACK3_EX(&, py_tri, )))
  {
    return nullptr;
  }

  if ((((len = mathutils_array_parse(tri[0], 2, 3, py_tri[0], error_prefix)) != -1) &&
       (mathutils_array_parse(tri[1], len, len, py_tri[1], error_prefix) != -1) &&
       (mathutils_array_parse(tri[2], len, len, py_tri[2], error_prefix) != -1)) == 0)
  {
    return nullptr;
  }

  return PyFloat_FromDouble((len == 3 ? area_tri_v3 : area_tri_v2)(UNPACK3(tri)));
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_volume_tetrahedron_doc,
    ".. function:: volume_tetrahedron(v1, v2, v3, v4, /)\n"
    "\n"
    "   Return the absolute (unsigned) volume formed by a tetrahedron "
    "(points can be in any order).\n"
    "\n"
    "   :param v1: Point1\n"
    "   :type v1: :class:`mathutils.Vector`\n"
    "   :param v2: Point2\n"
    "   :type v2: :class:`mathutils.Vector`\n"
    "   :param v3: Point3\n"
    "   :type v3: :class:`mathutils.Vector`\n"
    "   :param v4: Point4\n"
    "   :type v4: :class:`mathutils.Vector`\n"
    "   :return: The volume of the tetrahedron.\n"
    "   :rtype: float\n");
static PyObject *M_Geometry_volume_tetrahedron(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "volume_tetrahedron";
  PyObject *py_tet[4];
  float tet[4][3];
  int i;

  if (!PyArg_ParseTuple(args,
                        "O" /* `v1` */
                        "O" /* `v2` */
                        "O" /* `v3` */
                        "O" /* `v4` */
                        ":volume_tetrahedron",
                        UNPACK4_EX(&, py_tet, )))
  {
    return nullptr;
  }

  for (i = 0; i < ARRAY_SIZE(tet); i++) {
    if (mathutils_array_parse(tet[i], 3, 3 | MU_ARRAY_SPILL, py_tet[i], error_prefix) == -1) {
      return nullptr;
    }
  }

  return PyFloat_FromDouble(volume_tetrahedron_v3(UNPACK4(tet)));
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_intersect_line_line_2d_doc,
    ".. function:: intersect_line_line_2d(lineA_p1, lineA_p2, lineB_p1, lineB_p2, /)\n"
    "\n"
    "   Takes 2 segments (defined by 4 vectors) and returns a vector for their point of "
    "intersection or None.\n"
    "\n"
    "   .. warning:: Despite its name, this function works on segments, and not on lines.\n"
    "\n"
    "   :param lineA_p1: First point of the first segment\n"
    "   :type lineA_p1: :class:`mathutils.Vector`\n"
    "   :param lineA_p2: Second point of the first segment\n"
    "   :type lineA_p2: :class:`mathutils.Vector`\n"
    "   :param lineB_p1: First point of the second segment\n"
    "   :type lineB_p1: :class:`mathutils.Vector`\n"
    "   :param lineB_p2: Second point of the second segment\n"
    "   :type lineB_p2: :class:`mathutils.Vector`\n"
    "   :return: The point of intersection or None when not found\n"
    "   :rtype: :class:`mathutils.Vector` | None\n");
static PyObject *M_Geometry_intersect_line_line_2d(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "intersect_line_line_2d";
  PyObject *py_lines[4];
  float lines[4][2];
  float vi[2];
  int i;

  if (!PyArg_ParseTuple(args,
                        "O" /* `lineA_p1` */
                        "O" /* `lineA_p2` */
                        "O" /* `lineB_p1` */
                        "O" /* `lineB_p2` */
                        ":intersect_line_line_2d",
                        UNPACK4_EX(&, py_lines, )))
  {
    return nullptr;
  }

  for (i = 0; i < ARRAY_SIZE(lines); i++) {
    if (mathutils_array_parse(lines[i], 2, 2 | MU_ARRAY_SPILL, py_lines[i], error_prefix) == -1) {
      return nullptr;
    }
  }

  if (isect_seg_seg_v2_point(UNPACK4(lines), vi) == 1) {
    return Vector_CreatePyObject(vi, 2, nullptr);
  }

  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_intersect_line_plane_doc,
    ".. function:: intersect_line_plane(line_a, line_b, plane_co, plane_no, no_flip=False, /)\n"
    "\n"
    "   Calculate the intersection between a line (as 2 vectors) and a plane.\n"
    "   Returns a vector for the intersection or None.\n"
    "\n"
    "   :param line_a: First point of the line\n"
    "   :type line_a: :class:`mathutils.Vector`\n"
    "   :param line_b: Second point of the line\n"
    "   :type line_b: :class:`mathutils.Vector`\n"
    "   :param plane_co: A point on the plane\n"
    "   :type plane_co: :class:`mathutils.Vector`\n"
    "   :param plane_no: The direction the plane is facing\n"
    "   :type plane_no: :class:`mathutils.Vector`\n"
    "   :param no_flip: Currently ignored.\n"
    "   :type no_flip: bool\n"
    "   :return: The point of intersection or None when not found\n"
    "   :rtype: :class:`mathutils.Vector` | None\n");
static PyObject *M_Geometry_intersect_line_plane(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "intersect_line_plane";
  PyObject *py_line_a, *py_line_b, *py_plane_co, *py_plane_no;
  float line_a[3], line_b[3], plane_co[3], plane_no[3];
  float isect[3];
  const bool no_flip = false;

  if (!PyArg_ParseTuple(args,
                        "O"  /* `line_a` */
                        "O"  /* `line_b` */
                        "O"  /* `plane_co` */
                        "O"  /* `plane_no` */
                        "|"  /* Optional arguments. */
                        "O&" /* `no_flip` */
                        ":intersect_line_plane",
                        &py_line_a,
                        &py_line_b,
                        &py_plane_co,
                        &py_plane_no,
                        PyC_ParseBool,
                        &no_flip))
  {
    return nullptr;
  }

  if (((mathutils_array_parse(line_a, 3, 3 | MU_ARRAY_SPILL, py_line_a, error_prefix) != -1) &&
       (mathutils_array_parse(line_b, 3, 3 | MU_ARRAY_SPILL, py_line_b, error_prefix) != -1) &&
       (mathutils_array_parse(plane_co, 3, 3 | MU_ARRAY_SPILL, py_plane_co, error_prefix) != -1) &&
       (mathutils_array_parse(plane_no, 3, 3 | MU_ARRAY_SPILL, py_plane_no, error_prefix) !=
        -1)) == 0)
  {
    return nullptr;
  }

  /* TODO: implements no_flip */
  if (isect_line_plane_v3(isect, line_a, line_b, plane_co, plane_no) == 1) {
    return Vector_CreatePyObject(isect, 3, nullptr);
  }

  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_intersect_plane_plane_doc,
    ".. function:: intersect_plane_plane(plane_a_co, plane_a_no, plane_b_co, plane_b_no, /)\n"
    "\n"
    "   Return the intersection between two planes\n"
    "\n"
    "   :param plane_a_co: Point on the first plane\n"
    "   :type plane_a_co: :class:`mathutils.Vector`\n"
    "   :param plane_a_no: Normal of the first plane\n"
    "   :type plane_a_no: :class:`mathutils.Vector`\n"
    "   :param plane_b_co: Point on the second plane\n"
    "   :type plane_b_co: :class:`mathutils.Vector`\n"
    "   :param plane_b_no: Normal of the second plane\n"
    "   :type plane_b_no: :class:`mathutils.Vector`\n"
    "   :return: The line of the intersection represented as a point and a vector or None if the "
    "intersection can't be calculated\n"
    "   :rtype: tuple[:class:`mathutils.Vector`, :class:`mathutils.Vector`] | "
    "tuple[None, None]\n");
static PyObject *M_Geometry_intersect_plane_plane(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "intersect_plane_plane";
  PyObject *ret, *ret_co, *ret_no;
  PyObject *py_plane_a_co, *py_plane_a_no, *py_plane_b_co, *py_plane_b_no;
  float plane_a_co[3], plane_a_no[3], plane_b_co[3], plane_b_no[3];
  float plane_a[4], plane_b[4];

  float isect_co[3];
  float isect_no[3];

  if (!PyArg_ParseTuple(args,
                        "O" /* `plane_a_co` */
                        "O" /* `plane_a_no` */
                        "O" /* `plane_b_co` */
                        "O" /* `plane_b_no` */
                        ":intersect_plane_plane",
                        &py_plane_a_co,
                        &py_plane_a_no,
                        &py_plane_b_co,
                        &py_plane_b_no))
  {
    return nullptr;
  }

  if (((mathutils_array_parse(plane_a_co, 3, 3 | MU_ARRAY_SPILL, py_plane_a_co, error_prefix) !=
        -1) &&
       (mathutils_array_parse(plane_a_no, 3, 3 | MU_ARRAY_SPILL, py_plane_a_no, error_prefix) !=
        -1) &&
       (mathutils_array_parse(plane_b_co, 3, 3 | MU_ARRAY_SPILL, py_plane_b_co, error_prefix) !=
        -1) &&
       (mathutils_array_parse(plane_b_no, 3, 3 | MU_ARRAY_SPILL, py_plane_b_no, error_prefix) !=
        -1)) == 0)
  {
    return nullptr;
  }

  plane_from_point_normal_v3(plane_a, plane_a_co, plane_a_no);
  plane_from_point_normal_v3(plane_b, plane_b_co, plane_b_no);

  if (isect_plane_plane_v3(plane_a, plane_b, isect_co, isect_no)) {
    normalize_v3(isect_no);

    ret_co = Vector_CreatePyObject(isect_co, 3, nullptr);
    ret_no = Vector_CreatePyObject(isect_no, 3, nullptr);
  }
  else {
    ret_co = Py_NewRef(Py_None);
    ret_no = Py_NewRef(Py_None);
  }

  ret = PyTuple_New(2);
  PyTuple_SET_ITEMS(ret, ret_co, ret_no);
  return ret;
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_intersect_line_sphere_doc,
    ".. function:: intersect_line_sphere(line_a, line_b, sphere_co, sphere_radius, clip=True, /)\n"
    "\n"
    "   Takes a line (as 2 points) and a sphere (as a point and a radius) and\n"
    "   returns the intersection\n"
    "\n"
    "   :param line_a: First point of the line\n"
    "   :type line_a: :class:`mathutils.Vector`\n"
    "   :param line_b: Second point of the line\n"
    "   :type line_b: :class:`mathutils.Vector`\n"
    "   :param sphere_co: The center of the sphere\n"
    "   :type sphere_co: :class:`mathutils.Vector`\n"
    "   :param sphere_radius: Radius of the sphere\n"
    "   :type sphere_radius: float\n"
    "   :param clip: When False, don't restrict the intersection to the line segment.\n"
    "   :type clip: bool\n"
    "   :return: The intersection points as a pair of vectors "
    "(each is None when not found).\n"
    "   :rtype: tuple[:class:`mathutils.Vector` | None, :class:`mathutils.Vector` | None]\n");
static PyObject *M_Geometry_intersect_line_sphere(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "intersect_line_sphere";
  PyObject *py_line_a, *py_line_b, *py_sphere_co;
  float line_a[3], line_b[3], sphere_co[3];
  float sphere_radius;
  bool clip = true;

  float isect_a[3];
  float isect_b[3];

  if (!PyArg_ParseTuple(args,
                        "O"  /* `line_a` */
                        "O"  /* `line_b` */
                        "O"  /* `sphere_co` */
                        "f"  /* `sphere_radius` */
                        "|"  /* Optional arguments. */
                        "O&" /* `clip` */
                        ":intersect_line_sphere",
                        &py_line_a,
                        &py_line_b,
                        &py_sphere_co,
                        &sphere_radius,
                        PyC_ParseBool,
                        &clip))
  {
    return nullptr;
  }

  if (((mathutils_array_parse(line_a, 3, 3 | MU_ARRAY_SPILL, py_line_a, error_prefix) != -1) &&
       (mathutils_array_parse(line_b, 3, 3 | MU_ARRAY_SPILL, py_line_b, error_prefix) != -1) &&
       (mathutils_array_parse(sphere_co, 3, 3 | MU_ARRAY_SPILL, py_sphere_co, error_prefix) !=
        -1)) == 0)
  {
    return nullptr;
  }

  bool use_a = true;
  bool use_b = true;
  float lambda;

  PyObject *ret = PyTuple_New(2);

  switch (isect_line_sphere_v3(line_a, line_b, sphere_co, sphere_radius, isect_a, isect_b)) {
    case 1:
      if (!(!clip || (((lambda = line_point_factor_v3(isect_a, line_a, line_b)) >= 0.0f) &&
                      (lambda <= 1.0f))))
      {
        use_a = false;
      }
      use_b = false;
      break;
    case 2:
      if (!(!clip || (((lambda = line_point_factor_v3(isect_a, line_a, line_b)) >= 0.0f) &&
                      (lambda <= 1.0f))))
      {
        use_a = false;
      }
      if (!(!clip || (((lambda = line_point_factor_v3(isect_b, line_a, line_b)) >= 0.0f) &&
                      (lambda <= 1.0f))))
      {
        use_b = false;
      }
      break;
    default:
      use_a = false;
      use_b = false;
      break;
  }

  PyTuple_SET_ITEMS(ret,
                    use_a ? Vector_CreatePyObject(isect_a, 3, nullptr) : Py_NewRef(Py_None),
                    use_b ? Vector_CreatePyObject(isect_b, 3, nullptr) : Py_NewRef(Py_None));

  return ret;
}

/* keep in sync with M_Geometry_intersect_line_sphere */
PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_intersect_line_sphere_2d_doc,
    ".. function:: intersect_line_sphere_2d(line_a, line_b, sphere_co, "
    "sphere_radius, clip=True, /)\n"
    "\n"
    "   Takes a line (as 2 points) and a circle (as a point and a radius) and\n"
    "   returns the intersection\n"
    "\n"
    "   :param line_a: First point of the line\n"
    "   :type line_a: :class:`mathutils.Vector`\n"
    "   :param line_b: Second point of the line\n"
    "   :type line_b: :class:`mathutils.Vector`\n"
    "   :param sphere_co: The center of the circle\n"
    "   :type sphere_co: :class:`mathutils.Vector`\n"
    "   :param sphere_radius: Radius of the circle\n"
    "   :type sphere_radius: float\n"
    "   :param clip: When False, don't restrict the intersection to the line segment.\n"
    "   :type clip: bool\n"
    "   :return: The intersection points as a pair of vectors "
    "(each is None when not found).\n"
    "   :rtype: tuple[:class:`mathutils.Vector` | None, :class:`mathutils.Vector` | None]\n");
static PyObject *M_Geometry_intersect_line_sphere_2d(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "intersect_line_sphere_2d";
  PyObject *py_line_a, *py_line_b, *py_sphere_co;
  float line_a[2], line_b[2], sphere_co[2];
  float sphere_radius;
  bool clip = true;

  float isect_a[2];
  float isect_b[2];

  if (!PyArg_ParseTuple(args,
                        "O"  /* `line_a` */
                        "O"  /* `line_b` */
                        "O"  /* `sphere_co` */
                        "f"  /* `sphere_radius` */
                        "|"  /* Optional arguments. */
                        "O&" /* `clip` */
                        ":intersect_line_sphere_2d",
                        &py_line_a,
                        &py_line_b,
                        &py_sphere_co,
                        &sphere_radius,
                        PyC_ParseBool,
                        &clip))
  {
    return nullptr;
  }

  if (((mathutils_array_parse(line_a, 2, 2 | MU_ARRAY_SPILL, py_line_a, error_prefix) != -1) &&
       (mathutils_array_parse(line_b, 2, 2 | MU_ARRAY_SPILL, py_line_b, error_prefix) != -1) &&
       (mathutils_array_parse(sphere_co, 2, 2 | MU_ARRAY_SPILL, py_sphere_co, error_prefix) !=
        -1)) == 0)
  {
    return nullptr;
  }

  bool use_a = true;
  bool use_b = true;
  float lambda;

  PyObject *ret = PyTuple_New(2);

  switch (isect_line_sphere_v2(line_a, line_b, sphere_co, sphere_radius, isect_a, isect_b)) {
    case 1:
      if (!(!clip || (((lambda = line_point_factor_v2(isect_a, line_a, line_b)) >= 0.0f) &&
                      (lambda <= 1.0f))))
      {
        use_a = false;
      }
      use_b = false;
      break;
    case 2:
      if (!(!clip || (((lambda = line_point_factor_v2(isect_a, line_a, line_b)) >= 0.0f) &&
                      (lambda <= 1.0f))))
      {
        use_a = false;
      }
      if (!(!clip || (((lambda = line_point_factor_v2(isect_b, line_a, line_b)) >= 0.0f) &&
                      (lambda <= 1.0f))))
      {
        use_b = false;
      }
      break;
    default:
      use_a = false;
      use_b = false;
      break;
  }

  PyTuple_SET_ITEMS(ret,
                    use_a ? Vector_CreatePyObject(isect_a, 2, nullptr) : Py_NewRef(Py_None),
                    use_b ? Vector_CreatePyObject(isect_b, 2, nullptr) : Py_NewRef(Py_None));

  return ret;
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_intersect_point_line_doc,
    ".. function:: intersect_point_line(pt, line_p1, line_p2, /)\n"
    "\n"
    "   Takes a point and a line and returns the closest point on the line and its "
    "parametric distance from the first point of the line. "
    "A value of 0.0 is the first point, 1.0 is the second, "
    "values outside [0, 1] are extrapolated.\n"
    "\n"
    "   :param pt: Point\n"
    "   :type pt: :class:`mathutils.Vector`\n"
    "   :param line_p1: First point of the line\n"
    "   :type line_p1: :class:`mathutils.Vector`\n"
    "   :param line_p2: Second point of the line\n"
    "   :type line_p2: :class:`mathutils.Vector`\n"
    "   :return: The closest point on the line and its parametric distance from the first point.\n"
    "   :rtype: tuple[:class:`mathutils.Vector`, float]\n");
static PyObject *M_Geometry_intersect_point_line(PyObject * /*self*/,
                                                 PyObject *const *args,
                                                 Py_ssize_t nargs)
{
  const char *error_prefix = "intersect_point_line";
  float pt[3], pt_out[3], line_a[3], line_b[3];
  int pt_num = 2;

  if (!_PyArg_CheckPositional(error_prefix, nargs, 3, 3)) {
    return nullptr;
  }

  PyObject *py_pt = args[0];
  PyObject *py_line_a = args[1];
  PyObject *py_line_b = args[2];

  /* Accept 2D verts. */
  if ((((pt_num = mathutils_array_parse(
             pt, 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_pt, error_prefix)) != -1) &&
       (mathutils_array_parse(
            line_a, 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_line_a, error_prefix) != -1) &&
       (mathutils_array_parse(
            line_b, 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_line_b, error_prefix) != -1)) == 0)
  {
    return nullptr;
  }

  /* Do the calculation. */
  const float lambda = closest_to_line_v3(pt_out, pt, line_a, line_b);

  PyObject *ret = PyTuple_New(2);
  PyTuple_SET_ITEMS(
      ret, Vector_CreatePyObject(pt_out, pt_num, nullptr), PyFloat_FromDouble(lambda));
  return ret;
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_intersect_point_line_segment_doc,
    ".. function:: intersect_point_line_segment(pt, seg_p1, seg_p2, /)\n"
    "\n"
    "   Takes a point and a segment and returns the closest point on the segment "
    "and the distance to the segment.\n"
    "\n"
    "   :param pt: Point\n"
    "   :type pt: :class:`mathutils.Vector`\n"
    "   :param seg_p1: First point of the segment\n"
    "   :type seg_p1: :class:`mathutils.Vector`\n"
    "   :param seg_p2: Second point of the segment\n"
    "   :type seg_p2: :class:`mathutils.Vector`\n"
    "   :return: The closest point on the segment and the distance to the segment.\n"
    "   :rtype: tuple[:class:`mathutils.Vector`, float]\n");
static PyObject *M_Geometry_intersect_point_line_segment(PyObject * /*self*/,
                                                         PyObject *const *args,
                                                         Py_ssize_t nargs)
{
  const char *error_prefix = "intersect_point_line_segment";
  float pt[3], pt_out[3], seg_a[3], seg_b[3];
  int pt_num = 2;

  if (!_PyArg_CheckPositional(error_prefix, nargs, 3, 3)) {
    return nullptr;
  }

  PyObject *py_pt = args[0];
  PyObject *py_seq_a = args[1];
  PyObject *py_seg_b = args[2];

  /* Accept 2D verts. */
  if ((((pt_num = mathutils_array_parse(
             pt, 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_pt, error_prefix)) != -1) &&
       (mathutils_array_parse(
            seg_a, 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_seq_a, error_prefix) != -1) &&
       (mathutils_array_parse(
            seg_b, 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_seg_b, error_prefix) != -1)) == 0)
  {
    return nullptr;
  }

  /* Do the calculation. */
  closest_to_line_segment_v3(pt_out, pt, seg_a, seg_b);
  const float lambda = len_v3v3(pt_out, pt);

  PyObject *ret = PyTuple_New(2);
  PyTuple_SET_ITEMS(
      ret, Vector_CreatePyObject(pt_out, pt_num, nullptr), PyFloat_FromDouble(lambda));
  return ret;
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_intersect_point_tri_doc,
    ".. function:: intersect_point_tri(pt, tri_p1, tri_p2, tri_p3, /)\n"
    "\n"
    "   Takes 4 vectors: one is the point and the next 3 define the triangle. Projects "
    "the point onto the triangle plane and checks if it is within the triangle.\n"
    "\n"
    "   :param pt: Point\n"
    "   :type pt: :class:`mathutils.Vector`\n"
    "   :param tri_p1: First point of the triangle\n"
    "   :type tri_p1: :class:`mathutils.Vector`\n"
    "   :param tri_p2: Second point of the triangle\n"
    "   :type tri_p2: :class:`mathutils.Vector`\n"
    "   :param tri_p3: Third point of the triangle\n"
    "   :type tri_p3: :class:`mathutils.Vector`\n"
    "   :return: Point on the triangle's plane or None if it's outside the triangle\n"
    "   :rtype: :class:`mathutils.Vector` | None\n");
static PyObject *M_Geometry_intersect_point_tri(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "intersect_point_tri";
  PyObject *py_pt, *py_tri[3];
  float pt[3], tri[3][3];
  float vi[3];
  int i;

  if (!PyArg_ParseTuple(args,
                        "O" /* `pt` */
                        "O" /* `tri_p1` */
                        "O" /* `tri_p2` */
                        "O" /* `tri_p3` */
                        ":intersect_point_tri",
                        &py_pt,
                        UNPACK3_EX(&, py_tri, )))
  {
    return nullptr;
  }

  if (mathutils_array_parse(pt, 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_pt, error_prefix) == -1)
  {
    return nullptr;
  }
  for (i = 0; i < ARRAY_SIZE(tri); i++) {
    if (mathutils_array_parse(
            tri[i], 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_tri[i], error_prefix) == -1)
    {
      return nullptr;
    }
  }

  if (isect_point_tri_v3(pt, UNPACK3(tri), vi)) {
    return Vector_CreatePyObject(vi, 3, nullptr);
  }

  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_closest_point_on_tri_doc,
    ".. function:: closest_point_on_tri(pt, tri_p1, tri_p2, tri_p3, /)\n"
    "\n"
    "   Takes 4 vectors: one is the point and the next 3 define the triangle.\n"
    "\n"
    "   :param pt: Point\n"
    "   :type pt: :class:`mathutils.Vector`\n"
    "   :param tri_p1: First point of the triangle\n"
    "   :type tri_p1: :class:`mathutils.Vector`\n"
    "   :param tri_p2: Second point of the triangle\n"
    "   :type tri_p2: :class:`mathutils.Vector`\n"
    "   :param tri_p3: Third point of the triangle\n"
    "   :type tri_p3: :class:`mathutils.Vector`\n"
    "   :return: The closest point of the triangle.\n"
    "   :rtype: :class:`mathutils.Vector`\n");
static PyObject *M_Geometry_closest_point_on_tri(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "closest_point_on_tri";
  PyObject *py_pt, *py_tri[3];
  float pt[3], tri[3][3];
  float vi[3];
  int i;

  if (!PyArg_ParseTuple(args,
                        "O" /* `pt` */
                        "O" /* `tri_p1` */
                        "O" /* `tri_p2` */
                        "O" /* `tri_p3` */
                        ":closest_point_on_tri",
                        &py_pt,
                        UNPACK3_EX(&, py_tri, )))
  {
    return nullptr;
  }

  if (mathutils_array_parse(pt, 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_pt, error_prefix) == -1)
  {
    return nullptr;
  }
  for (i = 0; i < ARRAY_SIZE(tri); i++) {
    if (mathutils_array_parse(
            tri[i], 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_tri[i], error_prefix) == -1)
    {
      return nullptr;
    }
  }

  closest_on_tri_to_point_v3(vi, pt, UNPACK3(tri));

  return Vector_CreatePyObject(vi, 3, nullptr);
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_intersect_point_tri_2d_doc,
    ".. function:: intersect_point_tri_2d(pt, tri_p1, tri_p2, tri_p3, /)\n"
    "\n"
    "   Takes 4 vectors (using only the x and y coordinates): one is the point and the next 3 "
    "define the triangle. Returns a non-zero value if the point is within the triangle, otherwise "
    "0.\n"
    "\n"
    "   :param pt: Point\n"
    "   :type pt: :class:`mathutils.Vector`\n"
    "   :param tri_p1: First point of the triangle\n"
    "   :type tri_p1: :class:`mathutils.Vector`\n"
    "   :param tri_p2: Second point of the triangle\n"
    "   :type tri_p2: :class:`mathutils.Vector`\n"
    "   :param tri_p3: Third point of the triangle\n"
    "   :type tri_p3: :class:`mathutils.Vector`\n"
    "   :return: 1 if inside with CCW winding, -1 if inside with CW winding, otherwise 0.\n"
    "   :rtype: int\n");
static PyObject *M_Geometry_intersect_point_tri_2d(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "intersect_point_tri_2d";
  PyObject *py_pt, *py_tri[3];
  float pt[2], tri[3][2];
  int i;

  if (!PyArg_ParseTuple(args,
                        "O" /* `pt` */
                        "O" /* `tri_p1` */
                        "O" /* `tri_p2` */
                        "O" /* `tri_p3` */
                        ":intersect_point_tri_2d",
                        &py_pt,
                        UNPACK3_EX(&, py_tri, )))
  {
    return nullptr;
  }

  if (mathutils_array_parse(pt, 2, 2 | MU_ARRAY_SPILL, py_pt, error_prefix) == -1) {
    return nullptr;
  }
  for (i = 0; i < ARRAY_SIZE(tri); i++) {
    if (mathutils_array_parse(tri[i], 2, 2 | MU_ARRAY_SPILL, py_tri[i], error_prefix) == -1) {
      return nullptr;
    }
  }

  return PyLong_FromLong(isect_point_tri_v2(pt, UNPACK3(tri)));
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_intersect_point_quad_2d_doc,
    ".. function:: intersect_point_quad_2d(pt, quad_p1, quad_p2, quad_p3, quad_p4, /)\n"
    "\n"
    "   Takes 5 vectors (using only the x and y coordinates): one is the point and the "
    "next 4 define the quad,\n"
    "   only the x and y are used from the vectors. Returns a non-zero value if the point is "
    "within the quad, otherwise 0.\n"
    "   Works only with convex quads without singular edges.\n"
    "\n"
    "   :param pt: Point\n"
    "   :type pt: :class:`mathutils.Vector`\n"
    "   :param quad_p1: First point of the quad\n"
    "   :type quad_p1: :class:`mathutils.Vector`\n"
    "   :param quad_p2: Second point of the quad\n"
    "   :type quad_p2: :class:`mathutils.Vector`\n"
    "   :param quad_p3: Third point of the quad\n"
    "   :type quad_p3: :class:`mathutils.Vector`\n"
    "   :param quad_p4: Fourth point of the quad\n"
    "   :type quad_p4: :class:`mathutils.Vector`\n"
    "   :return: 1 if inside with CCW winding, -1 if inside with CW winding, otherwise 0.\n"
    "   :rtype: int\n");
static PyObject *M_Geometry_intersect_point_quad_2d(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "intersect_point_quad_2d";
  PyObject *py_pt, *py_quad[4];
  float pt[2], quad[4][2];
  int i;

  if (!PyArg_ParseTuple(args,
                        "O" /* `pt` */
                        "O" /* `quad_p1` */
                        "O" /* `quad_p2` */
                        "O" /* `quad_p3` */
                        "O" /* `quad_p4` */
                        ":intersect_point_quad_2d",
                        &py_pt,
                        UNPACK4_EX(&, py_quad, )))
  {
    return nullptr;
  }

  if (mathutils_array_parse(pt, 2, 2 | MU_ARRAY_SPILL, py_pt, error_prefix) == -1) {
    return nullptr;
  }
  for (i = 0; i < ARRAY_SIZE(quad); i++) {
    if (mathutils_array_parse(quad[i], 2, 2 | MU_ARRAY_SPILL, py_quad[i], error_prefix) == -1) {
      return nullptr;
    }
  }

  return PyLong_FromLong(isect_point_quad_v2(pt, UNPACK4(quad)));
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_distance_point_to_plane_doc,
    ".. function:: distance_point_to_plane(pt, plane_co, plane_no, /)\n"
    "\n"
    "   Returns the signed distance between a point and a plane "
    "(negative when below the normal).\n"
    "\n"
    "   :param pt: Point\n"
    "   :type pt: :class:`mathutils.Vector`\n"
    "   :param plane_co: A point on the plane\n"
    "   :type plane_co: :class:`mathutils.Vector`\n"
    "   :param plane_no: The direction the plane is facing\n"
    "   :type plane_no: :class:`mathutils.Vector`\n"
    "   :return: The signed distance.\n"
    "   :rtype: float\n");
static PyObject *M_Geometry_distance_point_to_plane(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "distance_point_to_plane";
  PyObject *py_pt, *py_plane_co, *py_plane_no;
  float pt[3], plane_co[3], plane_no[3];
  float plane[4];

  if (!PyArg_ParseTuple(args,
                        "O" /* `pt` */
                        "O" /* `plane_co` */
                        "O" /* `plane_no` */
                        ":distance_point_to_plane",
                        &py_pt,
                        &py_plane_co,
                        &py_plane_no))
  {
    return nullptr;
  }

  if (((mathutils_array_parse(pt, 3, 3 | MU_ARRAY_SPILL, py_pt, error_prefix) != -1) &&
       (mathutils_array_parse(plane_co, 3, 3 | MU_ARRAY_SPILL, py_plane_co, error_prefix) != -1) &&
       (mathutils_array_parse(plane_no, 3, 3 | MU_ARRAY_SPILL, py_plane_no, error_prefix) !=
        -1)) == 0)
  {
    return nullptr;
  }

  plane_from_point_normal_v3(plane, plane_co, plane_no);
  return PyFloat_FromDouble(dist_signed_to_plane_v3(pt, plane));
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_barycentric_transform_doc,
    ".. function:: barycentric_transform(point, tri_a1, tri_a2, tri_a3, tri_b1, tri_b2, tri_b3, "
    "/)\n"
    "\n"
    "   Return a transformed point, the transformation is defined by 2 triangles.\n"
    "\n"
    "   :param point: The point to transform.\n"
    "   :type point: :class:`mathutils.Vector`\n"
    "   :param tri_a1: source triangle vertex.\n"
    "   :type tri_a1: :class:`mathutils.Vector`\n"
    "   :param tri_a2: source triangle vertex.\n"
    "   :type tri_a2: :class:`mathutils.Vector`\n"
    "   :param tri_a3: source triangle vertex.\n"
    "   :type tri_a3: :class:`mathutils.Vector`\n"
    "   :param tri_b1: target triangle vertex.\n"
    "   :type tri_b1: :class:`mathutils.Vector`\n"
    "   :param tri_b2: target triangle vertex.\n"
    "   :type tri_b2: :class:`mathutils.Vector`\n"
    "   :param tri_b3: target triangle vertex.\n"
    "   :type tri_b3: :class:`mathutils.Vector`\n"
    "   :return: The transformed point\n"
    "   :rtype: :class:`mathutils.Vector`\n");
static PyObject *M_Geometry_barycentric_transform(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "barycentric_transform";
  PyObject *py_pt_src, *py_tri_src[3], *py_tri_dst[3];
  float pt_src[3], pt_dst[3], tri_src[3][3], tri_dst[3][3];
  int i;

  if (!PyArg_ParseTuple(args,
                        "O" /* `point` */
                        "O" /* `tri_a1` */
                        "O" /* `tri_a2` */
                        "O" /* `tri_a3` */
                        "O" /* `tri_b1` */
                        "O" /* `tri_b2` */
                        "O" /* `tri_b3` */
                        ":barycentric_transform",
                        &py_pt_src,
                        UNPACK3_EX(&, py_tri_src, ),
                        UNPACK3_EX(&, py_tri_dst, )))
  {
    return nullptr;
  }

  if (mathutils_array_parse(pt_src, 3, 3 | MU_ARRAY_SPILL, py_pt_src, error_prefix) == -1) {
    return nullptr;
  }
  for (i = 0; i < ARRAY_SIZE(tri_src); i++) {
    if (((mathutils_array_parse(tri_src[i], 3, 3 | MU_ARRAY_SPILL, py_tri_src[i], error_prefix) !=
          -1) &&
         (mathutils_array_parse(tri_dst[i], 3, 3 | MU_ARRAY_SPILL, py_tri_dst[i], error_prefix) !=
          -1)) == 0)
    {
      return nullptr;
    }
  }

  transform_point_by_tri_v3(pt_dst, pt_src, UNPACK3(tri_dst), UNPACK3(tri_src));

  return Vector_CreatePyObject(pt_dst, 3, nullptr);
}

struct PointsInPlanes_UserData {
  PyObject *py_verts;
  char *planes_used;
};

static void points_in_planes_fn(const float co[3], int i, int j, int k, void *user_data_p)
{
  PointsInPlanes_UserData *user_data = static_cast<PointsInPlanes_UserData *>(user_data_p);
  PyList_APPEND(user_data->py_verts, Vector_CreatePyObject(co, 3, nullptr));
  user_data->planes_used[i] = true;
  user_data->planes_used[j] = true;
  user_data->planes_used[k] = true;
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_points_in_planes_doc,
    ".. function:: points_in_planes(planes, epsilon_coplanar=1e-4, epsilon_isect=1e-6, /)\n"
    "\n"
    "   Returns a list of points inside all planes given and a list of index values for "
    "the planes used.\n"
    "\n"
    "   :param planes: List of planes (4D vectors).\n"
    "   :type planes: list[:class:`mathutils.Vector`]\n"
    "   :param epsilon_coplanar: Epsilon value for interpreting plane pairs as co-planar.\n"
    "   :type epsilon_coplanar: float\n"
    "   :param epsilon_isect: Epsilon value for intersection.\n"
    "   :type epsilon_isect: float\n"
    "   :return: Two lists, one containing the 3D coordinates inside the planes, "
    "another containing the plane indices used.\n"
    "   :rtype: tuple[list[:class:`mathutils.Vector`], list[int]]\n");
static PyObject *M_Geometry_points_in_planes(PyObject * /*self*/, PyObject *args)
{
  PyObject *py_planes;
  float (*planes)[4];
  float eps_coplanar = 1e-4f;
  float eps_isect = 1e-6f;
  uint planes_len;

  if (!PyArg_ParseTuple(args,
                        "O" /* `planes` */
                        "|" /* Optional arguments. */
                        "f" /* `epsilon_coplanar` */
                        "f" /* `epsilon_isect` */
                        ":points_in_planes",
                        &py_planes,
                        &eps_coplanar,
                        &eps_isect))
  {
    return nullptr;
  }

  if ((planes_len = mathutils_array_parse_alloc_v(
           reinterpret_cast<float **>(&planes), 4, py_planes, "points_in_planes")) == -1)
  {
    return nullptr;
  }

  /* NOTE: this could be refactored into plain C easy - py bits are noted. */

  PointsInPlanes_UserData user_data{};
  user_data.py_verts = PyList_New(0);
  user_data.planes_used = static_cast<char *>(PyMem_Malloc(sizeof(char) * planes_len));

  /* python */
  PyObject *py_plane_index = PyList_New(0);

  memset(user_data.planes_used, 0, sizeof(char) * planes_len);

  const bool has_isect = isect_planes_v3_fn(
      planes, planes_len, eps_coplanar, eps_isect, points_in_planes_fn, &user_data);
  PyMem_Free(planes);

  /* Now make user_data list of used planes. */
  if (has_isect) {
    for (int i = 0; i < planes_len; i++) {
      if (user_data.planes_used[i]) {
        PyList_APPEND(py_plane_index, PyLong_FromLong(i));
      }
    }
  }
  PyMem_Free(user_data.planes_used);

  {
    PyObject *ret = PyTuple_New(2);
    PyTuple_SET_ITEMS(ret, user_data.py_verts, py_plane_index);
    return ret;
  }
}

#ifndef MATH_STANDALONE

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_interpolate_bezier_doc,
    ".. function:: interpolate_bezier(knot1, handle1, handle2, knot2, resolution, /)\n"
    "\n"
    "   Interpolate a bezier spline segment.\n"
    "\n"
    "   :param knot1: First bezier spline point.\n"
    "   :type knot1: :class:`mathutils.Vector`\n"
    "   :param handle1: First bezier spline handle.\n"
    "   :type handle1: :class:`mathutils.Vector`\n"
    "   :param handle2: Second bezier spline handle.\n"
    "   :type handle2: :class:`mathutils.Vector`\n"
    "   :param knot2: Second bezier spline point.\n"
    "   :type knot2: :class:`mathutils.Vector`\n"
    "   :param resolution: Number of points to return.\n"
    "   :type resolution: int\n"
    "   :return: The interpolated points.\n"
    "   :rtype: list[:class:`mathutils.Vector`]\n");
static PyObject *M_Geometry_interpolate_bezier(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "interpolate_bezier";
  PyObject *py_data[4];
  float data[4][4] = {{0.0f}};
  int resolu;
  int dims = 0;
  int i;
  float *coord_array, *fp;
  PyObject *list;

  if (!PyArg_ParseTuple(args,
                        "O" /* `knot1` */
                        "O" /* `handle1` */
                        "O" /* `handle2` */
                        "O" /* `knot2` */
                        "i" /* `resolution` */
                        ":interpolate_bezier",
                        UNPACK4_EX(&, py_data, ),
                        &resolu))
  {
    return nullptr;
  }

  for (i = 0; i < 4; i++) {
    int dims_tmp;
    if ((dims_tmp = mathutils_array_parse(
             data[i], 2, 3 | MU_ARRAY_SPILL | MU_ARRAY_ZERO, py_data[i], error_prefix)) == -1)
    {
      return nullptr;
    }
    dims = max_ii(dims, dims_tmp);
  }

  if (resolu <= 1) {
    PyErr_SetString(PyExc_ValueError, "resolution must be 2 or over");
    return nullptr;
  }

  coord_array = MEM_new_array_zeroed<float>(size_t(dims) * size_t(resolu), error_prefix);
  for (i = 0; i < dims; i++) {
    BKE_curve_forward_diff_bezier(
        UNPACK4_EX(, data, [i]), coord_array + i, resolu - 1, sizeof(float) * dims);
  }

  list = PyList_New(resolu);
  fp = coord_array;
  for (i = 0; i < resolu; i++, fp = fp + dims) {
    PyList_SET_ITEM(list, i, Vector_CreatePyObject(fp, dims, nullptr));
  }
  MEM_delete(coord_array);
  return list;
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_tessellate_polygon_doc,
    ".. function:: tessellate_polygon(polylines, /)\n"
    "\n"
    "   Takes a list of polylines (each point a pair or triplet of numbers) and returns "
    "the point indices for a polyline filled with triangles. Degenerate (zero area) input "
    "yields no triangles. Self-intersecting outlines are not supported: since the result "
    "only refers to the input points, triangles that would require the edge intersection "
    "points (which are not part of the input) are omitted.\n"
    "\n"
    "   :param polylines: Polygons where each polygon is a sequence of 2D or 3D points.\n"
    "   :type polylines: Sequence[Sequence[Sequence[float]]]\n"
    "   :return: A list of triangles.\n"
    "   :rtype: list[tuple[int, int, int]]\n");
/* Fills multiple poly lines using the robust exact-predicate Constrained Delaunay
 * Triangulation solver. The returned triangles reference the original input point
 * indices (via the CDT output-to-input vertex map), keeping the long-standing contract.
 * The legacy scan-fill path this replaced could loop on near-collinear vertices at large
 * coordinates and emit degenerate triangles, see: #160745. */
static PyObject *M_Geometry_tessellate_polygon(PyObject * /*self*/, PyObject *polyLineSeq)
{
  if (!PySequence_Check(polyLineSeq)) {
    PyErr_SetString(PyExc_TypeError, "expected a sequence of poly lines");
    return nullptr;
  }

  const int len_polylines = PySequence_Size(polyLineSeq);

  /* Flatten all polyline points into a single list. A point's position in this list is
   * the index reported back to the caller, so the input order must be preserved. */
  Vector<float3> coords;
  Vector<int> poly_counts;
  bool is_2d = true;

  for (int i = 0; i < len_polylines; i++) {
    PyObject *polyLine = PySequence_GetItem(polyLineSeq, i);
    if (!PySequence_Check(polyLine)) {
      Py_XDECREF(polyLine); /* May be null so use #Py_XDECREF. */
      PyErr_SetString(PyExc_TypeError,
                      "One or more of the polylines is not a sequence of mathutils.Vector's");
      return nullptr;
    }

    const int len_polypoints = PySequence_Size(polyLine);
    if (len_polypoints > 0) { /* Don't bother adding edges as polylines. */
      poly_counts.append(len_polypoints);
      for (int index = 0; index < len_polypoints; index++) {
        PyObject *polyVec = PySequence_GetItem(polyLine, index);
        float co[3];
        const int polyVec_len = mathutils_array_parse(
            co, 2, 3 | MU_ARRAY_SPILL, polyVec, "tessellate_polygon: parse coord");
        Py_DECREF(polyVec);

        if (polyVec_len == -1) [[unlikely]] {
          Py_DECREF(polyLine);
          return nullptr;
        }
        if (polyVec_len == 2) {
          co[2] = 0.0f;
        }
        else if (polyVec_len == 3) {
          is_2d = false;
        }
        coords.append(float3(co[0], co[1], co[2]));
      }
    }
    Py_DECREF(polyLine);
  }

  const int totpoints = int(coords.size());
  if (totpoints == 0) {
    /* No points, do this so scripts don't barf. */
    return PyList_New(0);
  }

  /* The CDT solver works in 2D, so project the points onto a plane first: -Z for 2D
   * input, the best-fit plane (Newell's method) for 3D. This matches the projection the
   * previous scan-fill path used, which also keeps the output triangle winding
   * consistent with what this function historically returned. */
  float n[3] = {0.0f, 0.0f, -1.0f};
  if (!is_2d) {
    zero_v3(n);
    const float *v_prev = coords.last();
    for (const float3 &co : coords) {
      add_newell_cross_v3_v3v3(n, v_prev, co);
      v_prev = co;
    }
    if (normalize_v3(n) == 0.0f) {
      /* Degenerate input (all points collinear or coincident): any projection yields
       * zero area and no triangles, use the -Z default. */
      n[0] = n[1] = 0.0f;
      n[2] = -1.0f;
    }
  }
  float mat_2d[3][3];
  axis_dominant_v3_to_m3_negate(mat_2d, n);

  Array<double2> verts_2d(totpoints);
  for (const int i : coords.index_range()) {
    float xy[2];
    mul_v2_m3v3(xy, mat_2d, coords[i]);
    verts_2d[i] = double2(xy[0], xy[1]);
  }

  /* One CDT face per polyline. Because the faces are built in the same order the points
   * were flattened, and each face spans a contiguous block, the face-vertex indices are
   * simply the identity mapping into the flat vertex list. */
  Array<int> face_offset_data(poly_counts.size() + 1);
  for (const int p : poly_counts.index_range()) {
    face_offset_data[p] = poly_counts[p];
  }
  const OffsetIndices<int> face_offsets = offset_indices::accumulate_counts_to_offsets(
      face_offset_data);

  Array<int> face_vert_indices(totpoints);
  for (const int i : IndexRange(totpoints)) {
    face_vert_indices[i] = i;
  }

  meshintersect::CDT_input<double> input;
  input.vert = verts_2d;
  input.face_offsets = face_offsets;
  input.face_vert_indices = face_vert_indices;
  input.epsilon = 1e-8;
  input.need_ids = true;

  const meshintersect::CDT_result<double> result = meshintersect::delaunay_2d_calc(
      input, CDT_INSIDE_WITH_HOLES);

  /* Map each output triangle back to original input point indices. For valid (non
   * self-intersecting) input, every output vertex has exactly one input origin. */
  Vector<int3> tris;
  tris.reserve(result.face.size());
  for (const Vector<int> &face : result.face) {
    BLI_assert(face.size() == 3);
    int3 tri;
    bool ok = true;
    for (int j = 0; j < 3; j++) {
      const Vector<uint32_t> &orig = result.vert_orig[face[j]];
      if (orig.is_empty()) [[unlikely]] {
        /* Intersection vertex from self-intersecting input has no input index. */
        ok = false;
        break;
      }
      tri[j] = int(orig[0]);
    }
    if (ok) {
      /* Swap the last two indices: CDT emits faces with the opposite orientation to the
       * legacy scan-fill output in the projected space, keep the winding this function
       * historically returned (#displist_fill_cdt_process_group does the same). */
      tris.append({tri[0], tri[2], tri[1]});
    }
  }

  PyObject *tri_list = PyList_New(tris.size());
  if (!tri_list) {
    PyErr_SetString(PyExc_RuntimeError, "failed to make a new list");
    return nullptr;
  }
  for (const int i : tris.index_range()) {
    PyList_SET_ITEM(tri_list, i, PyC_Tuple_Pack_I32({tris[i][0], tris[i][1], tris[i][2]}));
  }
  return tri_list;
}

static int boxPack_FromPyObject(PyObject *value, BoxPack **r_boxarray)
{
  Py_ssize_t len, i;
  PyObject *list_item, *item_1, *item_2;
  BoxPack *boxarray;

  /* Error checking must already be done */
  if (!PyList_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "can only back a list of [x, y, w, h]");
    return -1;
  }

  len = PyList_GET_SIZE(value);

  boxarray = MEM_new_array_uninitialized<BoxPack>(size_t(len), __func__);

  for (i = 0; i < len; i++) {
    list_item = PyList_GET_ITEM(value, i);
    if (!PyList_Check(list_item) || PyList_GET_SIZE(list_item) < 4) {
      MEM_delete(boxarray);
      PyErr_SetString(PyExc_TypeError, "can only pack a list of [x, y, w, h]");
      return -1;
    }

    BoxPack *box = &boxarray[i];

    item_1 = PyList_GET_ITEM(list_item, 2);
    item_2 = PyList_GET_ITEM(list_item, 3);

    box->w = float(PyFloat_AsDouble(item_1));
    box->h = float(PyFloat_AsDouble(item_2));
    box->index = i;

    /* accounts for error case too and overwrites with own error */
    if (box->w < 0.0f || box->h < 0.0f) {
      MEM_delete(boxarray);
      PyErr_SetString(PyExc_TypeError,
                      "error parsing width and height values from list: "
                      "[x, y, w, h], not numbers or below zero");
      return -1;
    }

    /* verts will be added later */
  }

  *r_boxarray = boxarray;
  return 0;
}

static void boxPack_ToPyObject(PyObject *value, const BoxPack *boxarray)
{
  Py_ssize_t len, i;
  PyObject *list_item;

  len = PyList_GET_SIZE(value);

  for (i = 0; i < len; i++) {
    const BoxPack *box = &boxarray[i];
    list_item = PyList_GET_ITEM(value, box->index);
    PyList_SetItem(list_item, 0, PyFloat_FromDouble(box->x));
    PyList_SetItem(list_item, 1, PyFloat_FromDouble(box->y));
  }
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_box_pack_2d_doc,
    ".. function:: box_pack_2d(boxes, /)\n"
    "\n"
    "   Returns a tuple with the width and height of the packed bounding box.\n"
    "\n"
    "   :param boxes: list of boxes, each box is a list where the first 4 items are "
    "[X, Y, width, height, ...] other items are ignored. "
    "The X & Y values in this list are modified to set the packed positions.\n"
    "   :type boxes: list[list[float]]\n"
    "   :return: The width and height of the packed bounding box.\n"
    "   :rtype: tuple[float, float]\n");
static PyObject *M_Geometry_box_pack_2d(PyObject * /*self*/, PyObject *boxlist)
{
  float tot_width = 0.0f, tot_height = 0.0f;
  Py_ssize_t len;

  PyObject *ret;

  if (!PyList_Check(boxlist)) {
    PyErr_SetString(PyExc_TypeError, "expected a list of boxes [[x, y, w, h], ... ]");
    return nullptr;
  }

  len = PyList_GET_SIZE(boxlist);
  if (len) {
    BoxPack *boxarray = nullptr;
    if (boxPack_FromPyObject(boxlist, &boxarray) == -1) {
      return nullptr; /* exception set */
    }

    const bool sort_boxes = true; /* Caution: BLI_box_pack_2d sorting is non-deterministic. */
    /* Non Python function */
    BLI_box_pack_2d(boxarray, len, sort_boxes, &tot_width, &tot_height);

    boxPack_ToPyObject(boxlist, boxarray);
    MEM_delete(boxarray);
  }

  ret = PyTuple_New(2);
  PyTuple_SET_ITEMS(ret, PyFloat_FromDouble(tot_width), PyFloat_FromDouble(tot_height));
  return ret;
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_box_fit_2d_doc,
    ".. function:: box_fit_2d(points, /)\n"
    "\n"
    "   Returns an angle that best fits the points to an axis aligned rectangle\n"
    "\n"
    "   :param points: Sequence of 2D points.\n"
    "   :type points: Sequence[Sequence[float]]\n"
    "   :return: The rotation angle in radians for the best axis-aligned bounding box fit.\n"
    "   :rtype: float\n");
static PyObject *M_Geometry_box_fit_2d(PyObject * /*self*/, PyObject *pointlist)
{
  float (*points)[2];
  Py_ssize_t len;

  float angle = 0.0f;

  len = mathutils_array_parse_alloc_v(
      (reinterpret_cast<float **>(&points)), 2, pointlist, "box_fit_2d");
  if (len == -1) {
    return nullptr;
  }

  if (len) {
    /* Non Python function */
    angle = BLI_convexhull_aabb_fit_points_2d({reinterpret_cast<float2 *>(points), len});

    PyMem_Free(points);
  }

  return PyFloat_FromDouble(angle);
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_convex_hull_2d_doc,
    ".. function:: convex_hull_2d(points, /)\n"
    "\n"
    "   Returns the indices of the points forming the convex hull, in counter-clockwise order.\n"
    "\n"
    "   :param points: Sequence of 2D points.\n"
    "   :type points: Sequence[Sequence[float]]\n"
    "   :return: Indices of convex hull vertices in counter-clockwise order.\n"
    "   :rtype: list[int]\n");
static PyObject *M_Geometry_convex_hull_2d(PyObject * /*self*/, PyObject *pointlist)
{
  float (*points)[2];
  Py_ssize_t len;

  PyObject *ret;

  len = mathutils_array_parse_alloc_v(
      (reinterpret_cast<float **>(&points)), 2, pointlist, "convex_hull_2d");
  if (len == -1) {
    return nullptr;
  }

  if (len) {
    int *index_map;
    Py_ssize_t len_ret, i;

    index_map = MEM_new_array_uninitialized<int>(size_t(len), __func__);

    /* Non Python function */
    len_ret = BLI_convexhull_2d({reinterpret_cast<float2 *>(points), len}, index_map);

    ret = PyList_New(len_ret);
    for (i = 0; i < len_ret; i++) {
      PyList_SET_ITEM(ret, i, PyLong_FromLong(index_map[i]));
    }

    MEM_delete(index_map);

    PyMem_Free(points);
  }
  else {
    ret = PyList_New(0);
  }

  return ret;
}

/* Return a PyObject that is a list of lists, using the flattened list array
 * to fill values, with start_table and len_table giving the start index
 * and length of the toplevel_len sub-lists.
 */
template<typename T> static PyObject *list_of_lists_from_arrays(const Span<Vector<T>> data)
{
  if (data.is_empty()) {
    return PyList_New(0);
  }
  PyObject *ret = PyList_New(data.size());
  for (const int i : data.index_range()) {
    const Span<T> group = data[i];
    PyObject *sublist = PyList_New(group.size());
    for (const int j : group.index_range()) {
      PyList_SET_ITEM(sublist, j, PyLong_FromLong(group[j]));
    }
    PyList_SET_ITEM(ret, i, sublist);
  }
  return ret;
}

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_delaunay_2d_cdt_doc,
    ".. function:: delaunay_2d_cdt(vert_coords, edges, faces, output_type, epsilon, "
    "need_ids=True, /)\n"
    "\n"
    "   Computes the Constrained Delaunay Triangulation of a set of vertices,\n"
    "   with edges and faces that must appear in the triangulation.\n"
    "   Some triangles may be eaten away, or combined with other triangles,\n"
    "   according to output type.\n"
    "   The returned verts may be in a different order from input verts, may be moved\n"
    "   slightly, and may be merged with other nearby verts.\n"
    "   The three returned orig lists give, for each of verts, edges, and faces, the list of\n"
    "   input element indices corresponding to the positionally same output element.\n"
    "   For edges, the orig indices start with the input edges and then continue\n"
    "   with the edges implied by each of the faces (n of them for an n-gon).\n"
    "   If the need_ids argument is supplied, and False, then the code skips the preparation\n"
    "   of the orig arrays, which may save some time.\n"
    "\n"
    "   :param vert_coords: Vertex coordinates (2d)\n"
    "   :type vert_coords: Sequence[:class:`mathutils.Vector`]\n"
    "   :param edges: Edges, as pairs of indices in ``vert_coords``\n"
    "   :type edges: Sequence[tuple[int, int]]\n"
    "   :param faces: Faces, each sublist is a face, "
    "as indices in ``vert_coords`` (CCW oriented).\n"
    "   :type faces: Sequence[Sequence[int]]\n"
    "   :param output_type: What output looks like. 0 => triangles with convex hull. "
    "1 => triangles inside constraints. "
    "2 => the input constraints, intersected. "
    "3 => like 2 but detect holes and omit them from output. "
    "4 => like 2 but with extra edges to make valid BMesh faces. "
    "5 => like 4 but detect holes and omit them from output.\n"
    "   :type output_type: int\n"
    "   :param epsilon: For nearness tests; should not be zero\n"
    "   :type epsilon: float\n"
    "   :param need_ids: are the orig output arrays needed?\n"
    "   :type need_ids: bool\n"
    "   :return: Output tuple, (vert_coords, edges, faces, orig_verts, orig_edges, orig_faces)\n"
    "   :rtype: tuple["
    "list[:class:`mathutils.Vector`], "
    "list[tuple[int, int]], "
    "list[list[int]], "
    "list[list[int]], "
    "list[list[int]], "
    "list[list[int]]]\n");
static PyObject *M_Geometry_delaunay_2d_cdt(PyObject * /*self*/, PyObject *args)
{
  const char *error_prefix = "delaunay_2d_cdt";
  PyObject *vert_coords, *edges, *faces;
  int output_type;
  float epsilon;
  bool need_ids = true;
  float (*in_coords)[2] = nullptr;
  int (*in_edges)[2] = nullptr;
  Py_ssize_t vert_coords_len, edges_len;
  PyObject *out_vert_coords = nullptr;
  PyObject *out_edges = nullptr;
  PyObject *out_faces = nullptr;
  PyObject *out_orig_verts = nullptr;
  PyObject *out_orig_edges = nullptr;
  PyObject *out_orig_faces = nullptr;
  PyObject *ret_value = nullptr;

  if (!PyArg_ParseTuple(args,
                        "O"  /* `vert_coords` */
                        "O"  /* `edges` */
                        "O"  /* `faces` */
                        "i"  /* `output_type` */
                        "f"  /* `epsilon` */
                        "|"  /* Optional arguments. */
                        "O&" /* `need_ids` */
                        ":delaunay_2d_cdt",
                        &vert_coords,
                        &edges,
                        &faces,
                        &output_type,
                        &epsilon,
                        PyC_ParseBool,
                        &need_ids))
  {
    return nullptr;
  }

  BLI_SCOPED_DEFER([&]() {
    if (in_coords != nullptr) {
      PyMem_Free(in_coords);
    }
    if (in_edges != nullptr) {
      PyMem_Free(in_edges);
    }
  });

  vert_coords_len = mathutils_array_parse_alloc_v(
      reinterpret_cast<float **>(&in_coords), 2, vert_coords, error_prefix);
  if (vert_coords_len == -1) {
    return nullptr;
  }

  edges_len = mathutils_array_parse_alloc_vi(
      reinterpret_cast<int **>(&in_edges), 2, edges, error_prefix);
  if (edges_len == -1) {
    return nullptr;
  }

  Array<int> face_offsets;
  Array<int> face_vert_indices;
  if (!mathutils_array_parse_alloc_viseq(faces, error_prefix, face_offsets, face_vert_indices)) {
    return nullptr;
  }

  Array<double2> verts(vert_coords_len);
  for (const int i : verts.index_range()) {
    verts[i] = {double(in_coords[i][0]), double(in_coords[i][1])};
  }

  meshintersect::CDT_input<double> in;
  in.vert = verts;
  in.edge = Span(reinterpret_cast<int2 *>(in_edges), edges_len);
  in.face_offsets = face_offsets.as_span();
  in.face_vert_indices = face_vert_indices;
  in.epsilon = epsilon;
  in.need_ids = need_ids;

  const meshintersect::CDT_result<double> res = meshintersect::delaunay_2d_calc(
      in, CDT_output_type(output_type));

  ret_value = PyTuple_New(6);

  out_vert_coords = PyList_New(res.vert.size());
  for (const int i : res.vert.index_range()) {
    const float2 vert_float(res.vert[i]);
    PyObject *item = Vector_CreatePyObject(vert_float, 2, nullptr);
    if (item == nullptr) {
      Py_DECREF(ret_value);
      Py_DECREF(out_vert_coords);
      return nullptr;
    }
    PyList_SET_ITEM(out_vert_coords, i, item);
  }
  PyTuple_SET_ITEM(ret_value, 0, out_vert_coords);

  out_edges = PyList_New(res.edge.size());
  for (const int i : res.edge.index_range()) {
    PyObject *item = PyTuple_New(2);
    PyTuple_SET_ITEM(item, 0, PyLong_FromLong(long(res.edge[i][0])));
    PyTuple_SET_ITEM(item, 1, PyLong_FromLong(long(res.edge[i][1])));
    PyList_SET_ITEM(out_edges, i, item);
  }
  PyTuple_SET_ITEM(ret_value, 1, out_edges);

  out_faces = list_of_lists_from_arrays(res.face.as_span());
  PyTuple_SET_ITEM(ret_value, 2, out_faces);

  out_orig_verts = list_of_lists_from_arrays(res.vert_orig.as_span());
  PyTuple_SET_ITEM(ret_value, 3, out_orig_verts);

  out_orig_edges = list_of_lists_from_arrays(res.edge_orig.as_span());
  PyTuple_SET_ITEM(ret_value, 4, out_orig_edges);

  out_orig_faces = list_of_lists_from_arrays(res.face_orig.as_span());
  PyTuple_SET_ITEM(ret_value, 5, out_orig_faces);

  return ret_value;
}

#endif /* MATH_STANDALONE */

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wcast-function-type"
#  else
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wcast-function-type"
#  endif
#endif

static PyMethodDef M_Geometry_methods[] = {
    {"intersect_ray_tri",
     static_cast<PyCFunction>(M_Geometry_intersect_ray_tri),
     METH_VARARGS,
     M_Geometry_intersect_ray_tri_doc},
    {"intersect_point_line",
     reinterpret_cast<PyCFunction>(M_Geometry_intersect_point_line),
     METH_FASTCALL,
     M_Geometry_intersect_point_line_doc},
    {"intersect_point_line_segment",
     reinterpret_cast<PyCFunction>(M_Geometry_intersect_point_line_segment),
     METH_FASTCALL,
     M_Geometry_intersect_point_line_segment_doc},
    {"intersect_point_tri",
     static_cast<PyCFunction>(M_Geometry_intersect_point_tri),
     METH_VARARGS,
     M_Geometry_intersect_point_tri_doc},
    {"closest_point_on_tri",
     static_cast<PyCFunction>(M_Geometry_closest_point_on_tri),
     METH_VARARGS,
     M_Geometry_closest_point_on_tri_doc},
    {"intersect_point_tri_2d",
     static_cast<PyCFunction>(M_Geometry_intersect_point_tri_2d),
     METH_VARARGS,
     M_Geometry_intersect_point_tri_2d_doc},
    {"intersect_point_quad_2d",
     static_cast<PyCFunction>(M_Geometry_intersect_point_quad_2d),
     METH_VARARGS,
     M_Geometry_intersect_point_quad_2d_doc},
    {"intersect_line_line",
     static_cast<PyCFunction>(M_Geometry_intersect_line_line),
     METH_VARARGS,
     M_Geometry_intersect_line_line_doc},
    {"intersect_line_line_2d",
     static_cast<PyCFunction>(M_Geometry_intersect_line_line_2d),
     METH_VARARGS,
     M_Geometry_intersect_line_line_2d_doc},
    {"intersect_line_plane",
     static_cast<PyCFunction>(M_Geometry_intersect_line_plane),
     METH_VARARGS,
     M_Geometry_intersect_line_plane_doc},
    {"intersect_plane_plane",
     static_cast<PyCFunction>(M_Geometry_intersect_plane_plane),
     METH_VARARGS,
     M_Geometry_intersect_plane_plane_doc},
    {"intersect_line_sphere",
     static_cast<PyCFunction>(M_Geometry_intersect_line_sphere),
     METH_VARARGS,
     M_Geometry_intersect_line_sphere_doc},
    {"intersect_line_sphere_2d",
     static_cast<PyCFunction>(M_Geometry_intersect_line_sphere_2d),
     METH_VARARGS,
     M_Geometry_intersect_line_sphere_2d_doc},
    {"distance_point_to_plane",
     static_cast<PyCFunction>(M_Geometry_distance_point_to_plane),
     METH_VARARGS,
     M_Geometry_distance_point_to_plane_doc},
    {"intersect_sphere_sphere_2d",
     static_cast<PyCFunction>(M_Geometry_intersect_sphere_sphere_2d),
     METH_VARARGS,
     M_Geometry_intersect_sphere_sphere_2d_doc},
    {"intersect_tri_tri_2d",
     static_cast<PyCFunction>(M_Geometry_intersect_tri_tri_2d),
     METH_VARARGS,
     M_Geometry_intersect_tri_tri_2d_doc},
    {"area_tri",
     static_cast<PyCFunction>(M_Geometry_area_tri),
     METH_VARARGS,
     M_Geometry_area_tri_doc},
    {"volume_tetrahedron",
     static_cast<PyCFunction>(M_Geometry_volume_tetrahedron),
     METH_VARARGS,
     M_Geometry_volume_tetrahedron_doc},
    {"normal", static_cast<PyCFunction>(M_Geometry_normal), METH_VARARGS, M_Geometry_normal_doc},
    {"barycentric_transform",
     static_cast<PyCFunction>(M_Geometry_barycentric_transform),
     METH_VARARGS,
     M_Geometry_barycentric_transform_doc},
    {"points_in_planes",
     static_cast<PyCFunction>(M_Geometry_points_in_planes),
     METH_VARARGS,
     M_Geometry_points_in_planes_doc},
#ifndef MATH_STANDALONE
    {"interpolate_bezier",
     static_cast<PyCFunction>(M_Geometry_interpolate_bezier),
     METH_VARARGS,
     M_Geometry_interpolate_bezier_doc},
    {"tessellate_polygon",
     static_cast<PyCFunction>(M_Geometry_tessellate_polygon),
     METH_O,
     M_Geometry_tessellate_polygon_doc},
    {"convex_hull_2d",
     static_cast<PyCFunction>(M_Geometry_convex_hull_2d),
     METH_O,
     M_Geometry_convex_hull_2d_doc},
    {"delaunay_2d_cdt",
     static_cast<PyCFunction>(M_Geometry_delaunay_2d_cdt),
     METH_VARARGS,
     M_Geometry_delaunay_2d_cdt_doc},
    {"box_fit_2d",
     static_cast<PyCFunction>(M_Geometry_box_fit_2d),
     METH_O,
     M_Geometry_box_fit_2d_doc},
    {"box_pack_2d",
     static_cast<PyCFunction>(M_Geometry_box_pack_2d),
     METH_O,
     M_Geometry_box_pack_2d_doc},
#endif
    {nullptr, nullptr, 0, nullptr},
};

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic pop
#  else
#    pragma GCC diagnostic pop
#  endif
#endif

PyDoc_STRVAR(
    /* Wrap. */
    M_Geometry_doc,
    "The Blender geometry module.");
static PyModuleDef M_Geometry_module_def = {
    /*m_base*/ PyModuleDef_HEAD_INIT,
    /*m_name*/ "mathutils.geometry",
    /*m_doc*/ M_Geometry_doc,
    /*m_size*/ 0,
    /*m_methods*/ M_Geometry_methods,
    /*m_slots*/ nullptr,
    /*m_traverse*/ nullptr,
    /*m_clear*/ nullptr,
    /*m_free*/ nullptr,
};

/*----------------------------MODULE INIT-------------------------*/

PyMODINIT_FUNC PyInit_mathutils_geometry()
{
  PyObject *submodule = PyModule_Create(&M_Geometry_module_def);
  return submodule;
}

}  // namespace blender
