/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 *
 * Mirror calculation for edit-mode and object mode.
 */

#include "MEM_guardedalloc.h"

#include "DNA_object_types.h"

#include "BKE_editmesh.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_types.hh"

#include "BLI_kdtree.h"

#include "ED_mesh.hh"

/* -------------------------------------------------------------------- */
/** \name Mesh Spatial Mirror API
 * \{ */
std::optional<EditMeshSymmetryHelper> EditMeshSymmetryHelper::create_if_needed(Object *ob,
                                                                               uchar htype)
{
  BLI_assert(htype != 0);
  BLI_assert((htype & ~(BM_VERT | BM_EDGE | BM_FACE)) == 0);

  if (!ob || !ob->data) {
    return std::nullopt;
  }
  Mesh *mesh = static_cast<Mesh *>(ob->data);
  BMEditMesh *em = BKE_editmesh_from_object(ob);

  if (!em || !em->bm || mesh->symmetry == 0) {
    return std::nullopt;
  }
  return EditMeshSymmetryHelper(ob, htype);
}

EditMeshSymmetryHelper::EditMeshSymmetryHelper(Object *ob, uchar htype)
    : em(BKE_editmesh_from_object(ob)), mesh(static_cast<Mesh *>(ob->data)), htype_(htype)
{
  BMesh *bmesh = em->bm;
  use_topology_mirror = (mesh->editflag & ME_EDIT_MIRROR_TOPO) != 0;

  blender::Set<BMVert *> processed_verts;
  blender::Set<BMEdge *> processed_edges;
  blender::Set<BMFace *> processed_faces;

  BMIter iter;

  for (int axis = 0; axis < 3; ++axis) {
    if (mesh->symmetry & (ME_SYMMETRY_X << axis)) {
      EDBM_verts_mirror_cache_begin(em,
                                    axis,
                                    (htype_ & BM_VERT) != 0,
                                    (htype_ & BM_EDGE) != 0,
                                    (htype_ & BM_FACE) != 0,
                                    use_topology_mirror);

      if (htype_ & BM_VERT) {
        BMVert *v_curr;
        BM_ITER_MESH (v_curr, &iter, bmesh, BM_VERTS_OF_MESH) {
          if (processed_verts.contains(v_curr)) {
            continue;
          }
          BMVert *v_mir = EDBM_verts_mirror_get(em, v_curr);
          if (v_mir && v_mir != v_curr) {
            BMVert *v_mir_check = EDBM_verts_mirror_get(em, v_mir);
            if (v_mir_check == v_curr) {
              vert_to_mirrors_map.lookup_or_add(v_curr, {}).append(v_mir);
              vert_to_mirrors_map.lookup_or_add(v_mir, {}).append(v_curr);
              processed_verts.add(v_curr);
              processed_verts.add(v_mir);
            }
          }
        }
      }

      if (htype_ & BM_EDGE) {
        BMEdge *e_curr;
        BM_ITER_MESH (e_curr, &iter, bmesh, BM_EDGES_OF_MESH) {
          if (processed_edges.contains(e_curr)) {
            continue;
          }
          BMEdge *e_mir = EDBM_verts_mirror_get_edge(em, e_curr);
          if (e_mir && e_mir != e_curr) {
            BMEdge *e_mir_check = EDBM_verts_mirror_get_edge(em, e_mir);
            if (e_mir_check == e_curr) {
              edge_to_mirrors_map.lookup_or_add(e_curr, {}).append(e_mir);
              edge_to_mirrors_map.lookup_or_add(e_mir, {}).append(e_curr);
              processed_edges.add(e_curr);
              processed_edges.add(e_mir);
            }
          }
        }
      }

      if (htype_ & BM_FACE) {
        BMFace *f_curr;
        BM_ITER_MESH (f_curr, &iter, bmesh, BM_FACES_OF_MESH) {
          if (processed_faces.contains(f_curr)) {
            continue;
          }
          BMFace *f_mir = EDBM_verts_mirror_get_face(em, f_curr);
          if (f_mir && f_mir != f_curr) {
            BMFace *f_mir_check = EDBM_verts_mirror_get_face(em, f_mir);
            if (f_mir_check == f_curr) {
              face_to_mirrors_map.lookup_or_add(f_curr, {}).append(f_mir);
              face_to_mirrors_map.lookup_or_add(f_mir, {}).append(f_curr);
              processed_faces.add(f_curr);
              processed_faces.add(f_mir);
            }
          }
        }
      }

      EDBM_verts_mirror_cache_end(em);
    }
  }
}

