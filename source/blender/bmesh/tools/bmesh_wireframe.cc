/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * Creates a solid wireframe from connected faces.
 */

#include <algorithm>

#include "MEM_guardedalloc.h"

#include "bmesh.hh"

#include "BLI_listbase.h"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"

#include "BKE_customdata.hh"
#include "BKE_deform.hh"

#include "bmesh_wireframe.hh"

struct JunctionRing {
  struct JunctionRing *next, *prev;
  BMVert **verts;
  int segments;
};

static GHash *junction_map = nullptr;

/* Add a circular vertex ring to the junction map for a given vertex. */
static void add_ring_to_junction(GHash *junction_map, BMVert *v, BMVert **ring, int segments)
{
  ListBase *rings = (ListBase *)BLI_ghash_lookup(junction_map, v);
  if (rings == nullptr) {
    rings = (ListBase *)MEM_callocN(sizeof(ListBase), __func__);
    BLI_ghash_insert(junction_map, v, rings);
  }

  JunctionRing *jr = (JunctionRing *)MEM_callocN(sizeof(JunctionRing), __func__);
  jr->verts = ring;
  jr->segments = segments;

  BLI_addtail(rings, jr);
}

/* Free all rings stored in a junction list. */
static void free_junction_list(void *val)
{
  ListBase *rings = (ListBase *)val;
  for (JunctionRing *jr = (JunctionRing *)rings->first; jr;) {
    JunctionRing *next = jr->next;
    if (jr->verts) {
      MEM_freeN(jr->verts);
    }
    MEM_freeN(jr);
    jr = next;
  }
  MEM_freeN(rings);
}

/* Compute average polygon normal vector of a ring around a junction center. */
static void ring_axis_from_center(BMVert **ring,
                                  const int segments,
                                  const float co_center[3],
                                  float r_axis[3])
{
  zero_v3(r_axis);
  for (int i = 0; i < segments; i++) {
    const int ni = (i + 1) % segments;
    float a[3], b[3], n[3];
    sub_v3_v3v3(a, ring[i]->co, co_center);
    sub_v3_v3v3(b, ring[ni]->co, co_center);
    cross_v3_v3v3(n, a, b);
    add_v3_v3(r_axis, n);
  }
  normalize_v3(r_axis);
}

/* Build two perpendicular tangent vectors from a given normal. */
static void frame_from_normal(const float n[3], float t1[3], float t2[3])
{
  float h[3] = {1.0f, 0.0f, 0.0f};
  if (fabsf(dot_v3v3(h, n)) > 0.9f) {
    h[0] = 0.0f;
    h[1] = 1.0f;
    h[2] = 0.0f;
  }
  cross_v3_v3v3(t1, n, h);
  normalize_v3(t1);
  cross_v3_v3v3(t2, n, t1);
  normalize_v3(t2);
}

/* Find best cyclic shift to align two rings. */
static int best_ring_shift(BMVert **ringA,
                           BMVert **ringB,
                           const int segments,
                           const float co_center[3])
{
  float(*dirA)[3] = static_cast<float(*)[3]>(MEM_mallocN(sizeof(float[3]) * segments, __func__));
  float(*dirB)[3] = static_cast<float(*)[3]>(MEM_mallocN(sizeof(float[3]) * segments, __func__));

  for (int i = 0; i < segments; i++) {
    sub_v3_v3v3(dirA[i], ringA[i]->co, co_center);
    normalize_v3(dirA[i]);
    sub_v3_v3v3(dirB[i], ringB[i]->co, co_center);
    normalize_v3(dirB[i]);
  }

  int best_k = 0;
  float best_score = -FLT_MAX;
  for (int k = 0; k < segments; k++) {
    float score = 0.0f;
    for (int i = 0; i < segments; i++) {
      score += dot_v3v3(dirA[i], dirB[(i + k) % segments]);
    }
    if (score > best_score) {
      best_score = score;
      best_k = k;
    }
  }

  MEM_freeN(dirA);
  MEM_freeN(dirB);
  return best_k;
}

