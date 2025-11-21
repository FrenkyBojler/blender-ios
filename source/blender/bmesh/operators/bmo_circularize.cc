/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 */

#include "bmesh.hh"
#include "intern/bmesh_operators_private.hh" /* own include */

/* Holds data for a vertex projected onto the local plane. */
struct CircleVert {
  BMVert *v;
  /* Current postion on the plane. */
  float co_2d[2];
  /* Where it should move to on the circle. */
  float target_2d[2];
};

/* Stores the geometry loop and whether it forms a closed circle or open chain. */
struct LoopData {
  blender::Vector<BMVert *> verts;
  bool is_closed;
};

void bmo_circularize_exec(BMesh * /*bm*/, BMOperator * /*op*/) {}