void EditMeshSymmetryHelper::tag_symmetrical_group(const blender::Vector<BMFace *> &f_initial,
                                                   char hflag) const
{
  BLI_assert((this->htype_ & BM_FACE) != 0);

  for (BMFace *f_orig : f_initial) {
    BM_elem_flag_enable(f_orig, hflag);
  }

  for (int axis = 0; axis < 3; ++axis) {
    if (this->mesh->symmetry & (ME_SYMMETRY_X << axis)) {
      EDBM_verts_mirror_cache_begin(this->em, axis, true, true, true, this->use_topology_mirror);
      for (BMFace *f_orig : f_initial) {
        BMFace *f_mir = EDBM_verts_mirror_get_face(this->em, f_orig);
        if (f_mir) {
          BM_elem_flag_enable(f_mir, hflag);
        }
      }
      EDBM_verts_mirror_cache_end(this->em);
    }
  }
}

void EditMeshSymmetryHelper::apply_on_mirror_verts(BMVert *v,
                                                   blender::FunctionRef<void(BMVert *)> op) const
{
  BLI_assert((this->htype_ & BM_VERT) != 0);
  if (!vert_to_mirrors_map.contains(v)) {
    return;
  }
  for (BMVert *v_mirror : vert_to_mirrors_map.lookup(v)) {
    op(v_mirror);
  }
}

void EditMeshSymmetryHelper::apply_on_mirror_edges(BMEdge *e,
                                                   blender::FunctionRef<void(BMEdge *)> op) const
{
  BLI_assert((this->htype_ & BM_EDGE) != 0);
  if (!edge_to_mirrors_map.contains(e)) {
    return;
  }
  for (BMEdge *e_mirror : edge_to_mirrors_map.lookup(e)) {
    op(e_mirror);
  }
}

void EditMeshSymmetryHelper::apply_on_mirror_faces(BMFace *f,
                                                   blender::FunctionRef<void(BMFace *)> op) const
{
  BLI_assert((this->htype_ & BM_FACE) != 0);
  if (!face_to_mirrors_map.contains(f)) {
    return;
  }
  for (BMFace *f_mirror : face_to_mirrors_map.lookup(f)) {
    op(f_mirror);
  }
}

bool EditMeshSymmetryHelper::any_mirror_vert_selected(BMVert *v, char hflag) const
{
  BLI_assert((this->htype_ & BM_VERT) != 0);
  if (!vert_to_mirrors_map.contains(v)) {
    return false;
  }
  for (BMVert *v_mirror : vert_to_mirrors_map.lookup(v)) {
    if (BM_elem_flag_test(v_mirror, hflag) && !BM_elem_flag_test(v_mirror, BM_ELEM_HIDDEN)) {
      return true;
    }
  }
  return false;
}

bool EditMeshSymmetryHelper::any_mirror_edge_selected(BMEdge *e, char hflag) const
{
  BLI_assert((this->htype_ & BM_EDGE) != 0);
  if (!edge_to_mirrors_map.contains(e)) {
    return false;
  }
  for (BMEdge *e_mirror : edge_to_mirrors_map.lookup(e)) {
    if (BM_elem_flag_test(e_mirror, hflag) && !BM_elem_flag_test(e_mirror, BM_ELEM_HIDDEN)) {
      return true;
    }
  }
  return false;
}

bool EditMeshSymmetryHelper::any_mirror_face_selected(BMFace *f, char hflag) const
{
  BLI_assert((this->htype_ & BM_FACE) != 0);
  if (!face_to_mirrors_map.contains(f)) {
    return false;
  }
  for (BMFace *f_mirror : face_to_mirrors_map.lookup(f)) {
    if (BM_elem_flag_test(f_mirror, hflag) && !BM_elem_flag_test(f_mirror, BM_ELEM_HIDDEN)) {
      return true;
    }
  }
  return false;
}