/* Create an inner cap ring of verts on a sphere around the junction center. */
static BMVert **make_cap_ring_on_sphere(
    BMesh *bm, BMVert **ring, const int segments, const float co_center[3], const float radius)
{
  BMVert **cap = (BMVert **)MEM_mallocN(sizeof(BMVert *) * segments, __func__);
  for (int i = 0; i < segments; i++) {
    float d[3];
    sub_v3_v3v3(d, ring[i]->co, co_center);
    if (normalize_v3(d) == 0.0f) {
      copy_v3_v3(d, ring[i]->no);
    }
    float p[3];
    madd_v3_v3v3fl(p, co_center, d, radius);
    cap[i] = BM_vert_create(bm, p, nullptr, BM_CREATE_NOP);
  }
  return cap;
}

/* Connect two rings into a strip of quads, with a given cyclic shift. */
static void stitch_rings_with_shift(
    BMesh *bm, BMVert **ringA, BMVert **ringB, const int segments, const int shiftB)
{
  for (int i = 0; i < segments; i++) {
    const int ni = (i + 1) % segments;
    const int bi = (i + shiftB) % segments;
    const int bni = (ni + shiftB) % segments;
    BMFace *f = BM_face_create_quad_tri(
        bm, ringA[i], ringA[ni], ringB[bni], ringB[bi], nullptr, BM_CREATE_NOP);
    if (f) {
      BM_elem_flag_enable(f, BM_ELEM_TAG);
    }
  }
}

/* Build caps at junction vertices by stitching all attached rings together. */
static void build_vertex_junction_caps(BMesh *bm, GHash *junction_map)
{
  if (!junction_map) {
    return;
  }

  const float cap_scale = 1.0f;

  GHashIterator gh_iter;
  GHASH_ITER (gh_iter, junction_map) {
    BMVert *vcenter = (BMVert *)BLI_ghashIterator_getKey(&gh_iter);
    ListBase *rings = (ListBase *)BLI_ghashIterator_getValue(&gh_iter);
    if (!rings || !rings->first) {
      continue;
    }

    int ring_count = 0;
    for (JunctionRing *jr = (JunctionRing *)rings->first; jr; jr = jr->next) {
      ring_count++;
    }
    if (ring_count == 0) {
      continue;
    }

    typedef struct RingCap {
      JunctionRing *jr;
      BMVert **cap;
      float axis[3];
      float angle;
      float tube_radius;
    } RingCap;

    RingCap *rc = (RingCap *)MEM_callocN(sizeof(RingCap) * ring_count, __func__);

    float sortN[3] = {0, 0, 0};

    int index = 0;
    for (JunctionRing *jr = (JunctionRing *)rings->first; jr; jr = jr->next, index++) {
      rc[index].jr = jr;

      ring_axis_from_center(jr->verts, jr->segments, vcenter->co, rc[index].axis);
      add_v3_v3(sortN, rc[index].axis);

      float d[3];
      sub_v3_v3v3(d, jr->verts[0]->co, vcenter->co);
      rc[index].tube_radius = len_v3(d);
    }

    if (normalize_v3(sortN) == 0.0f) {
      sortN[2] = 1.0f;
    }
    float T1[3], T2[3];
    frame_from_normal(sortN, T1, T2);

    index = 0;
    for (JunctionRing *jr = (JunctionRing *)rings->first; jr; jr = jr->next, index++) {
      const int S = jr->segments;

      const float cap_r = rc[index].tube_radius * cap_scale;
      rc[index].cap = make_cap_ring_on_sphere(bm, jr->verts, S, vcenter->co, cap_r);

      float p[3];
      project_plane_v3_v3v3(p, rc[index].axis, sortN);
      const float u = dot_v3v3(p, T1);
      const float v = dot_v3v3(p, T2);
      rc[index].angle = atan2f(v, u);

      for (int i = 0; i < S; i++) {
        const int ni = (i + 1) % S;
        BMFace *f = BM_face_create_quad_tri(bm,
                                            jr->verts[i],
                                            jr->verts[ni],
                                            rc[index].cap[ni],
                                            rc[index].cap[i],
                                            nullptr,
                                            BM_CREATE_NOP);
        if (f) {
          BM_elem_flag_enable(f, BM_ELEM_TAG);
        }
      }
    }

    std::sort(
        rc, rc + ring_count, [](const RingCap &a, const RingCap &b) { return a.angle < b.angle; });

    for (int i = 0; i < ring_count; i++) {
      const int j = (i + 1) % ring_count;
      JunctionRing *ria = rc[i].jr;
      JunctionRing *rib = rc[j].jr;

      const int S = ria->segments;

      const int shiftB = best_ring_shift(rc[i].cap, rc[j].cap, S, vcenter->co);

      stitch_rings_with_shift(bm, rc[i].cap, rc[j].cap, S, shiftB);
    }

    MEM_freeN(rc);
  }
}