void EditMeshSymmetryHelper::set_flag_on_mirror_verts(BMVert *v, char hflag, bool value) const
{
  apply_on_mirror_verts(v, [hflag, value](BMVert *v_mir) {
    if (!BM_elem_flag_test(v_mir, BM_ELEM_HIDDEN)) {
      if (value) {
        BM_elem_flag_enable(v_mir, hflag);
      }
      else {
        BM_elem_flag_disable(v_mir, hflag);
      }
    }
  });
}

void EditMeshSymmetryHelper::set_flag_on_mirror_edges(BMEdge *e, char hflag, bool value) const
{
  apply_on_mirror_edges(e, [hflag, value](BMEdge *e_mir) {
    if (!BM_elem_flag_test(e_mir, BM_ELEM_HIDDEN)) {
      if (value) {
        BM_elem_flag_enable(e_mir, hflag);
      }
      else {
        BM_elem_flag_disable(e_mir, hflag);
      }
    }
  });
}

void EditMeshSymmetryHelper::set_flag_on_mirror_faces(BMFace *f, char hflag, bool value) const
{
  apply_on_mirror_faces(f, [hflag, value](BMFace *f_mir) {
    if (!BM_elem_flag_test(f_mir, BM_ELEM_HIDDEN)) {
      if (value) {
        BM_elem_flag_enable(f_mir, hflag);
      }
      else {
        BM_elem_flag_disable(f_mir, hflag);
      }
    }
  });
}

#define KD_THRESH 0.00002f

static struct {
  KDTree_3d *tree;
} MirrKdStore = {nullptr};

void ED_mesh_mirror_spatial_table_begin(Object *ob, BMEditMesh *em, Mesh *mesh_eval)
{
  Mesh *mesh = static_cast<Mesh *>(ob->data);
  const bool use_em = (!mesh_eval && em && mesh->runtime->edit_mesh.get() == em);
  const int totvert = use_em    ? em->bm->totvert :
                      mesh_eval ? mesh_eval->verts_num :
                                  mesh->verts_num;

  if (MirrKdStore.tree) { /* happens when entering this call without ending it */
    ED_mesh_mirror_spatial_table_end(ob);
  }

  MirrKdStore.tree = BLI_kdtree_3d_new(totvert);

  if (use_em) {
    BMVert *eve;
    BMIter iter;
    int i;

    /* this needs to be valid for index lookups later (callers need) */
    BM_mesh_elem_table_ensure(em->bm, BM_VERT);

    BM_ITER_MESH_INDEX (eve, &iter, em->bm, BM_VERTS_OF_MESH, i) {
      BLI_kdtree_3d_insert(MirrKdStore.tree, i, eve->co);
    }
  }
  else {
    const blender::Span<blender::float3> positions = mesh_eval ? mesh_eval->vert_positions() :
                                                                 mesh->vert_positions();
    for (int i = 0; i < totvert; i++) {
      BLI_kdtree_3d_insert(MirrKdStore.tree, i, positions[i]);
    }
  }

  BLI_kdtree_3d_balance(MirrKdStore.tree);
}

int ED_mesh_mirror_spatial_table_lookup(Object *ob,
                                        BMEditMesh *em,
                                        Mesh *mesh_eval,
                                        const float co[3])
{
  if (MirrKdStore.tree == nullptr) {
    ED_mesh_mirror_spatial_table_begin(ob, em, mesh_eval);
  }

  if (MirrKdStore.tree) {
    KDTreeNearest_3d nearest;
    const int i = BLI_kdtree_3d_find_nearest(MirrKdStore.tree, co, &nearest);

    if (i != -1) {
      if (nearest.dist < KD_THRESH) {
        return i;
      }
    }
  }
  return -1;
}