/* Build a tube around a single edge, returning the two end rings */
static void build_tube_for_edge(BMesh *bm,
                                BMEdge *e,
                                const float radius,
                                const int segments,
                                BMVert ***r_ring1,
                                BMVert ***r_ring2)
{
  BMVert *v1 = e->v1;
  BMVert *v2 = e->v2;

  float edge_vec[3];
  sub_v3_v3v3(edge_vec, v2->co, v1->co);
  normalize_v3(edge_vec);

  float ref_vec[3] = {0.0f, 0.0f, 1.0f};
  if (fabsf(dot_v3v3(ref_vec, edge_vec)) > 0.99f) {
    ref_vec[0] = 1.0f;
    ref_vec[1] = 0.0f;
    ref_vec[2] = 0.0f;
  }

  float u_axis[3], v_axis[3];
  cross_v3_v3v3(u_axis, edge_vec, ref_vec);
  normalize_v3(u_axis);
  cross_v3_v3v3(v_axis, u_axis, edge_vec);
  normalize_v3(v_axis);

  BMVert **ring1 = (BMVert **)MEM_mallocN(sizeof(BMVert *) * segments, __func__);
  BMVert **ring2 = (BMVert **)MEM_mallocN(sizeof(BMVert *) * segments, __func__);

  const float angle_step = (2.0f * (float)M_PI) / (float)segments;

  for (int i = 0; i < segments; i++) {
    const float angle = i * angle_step;
    const float cos_a = cosf(angle);
    const float sin_a = sinf(angle);

    float offset[3];
    mul_v3_v3fl(offset, u_axis, cos_a * radius);
    madd_v3_v3fl(offset, v_axis, sin_a * radius);

    float vco1[3], vco2[3];
    add_v3_v3v3(vco1, v1->co, offset);
    add_v3_v3v3(vco2, v2->co, offset);

    ring1[i] = BM_vert_create(bm, vco1, nullptr, BM_CREATE_NOP);
    ring2[i] = BM_vert_create(bm, vco2, nullptr, BM_CREATE_NOP);
  }

  for (int i = 0; i < segments; i++) {
    int ni = (i + 1) % segments;
    BM_face_create_quad_tri(bm, ring1[i], ring1[ni], ring2[ni], ring2[i], nullptr, BM_CREATE_NOP);
  }

  *r_ring1 = ring1;
  *r_ring2 = ring2;
}

static BMLoop *bm_edge_tag_faceloop(BMEdge *e)
{
  BMLoop *l, *l_first;

  l = l_first = e->l;
  do {
    if (BM_elem_flag_test(l->f, BM_ELEM_TAG)) {
      return l;
    }
  } while ((l = l->radial_next) != l_first);

  /* in the case this is used, we know this will never happen */
  return nullptr;
}