void ED_mesh_mirror_spatial_table_end(Object * /*ob*/)
{
  /* TODO: store this in object/object-data (keep unused argument for now). */
  if (MirrKdStore.tree) {
    BLI_kdtree_3d_free(MirrKdStore.tree);
    MirrKdStore.tree = nullptr;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mesh Topology Mirror API
 * \{ */

using MirrTopoHash_t = uint;

struct MirrTopoVert_t {
  MirrTopoHash_t hash;
  int v_index;
};

static int mirrtopo_hash_sort(const void *l1, const void *l2)
{
  if (MirrTopoHash_t(intptr_t(l1)) > MirrTopoHash_t(intptr_t(l2))) {
    return 1;
  }
  if (MirrTopoHash_t(intptr_t(l1)) < MirrTopoHash_t(intptr_t(l2))) {
    return -1;
  }
  return 0;
}

static int mirrtopo_vert_sort(const void *v1, const void *v2)
{
  if (((MirrTopoVert_t *)v1)->hash > ((MirrTopoVert_t *)v2)->hash) {
    return 1;
  }
  if (((MirrTopoVert_t *)v1)->hash < ((MirrTopoVert_t *)v2)->hash) {
    return -1;
  }
  return 0;
}

bool ED_mesh_mirrtopo_recalc_check(BMEditMesh *em, Mesh *mesh, MirrTopoStore_t *mesh_topo_store)
{
  const bool is_editmode = em != nullptr;
  int totvert;
  int totedge;

  if (em) {
    totvert = em->bm->totvert;
    totedge = em->bm->totedge;
  }
  else {
    totvert = mesh->verts_num;
    totedge = mesh->edges_num;
  }

  if ((mesh_topo_store->index_lookup == nullptr) ||
      (mesh_topo_store->prev_is_editmode != is_editmode) ||
      (totvert != mesh_topo_store->prev_vert_tot) || (totedge != mesh_topo_store->prev_edge_tot))
  {
    return true;
  }
  return false;
}

void ED_mesh_mirrtopo_init(BMEditMesh *em,
                           Mesh *mesh,
                           MirrTopoStore_t *mesh_topo_store,
                           const bool skip_em_vert_array_init)
{
  if (em) {
    BLI_assert(mesh == nullptr);
  }
  const bool is_editmode = (em != nullptr);

  /* Edit-mode variables. */
  BMEdge *eed;
  BMIter iter;

  int a, last;
  int totvert, totedge;
  int tot_unique = -1, tot_unique_prev = -1;
  int tot_unique_edges = 0, tot_unique_edges_prev;

  MirrTopoHash_t topo_pass = 1;

  /* reallocate if needed */
  ED_mesh_mirrtopo_free(mesh_topo_store);

  mesh_topo_store->prev_is_editmode = is_editmode;

  if (em) {
    BM_mesh_elem_index_ensure(em->bm, BM_VERT);

    totvert = em->bm->totvert;
  }
  else {
    totvert = mesh->verts_num;
  }

  MirrTopoHash_t *topo_hash = MEM_calloc_arrayN<MirrTopoHash_t>(totvert, __func__);

  /* Initialize the vert-edge-user counts used to detect unique topology */
  if (em) {
    totedge = em->bm->totedge;

    BM_ITER_MESH (eed, &iter, em->bm, BM_EDGES_OF_MESH) {
      const int i1 = BM_elem_index_get(eed->v1), i2 = BM_elem_index_get(eed->v2);
      topo_hash[i1]++;
      topo_hash[i2]++;
    }
  }
  else {
    totedge = mesh->edges_num;
    for (const blender::int2 &edge : mesh->edges()) {
      topo_hash[edge[0]]++;
      topo_hash[edge[1]]++;
    }
  }

  MirrTopoHash_t *topo_hash_prev = static_cast<MirrTopoHash_t *>(MEM_dupallocN(topo_hash));

  tot_unique_prev = -1;
  tot_unique_edges_prev = -1;
  while (true) {
    /* use the number of edges per vert to give verts unique topology IDs */

    tot_unique_edges = 0;

    /* This can make really big numbers, wrapping around here is fine */
    if (em) {
      BM_ITER_MESH (eed, &iter, em->bm, BM_EDGES_OF_MESH) {
        const int i1 = BM_elem_index_get(eed->v1), i2 = BM_elem_index_get(eed->v2);
        topo_hash[i1] += topo_hash_prev[i2] * topo_pass;
        topo_hash[i2] += topo_hash_prev[i1] * topo_pass;
        tot_unique_edges += (topo_hash[i1] != topo_hash[i2]);
      }
    }
    else {
      for (const blender::int2 &edge : mesh->edges()) {
        const int i1 = edge[0], i2 = edge[1];
        topo_hash[i1] += topo_hash_prev[i2] * topo_pass;
        topo_hash[i2] += topo_hash_prev[i1] * topo_pass;
        tot_unique_edges += (topo_hash[i1] != topo_hash[i2]);
      }
    }
    memcpy(topo_hash_prev, topo_hash, sizeof(MirrTopoHash_t) * totvert);

    /* sort so we can count unique values */
    qsort(topo_hash_prev, totvert, sizeof(MirrTopoHash_t), mirrtopo_hash_sort);

    tot_unique = 1; /* account for skipping the first value */
    for (a = 1; a < totvert; a++) {
      if (topo_hash_prev[a - 1] != topo_hash_prev[a]) {
        tot_unique++;
      }
    }

    if ((tot_unique <= tot_unique_prev) && (tot_unique_edges <= tot_unique_edges_prev)) {
      /* Finish searching for unique values when 1 loop doesn't give a
       * higher number of unique values compared to the previous loop. */
      break;
    }
    tot_unique_prev = tot_unique;
    tot_unique_edges_prev = tot_unique_edges;
    /* Copy the hash calculated this iteration, so we can use them next time */
    memcpy(topo_hash_prev, topo_hash, sizeof(MirrTopoHash_t) * totvert);

    topo_pass++;
  }

  /* Hash/Index pairs are needed for sorting to find index pairs */
  MirrTopoVert_t *topo_pairs = MEM_calloc_arrayN<MirrTopoVert_t>(totvert, "MirrTopoPairs");

  /* since we are looping through verts, initialize these values here too */
  intptr_t *index_lookup = static_cast<intptr_t *>(
      MEM_mallocN(totvert * sizeof(*index_lookup), "mesh_topo_lookup"));

  if (em) {
    if (skip_em_vert_array_init == false) {
      BM_mesh_elem_table_ensure(em->bm, BM_VERT);
    }
  }

  for (a = 0; a < totvert; a++) {
    topo_pairs[a].hash = topo_hash[a];
    topo_pairs[a].v_index = a;

    /* initialize lookup */
    index_lookup[a] = -1;
  }

  qsort(topo_pairs, totvert, sizeof(MirrTopoVert_t), mirrtopo_vert_sort);

  last = 0;

  /* Get the pairs out of the sorted hashes.
   * NOTE: `totvert + 1` means we can use the previous 2,
   * but you can't ever access the last 'a' index of #MirrTopoPairs. */
  if (em) {
    BMVert **vtable = em->bm->vtable;
    for (a = 1; a <= totvert; a++) {
      // printf("I %d %ld %d\n",
      //        (a - last), MirrTopoPairs[a].hash, MirrTopoPairs[a].v_index);
      if ((a == totvert) || (topo_pairs[a - 1].hash != topo_pairs[a].hash)) {
        const int match_count = a - last;
        if (match_count == 2) {
          const int j = topo_pairs[a - 1].v_index, k = topo_pairs[a - 2].v_index;
          index_lookup[j] = intptr_t(vtable[k]);
          index_lookup[k] = intptr_t(vtable[j]);
        }
        else if (match_count == 1) {
          /* Center vertex. */
          const int j = topo_pairs[a - 1].v_index;
          index_lookup[j] = intptr_t(vtable[j]);
        }
        last = a;
      }
    }
  }
  else {
    /* same as above, for mesh */
    for (a = 1; a <= totvert; a++) {
      if ((a == totvert) || (topo_pairs[a - 1].hash != topo_pairs[a].hash)) {
        const int match_count = a - last;
        if (match_count == 2) {
          const int j = topo_pairs[a - 1].v_index, k = topo_pairs[a - 2].v_index;
          index_lookup[j] = k;
          index_lookup[k] = j;
        }
        else if (match_count == 1) {
          /* Center vertex. */
          const int j = topo_pairs[a - 1].v_index;
          index_lookup[j] = j;
        }
        last = a;
      }
    }
  }

  MEM_freeN(topo_pairs);
  topo_pairs = nullptr;

  MEM_freeN(topo_hash);
  MEM_freeN(topo_hash_prev);

  mesh_topo_store->index_lookup = index_lookup;
  mesh_topo_store->prev_vert_tot = totvert;
  mesh_topo_store->prev_edge_tot = totedge;
}

void ED_mesh_mirrtopo_free(MirrTopoStore_t *mesh_topo_store)
{
  MEM_SAFE_FREE(mesh_topo_store->index_lookup);
  mesh_topo_store->prev_vert_tot = -1;
  mesh_topo_store->prev_edge_tot = -1;
}

/** \} */