static void bm_vert_boundary_tangent(
    BMVert *v, float r_no[3], float r_no_face[3], BMVert **r_va_other, BMVert **r_vb_other)
{
  BMIter iter;
  BMEdge *e_iter;

  BMEdge *e_a = nullptr, *e_b = nullptr;
  BMVert *v_a, *v_b;

  BMLoop *l_a, *l_b;

  float no_face[3], no_edge[3];
  float tvec_a[3], tvec_b[3];

  /* get 2 boundary edges, there should only _be_ 2,
   * in case there are more - results won't be valid of course */
  BM_ITER_ELEM (e_iter, &iter, v, BM_EDGES_OF_VERT) {
    if (BM_elem_flag_test(e_iter, BM_ELEM_TAG)) {
      if (e_a == nullptr) {
        e_a = e_iter;
      }
      else {
        e_b = e_iter;
        break;
      }
    }
  }

  if (e_a && e_b) {
    /* NOTE: with an incorrectly flushed selection this can crash. */
    l_a = bm_edge_tag_faceloop(e_a);
    l_b = bm_edge_tag_faceloop(e_b);

    /* average edge face normal */
    add_v3_v3v3(no_face, l_a->f->no, l_b->f->no);
    normalize_v3(no_face);

    /* average edge direction */
    v_a = BM_edge_other_vert(e_a, v);
    v_b = BM_edge_other_vert(e_b, v);

    sub_v3_v3v3(tvec_a, v->co, v_a->co);
    sub_v3_v3v3(tvec_b, v_b->co, v->co);
    normalize_v3(tvec_a);
    normalize_v3(tvec_b);
    add_v3_v3v3(no_edge, tvec_a, tvec_b); /* not unit length but this is ok */

    /* check are we flipped the right way */
    BM_edge_calc_face_tangent(e_a, l_a, tvec_a);
    BM_edge_calc_face_tangent(e_b, l_b, tvec_b);
    add_v3_v3(tvec_a, tvec_b);

    *r_va_other = v_a;
    *r_vb_other = v_b;
  }
  else {
    /* degenerate case - vertex connects a boundary edged face to other faces,
     * so we have only one boundary face - only use it for calculations */
    l_a = bm_edge_tag_faceloop(e_a);

    copy_v3_v3(no_face, l_a->f->no);

    /* edge direction */
    v_a = BM_edge_other_vert(e_a, v);
    v_b = nullptr;

    sub_v3_v3v3(no_edge, v->co, v_a->co);

    /* check are we flipped the right way */
    BM_edge_calc_face_tangent(e_a, l_a, tvec_a);

    *r_va_other = nullptr;
    *r_vb_other = nullptr;
  }

  /* find the normal */
  cross_v3_v3v3(r_no, no_edge, no_face);
  normalize_v3(r_no);

  if (dot_v3v3(r_no, tvec_a) > 0.0f) {
    negate_v3(r_no);
  }

  copy_v3_v3(r_no_face, no_face);
}

/* check if we are the only tagged loop-face around this edge */
static bool bm_loop_is_radial_boundary(BMLoop *l_first)
{
  BMLoop *l = l_first->radial_next;

  if (l == l_first) {
    return true; /* a real boundary */
  }

  do {
    if (BM_elem_flag_test(l->f, BM_ELEM_TAG)) {
      return false;
    }
  } while ((l = l->radial_next) != l_first);

  return true;
}


void BM_mesh_wireframe(BMesh *bm,
                       const float offset,
                       const float offset_fac,
                       const float offset_fac_vg,
                       const bool use_replace,
                       const bool use_boundary,
                       const bool use_even_offset,
                       const bool use_relative_offset,
                       const bool use_crease,
                       const float crease_weight,
                       const int defgrp_index,
                       const bool defgrp_invert,
                       const short mat_offset,
                       const int mat_max,
                       /* for operators */
                       const bool use_tag,
                       const int segments)
{
  if (junction_map == nullptr) {
    junction_map = BLI_ghash_ptr_new(__func__);
  }

  const int totedge_orig = bm->totedge;
  BMEdge **edges_src = MEM_malloc_arrayN<BMEdge *>(totedge_orig, __func__);

  BMIter iter;
  BMEdge *e;
  int ei = 0;
  BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
    edges_src[ei++] = e;
  }

  const float ofs_orig = -(((-offset_fac + 1.0f) * 0.5f) * offset);
  const float ofs_new = offset + ofs_orig;
  const float ofs_mid = (ofs_orig + ofs_new) / 2.0f;
  const float inset = offset / 2.0f;
  int cd_edge_crease_offset = use_crease ? CustomData_get_offset_named(
                                               &bm->edata, CD_PROP_FLOAT, "crease_edge") :
                                           -1;
  const int cd_dvert_offset = (defgrp_index != -1) ?
                                  CustomData_get_offset(&bm->vdata, CD_MDEFORMVERT) :
                                  -1;
  const float offset_fac_vg_inv = 1.0f - offset_fac_vg;

  const int totvert_orig = bm->totvert;

  BMIter itersub;
  /* filled only with boundary verts */
  BMVert **verts_src = MEM_malloc_arrayN<BMVert *>(totvert_orig, __func__);
  BMVert **verts_neg = MEM_malloc_arrayN<BMVert *>(totvert_orig, __func__);
  BMVert **verts_pos = MEM_malloc_arrayN<BMVert *>(totvert_orig, __func__);

  /* Will over-allocate, but makes for easy lookups by index to keep aligned. */
  BMVert **verts_boundary = static_cast<BMVert **>(
      use_boundary ? MEM_mallocN(sizeof(BMVert *) * totvert_orig, __func__) : nullptr);

  float *verts_relfac = static_cast<float *>(
      (use_relative_offset || (cd_dvert_offset != -1)) ?
          MEM_mallocN(sizeof(float) * totvert_orig, __func__) :
          nullptr);

  /* May over-allocate if not all faces have wire. */
  BMVert **verts_loop;
  int verts_loop_tot = 0;

  BMVert *v_src;

  BMFace *f_src;
  BMLoop *l;

  float tvec[3];
  float fac, fac_shell;

  int i;

  if (use_crease && cd_edge_crease_offset == -1) {
    BM_data_layer_add_named(bm, &bm->edata, CD_PROP_FLOAT, "crease_edge");
    cd_edge_crease_offset = CustomData_get_offset_named(&bm->edata, CD_PROP_FLOAT, "crease_edge");
  }

  BM_ITER_MESH_INDEX (v_src, &iter, bm, BM_VERTS_OF_MESH, i) {
    BM_elem_index_set(v_src, i); /* set_inline */

    verts_src[i] = v_src;
    BM_elem_flag_disable(v_src, BM_ELEM_TAG);
  }
  bm->elem_index_dirty &= ~BM_VERT;

  /* setup tags, all faces and verts will be tagged which will be duplicated */

  BM_ITER_MESH_INDEX (f_src, &iter, bm, BM_FACES_OF_MESH, i) {
    BM_elem_index_set(f_src, i); /* set_inline */

    if (use_tag) {
      if (!BM_elem_flag_test(f_src, BM_ELEM_TAG)) {
        continue;
      }
    }
    else {
      BM_elem_flag_enable(f_src, BM_ELEM_TAG);
    }

    verts_loop_tot += f_src->len;
    BM_ITER_ELEM (l, &itersub, f_src, BM_LOOPS_OF_FACE) {
      BM_elem_flag_enable(l->v, BM_ELEM_TAG);

      /* also tag boundary edges */
      BM_elem_flag_set(l->e, BM_ELEM_TAG, bm_loop_is_radial_boundary(l));
    }
  }
  bm->elem_index_dirty &= ~BM_FACE;

  /* duplicate tagged verts */
  for (i = 0; i < totvert_orig; i++) {
    v_src = verts_src[i];
    if (BM_elem_flag_test(v_src, BM_ELEM_TAG)) {
      fac = 1.0f;

      if (verts_relfac) {
        if (use_relative_offset) {
          verts_relfac[i] = BM_vert_calc_median_tagged_edge_length(v_src);
        }
        else {
          verts_relfac[i] = 1.0f;
        }

        if (cd_dvert_offset != -1) {
          MDeformVert *dvert = static_cast<MDeformVert *>(
              BM_ELEM_CD_GET_VOID_P(v_src, cd_dvert_offset));
          float defgrp_fac = BKE_defvert_find_weight(dvert, defgrp_index);

          if (defgrp_invert) {
            defgrp_fac = 1.0f - defgrp_fac;
          }

          if (offset_fac_vg > 0.0f) {
            defgrp_fac = (offset_fac_vg + (defgrp_fac * offset_fac_vg_inv));
          }

          verts_relfac[i] *= defgrp_fac;
        }

        fac *= verts_relfac[i];
      }

      verts_neg[i] = BM_vert_create(bm, nullptr, v_src, BM_CREATE_NOP);
      verts_pos[i] = BM_vert_create(bm, nullptr, v_src, BM_CREATE_NOP);

      if (offset == 0.0f) {
        madd_v3_v3v3fl(verts_neg[i]->co, v_src->co, v_src->no, ofs_orig * fac);
        madd_v3_v3v3fl(verts_pos[i]->co, v_src->co, v_src->no, ofs_new * fac);
      }
      else {
        madd_v3_v3v3fl(tvec, v_src->co, v_src->no, ofs_mid * fac);

        madd_v3_v3v3fl(verts_neg[i]->co, tvec, v_src->no, (ofs_orig - ofs_mid) * fac);
        madd_v3_v3v3fl(verts_pos[i]->co, tvec, v_src->no, (ofs_new - ofs_mid) * fac);
      }
    }
    else {
      /* could skip this */
      verts_neg[i] = nullptr;
      verts_pos[i] = nullptr;
    }

    /* conflicts with BM_vert_calc_median_tagged_edge_length */
    if (use_relative_offset == false) {
      BM_elem_flag_disable(v_src, BM_ELEM_TAG);
    }
  }

  if (use_relative_offset) {
    BM_mesh_elem_hflag_disable_all(bm, BM_VERT, BM_ELEM_TAG, false);
  }

  verts_loop = MEM_malloc_arrayN<BMVert *>(verts_loop_tot, __func__);
  verts_loop_tot = 0; /* count up again */

  BM_ITER_MESH (f_src, &iter, bm, BM_FACES_OF_MESH) {

    if (use_tag && !BM_elem_flag_test(f_src, BM_ELEM_TAG)) {
      continue;
    }

    BM_ITER_ELEM (l, &itersub, f_src, BM_LOOPS_OF_FACE) {
      /* Because some faces might be skipped! */
      BM_elem_index_set(l, verts_loop_tot); /* set_dirty */

      BM_loop_calc_face_tangent(l, tvec);

      /* create offset vert */
      fac = 1.0f;

      if (verts_relfac) {
        fac *= verts_relfac[BM_elem_index_get(l->v)];
      }

      fac_shell = fac;
      if (use_even_offset) {
        fac_shell *= shell_angle_to_dist((float(M_PI) - BM_loop_calc_face_angle(l)) * 0.5f);
      }

      madd_v3_v3v3fl(tvec, l->v->co, tvec, inset * fac_shell);
      if (offset != 0.0f) {
        madd_v3_v3fl(tvec, l->v->no, ofs_mid * fac);
      }
      verts_loop[verts_loop_tot] = BM_vert_create(bm, tvec, l->v, BM_CREATE_NOP);

      if (use_boundary) {
        if (BM_elem_flag_test(l->e, BM_ELEM_TAG)) { /* is this a boundary? */
          BMVert *v_pair[2] = {l->v, l->next->v};

          for (i = 0; i < 2; i++) {
            BMVert *v_boundary = v_pair[i];
            if (!BM_elem_flag_test(v_boundary, BM_ELEM_TAG)) {
              const int v_boundary_index = BM_elem_index_get(v_boundary);
              float no_face[3];
              BMVert *va_other;
              BMVert *vb_other;

              BM_elem_flag_enable(v_boundary, BM_ELEM_TAG);

              bm_vert_boundary_tangent(v_boundary, tvec, no_face, &va_other, &vb_other);

              /* create offset vert */
              /* similar to code above but different angle calc */
              fac = 1.0f;

              if (verts_relfac) {
                fac *= verts_relfac[v_boundary_index];
              }

              fac_shell = fac;
              if (use_even_offset) {
                if (va_other) { /* for verts with only one boundary edge - this will be nullptr */
                  fac_shell *= shell_angle_to_dist(
                      (float(M_PI) - angle_on_axis_v3v3v3_v3(
                                         va_other->co, v_boundary->co, vb_other->co, no_face)) *
                      0.5f);
                }
              }

              madd_v3_v3v3fl(tvec, v_boundary->co, tvec, inset * fac_shell);
              if (offset != 0.0f) {
                madd_v3_v3fl(tvec, v_boundary->no, ofs_mid * fac);
              }
              verts_boundary[v_boundary_index] = BM_vert_create(
                  bm, tvec, v_boundary, BM_CREATE_NOP);
            }
          }
        }
      }

      verts_loop_tot++;
    }
  }
  bm->elem_index_dirty |= BM_LOOP;

  BM_ITER_MESH (f_src, &iter, bm, BM_FACES_OF_MESH) {

    /* skip recently added faces */
    if (BM_elem_index_get(f_src) == -1) {
      continue;
    }

    if (use_tag && !BM_elem_flag_test(f_src, BM_ELEM_TAG)) {
      continue;
    }

    BM_elem_flag_disable(f_src, BM_ELEM_TAG);

    BM_ITER_ELEM (l, &itersub, f_src, BM_LOOPS_OF_FACE) {
      BMFace *f_new;
      BMLoop *l_new;
      BMLoop *l_next = l->next;
      BMVert *v_l1 = verts_loop[BM_elem_index_get(l)];
      BMVert *v_l2 = verts_loop[BM_elem_index_get(l_next)];

      BMVert *v_src_l1 = l->v;
      BMVert *v_src_l2 = l_next->v;

      const int i_1 = BM_elem_index_get(v_src_l1);
      const int i_2 = BM_elem_index_get(v_src_l2);

      BMVert *v_neg1 = verts_neg[i_1];
      BMVert *v_neg2 = verts_neg[i_2];

      BMVert *v_pos1 = verts_pos[i_1];
      BMVert *v_pos2 = verts_pos[i_2];

      f_new = BM_face_create_quad_tri(bm, v_l1, v_l2, v_neg2, v_neg1, f_src, BM_CREATE_NOP);
      if (mat_offset) {
        f_new->mat_nr = std::clamp(f_new->mat_nr + mat_offset, 0, mat_max);
      }
      BM_elem_flag_enable(f_new, BM_ELEM_TAG);
      l_new = BM_FACE_FIRST_LOOP(f_new);

      BM_elem_attrs_copy(bm, l, l_new);
      BM_elem_attrs_copy(bm, l, l_new->prev);
      BM_elem_attrs_copy(bm, l_next, l_new->next);
      BM_elem_attrs_copy(bm, l_next, l_new->next->next);

      f_new = BM_face_create_quad_tri(bm, v_l2, v_l1, v_pos1, v_pos2, f_src, BM_CREATE_NOP);

      if (mat_offset) {
        f_new->mat_nr = std::clamp(f_new->mat_nr + mat_offset, 0, mat_max);
      }
      BM_elem_flag_enable(f_new, BM_ELEM_TAG);
      l_new = BM_FACE_FIRST_LOOP(f_new);

      BM_elem_attrs_copy(bm, l_next, l_new);
      BM_elem_attrs_copy(bm, l_next, l_new->prev);
      BM_elem_attrs_copy(bm, l, l_new->next);
      BM_elem_attrs_copy(bm, l, l_new->next->next);

      if (use_boundary) {
        if (BM_elem_flag_test(l->e, BM_ELEM_TAG)) {
          /* we know its a boundary and this is the only face user (which is being wire'd) */
          /* we know we only touch this edge/face once */
          BMVert *v_b1 = verts_boundary[i_1];
          BMVert *v_b2 = verts_boundary[i_2];

          f_new = BM_face_create_quad_tri(bm, v_b2, v_b1, v_neg1, v_neg2, f_src, BM_CREATE_NOP);
          if (mat_offset) {
            f_new->mat_nr = std::clamp(f_new->mat_nr + mat_offset, 0, mat_max);
          }
          BM_elem_flag_enable(f_new, BM_ELEM_TAG);
          l_new = BM_FACE_FIRST_LOOP(f_new);

          BM_elem_attrs_copy(bm, l_next, l_new);
          BM_elem_attrs_copy(bm, l_next, l_new->prev);
          BM_elem_attrs_copy(bm, l, l_new->next);
          BM_elem_attrs_copy(bm, l, l_new->next->next);

          f_new = BM_face_create_quad_tri(bm, v_b1, v_b2, v_pos2, v_pos1, f_src, BM_CREATE_NOP);
          if (mat_offset) {
            f_new->mat_nr = std::clamp(f_new->mat_nr + mat_offset, 0, mat_max);
          }
          BM_elem_flag_enable(f_new, BM_ELEM_TAG);
          l_new = BM_FACE_FIRST_LOOP(f_new);

          BM_elem_attrs_copy(bm, l, l_new);
          BM_elem_attrs_copy(bm, l, l_new->prev);
          BM_elem_attrs_copy(bm, l_next, l_new->next);
          BM_elem_attrs_copy(bm, l_next, l_new->next->next);

          if (use_crease) {
            BMEdge *e_new;
            e_new = BM_edge_exists(v_pos1, v_b1);
            BM_ELEM_CD_SET_FLOAT(e_new, cd_edge_crease_offset, crease_weight);

            e_new = BM_edge_exists(v_pos2, v_b2);
            BM_ELEM_CD_SET_FLOAT(e_new, cd_edge_crease_offset, crease_weight);

            e_new = BM_edge_exists(v_neg1, v_b1);
            BM_ELEM_CD_SET_FLOAT(e_new, cd_edge_crease_offset, crease_weight);

            e_new = BM_edge_exists(v_neg2, v_b2);
            BM_ELEM_CD_SET_FLOAT(e_new, cd_edge_crease_offset, crease_weight);
          }
        }
      }

      if (use_crease) {
        BMEdge *e_new;
        e_new = BM_edge_exists(v_pos1, v_l1);
        BM_ELEM_CD_SET_FLOAT(e_new, cd_edge_crease_offset, crease_weight);

        e_new = BM_edge_exists(v_pos2, v_l2);
        BM_ELEM_CD_SET_FLOAT(e_new, cd_edge_crease_offset, crease_weight);

        e_new = BM_edge_exists(v_neg1, v_l1);
        BM_ELEM_CD_SET_FLOAT(e_new, cd_edge_crease_offset, crease_weight);

        e_new = BM_edge_exists(v_neg2, v_l2);
        BM_ELEM_CD_SET_FLOAT(e_new, cd_edge_crease_offset, crease_weight);
      }
    }
  }

  if (use_boundary) {
    MEM_freeN(verts_boundary);
  }

  if (segments > 0) {
    fprintf(stderr, "building tubes with segments=%d\n", segments);

    for (int iedge = 0; iedge < totedge_orig; iedge++) {
      BMEdge *e_it = edges_src[iedge];
      if (!e_it) {
        continue;
      }
      fprintf(stderr,
              "  [tube] edge (%d -> %d)\n",
              BM_elem_index_get(e_it->v1),
              BM_elem_index_get(e_it->v2));

      BMVert **ring1, **ring2;
      build_tube_for_edge(bm, e_it, offset * 0.5f, segments, &ring1, &ring2);
      add_ring_to_junction(junction_map, e_it->v1, ring1, segments);
      add_ring_to_junction(junction_map, e_it->v2, ring2, segments);
    }

    build_vertex_junction_caps(bm, junction_map);
  }

  if (verts_relfac) {
    MEM_freeN(verts_relfac);
  }

  if (use_replace) {

    if (use_tag) {
/* only remove faces which are original and used to make wire,
 * use 'verts_pos' and 'verts_neg' to avoid a feedback loop. */

/* vertex must be from 'verts_src' */
#define VERT_DUPE_TEST_ORIG(v) (verts_neg[BM_elem_index_get(v)] != nullptr)
#define VERT_DUPE_TEST(v) (verts_pos[BM_elem_index_get(v)] != nullptr)
#define VERT_DUPE_CLEAR(v) \
  { \
    verts_pos[BM_elem_index_get(v)] = nullptr; \
  } \
  (void)0

      /* first ensure we keep all verts which are used in faces that weren't
       * entirely made into wire. */
      BM_ITER_MESH (f_src, &iter, bm, BM_FACES_OF_MESH) {
        int mix_flag = 0;
        BMLoop *l_iter, *l_first;

        /* skip new faces */
        if (BM_elem_index_get(f_src) == -1) {
          continue;
        }

        l_iter = l_first = BM_FACE_FIRST_LOOP(f_src);
        do {
          mix_flag |= (VERT_DUPE_TEST_ORIG(l_iter->v) ? 1 : 2);
          if (mix_flag == (1 | 2)) {
            break;
          }
        } while ((l_iter = l_iter->next) != l_first);

        if (mix_flag == (1 | 2)) {
          l_iter = l_first = BM_FACE_FIRST_LOOP(f_src);
          do {
            VERT_DUPE_CLEAR(l_iter->v);
          } while ((l_iter = l_iter->next) != l_first);
        }
      }

      /* now remove any verts which were made into wire by all faces */
      for (i = 0; i < totvert_orig; i++) {
        v_src = verts_src[i];
        BLI_assert(i == BM_elem_index_get(v_src));
        if (VERT_DUPE_TEST(v_src)) {
          BM_vert_kill(bm, v_src);
        }
      }

#undef VERT_DUPE_TEST_ORIG
#undef VERT_DUPE_TEST
#undef VERT_DUPE_CLEAR
    }
    else {
      /* simple case, no tags - replace all */
      for (i = 0; i < totvert_orig; i++) {
        BM_vert_kill(bm, verts_src[i]);
      }
    }
  }

  MEM_freeN(edges_src);
  MEM_freeN(verts_src);
  MEM_freeN(verts_neg);
  MEM_freeN(verts_pos);
  MEM_freeN(verts_loop);

  if (junction_map) {
    BLI_ghash_free(junction_map, nullptr, free_junction_list);
    junction_map = nullptr;
  }
}
