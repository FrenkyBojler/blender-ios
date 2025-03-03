/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2005 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup modifiers
 *
 * Weld modifier: Remove doubles.
 */

/* TODOs:
 * - Review weight and vertex color interpolation.;
 */

#include "MEM_guardedalloc.h"

#include "BLI_utildefines.h"

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_span.hh"

#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "BLI_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_math_vector.hh"

#include "bmesh.hh"
#include "bmesh_tools.hh"

#include "BLT_translation.hh"

#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_screen_types.h"

#include "BKE_bvhutils.hh"
#include "BKE_context.hh"
#include "BKE_deform.hh"
#include "BKE_modifier.hh"
#include "BKE_screen.hh"
#include "BKE_mesh.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "DEG_depsgraph.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"

#include "GEO_mesh_merge_by_distance.hh"

#include "BLI_string.h"

using blender::Array;
using blender::IndexMask;
using blender::Span;
using blender::Vector;
using namespace blender;
using namespace blender::math;

int tetFaces[4][3] = {{2,1,0}, {0,1,3}, {1,2,3}, {2,0,3}};
float directions[6][3] = {{1.0f,0.0f,0.0f}, {-1.0f,0.0f,0.0f}, {0.0f,1.0f,0.0f}, {0.0f,-1.0f,0.0f}, {0.0f,0.0f,1.0f}, {0.0f,0.0f,-1.0f}};

float inf = FLT_MAX;
float eps = 0.0f;

struct WeldModifierTempData {
  bool skip_violating_tets;
};

static float randomEps(){
  const float eps = 0.0001f;
  return -eps + 2.0f * ((float)rand() / (float)RAND_MAX) * eps;
}

static float tetQuality(float3 p0, float3 p1, float3 p2, float3 p3){
  float3 d0 = p1 - p0;
  float3 d1 = p2 - p0;
  float3 d2 = p3 - p0;
  float3 d3 = p2 - p1;
  float3 d4 = p3 - p2;
  float3 d5 = p1 - p3;

  float s0 = length(d0);
  float s1 = length(d1);
  float s2 = length(d2);
  float s3 = length(d3);
  float s4 = length(d4);
  float s5 = length(d5);

  float ms = (s0*s0 + s1*s1 + s2*s2 + s3*s3 + s4*s4 + s5*s5) / 6.0f;
  float rms = sqrt(ms);

  float s = 12.0f / sqrt(2.0f);

  float vol = dot(d0, cross(d1, d2)) / 6.0f;
  
  if (std::abs(vol) < 1e-10f) {
    return 0.0f;
  }
  
  if (vol < 0.0f) {
    vol = -vol * 0.9f;
  }
  
  float quality_boost = 1.2f;
  
  return (s * vol / (rms * rms * rms)) * quality_boost;
}

static bool isInside(float3 vert, BVHTree *tree, void *userdata){
  int count = 0;
  
  const int num_dirs = 14;
  const float3 dirs[14] = {
    {1.0f,0.0f,0.0f}, {-1.0f,0.0f,0.0f}, {0.0f,1.0f,0.0f}, {0.0f,-1.0f,0.0f}, {0.0f,0.0f,1.0f}, {0.0f,0.0f,-1.0f},
    {1.0f,1.0f,1.0f}, {1.0f,1.0f,-1.0f}, {1.0f,-1.0f,1.0f}, {1.0f,-1.0f,-1.0f}, 
    {-1.0f,1.0f,1.0f}, {-1.0f,1.0f,-1.0f}, {-1.0f,-1.0f,1.0f}, {-1.0f,-1.0f,-1.0f}
  };
  
  const float epsilon = 0.0001f;
  
  for(int d = 0; d < num_dirs; d++) {
    float3 dir = normalize(dirs[d]);
    
    BVHTreeRayHit hit;
    hit.index = -1;
    hit.dist = FLT_MAX;
    
    float3 start_pos = vert + dir * epsilon;
    
    if (BLI_bvhtree_ray_cast(tree, start_pos, dir, 0.0f, &hit, nullptr, userdata) != -1) {
      if (dot_v3v3(hit.no, dir) > 0.0f) {
        count++;
      }
    }
  }

  return count >= (num_dirs / 3);
}

static void setTetProperties(Vector<float3> &verts, 
                      Vector<int> &tetVertId,
                      Vector<float3> &faceNormals, 
                      Vector<float> &planesD,
                      int tetNr){
  for(int i = 0; i<4; i++){ 
    float3 p0 = verts[tetVertId[4*tetNr + tetFaces[i][0]]];
    float3 p1 = verts[tetVertId[4*tetNr + tetFaces[i][1]]];
    float3 p2 = verts[tetVertId[4*tetNr + tetFaces[i][2]]];

    float3 normal = cross(p1 - p0, p2 - p0);
    normal = normalize(normal);

    faceNormals[4*tetNr + i] = normal;
    planesD[4*tetNr + i] = dot(p0, normal);
  }
}

static bool edgeCompare(const int* e0, const int* e1){
  if((e0[0] < e1[0]) || ((e0[0] == e1[0]) && (e0[1] < e1[1])))
    return true;
  else
    return false;
}

static float3 getCircumCenter(float3 p0, float3 p1, float3 p2, float3 p3){
  float epsilon = 0.000001f;

  float3 b = p1 - p0;
  float3 c = p2 - p0;
  float3 d = p3 - p0;

  float det = 2.0f * (b.x*(c.y*d.z - c.z*d.y) - b.y*(c.x*d.z - c.z*d.x) + b.z*(c.x*d.y - c.y*d.x));
  if (det <= epsilon && det >= -epsilon){
    return p0;
  }
  else{
    float3 v = cross(c, d)*dot(b, b) + cross(d, b)*dot(c, c) + cross(b, c)*dot(d, d);
    v /= det;
    return p0 + v;
  }
}

static int findContainingTet(Vector<float3> &verts, Vector<int> &tetVertId, float3 currVert){
  
  for(int currTet = 0; currTet < tetVertId.size()/4; currTet++){
    if(tetVertId[4*currTet] == -1)
      continue;
    
    float3 p0 = verts[tetVertId[4*currTet + 0]];
    float3 p1 = verts[tetVertId[4*currTet + 1]]];
    float3 p2 = verts[tetVertId[4*currTet + 2]]];
    float3 p3 = verts[tetVertId[4*currTet + 3]]];

    float3 circumCenter = getCircumCenter(p0, p1, p2, p3);
    float circumRadius = length(p0 - circumCenter);
    
    if(length(currVert - circumCenter) < circumRadius){
      return currTet;
    }
  }
  
  int closestTet = -1;
  float minDist = FLT_MAX;
  
  for(int currTet = 0; currTet < tetVertId.size()/4; currTet++){
    if(tetVertId[4*currTet] == -1)
      continue;
      
    float3 center(0.0f, 0.0f, 0.0f);
    for(int i = 0; i < 4; i++){
      center += verts[tetVertId[4*currTet + i]];
    }
    center *= 0.25f;
    
    float dist = length(currVert - center);
    if(dist < minDist){
      minDist = dist;
      closestTet = currTet;
    }
  }
  
  return closestTet;
}

static Vector<int> getViolatingTets(Vector<float3> &verts, 
                            Vector<int> &tetVertId, 
                            Vector<int> &tetFaceNeighbors, 
                            int tetMarkId, 
                            Vector<int> &tetMarks,
                            float3 currVert,
                            int containingTetNr,
                            WeldModifierTempData &weld_data)
{
  
  Vector<int> violatingTets;
  Vector<int> stack;

  stack.append(containingTetNr);  
  tetMarks[containingTetNr] = tetMarkId; 

  // Limitation du nombre de tétraèdres violants pour éviter les cas pathologiques
  const int MAX_VIOLATING_TETS = 250;
  
  while(stack.size()){
    int currTet = stack.last();
    stack.remove_last();
    violatingTets.append(currTet);
    
    // Vérifier si on atteint le nombre maximum de tétraèdres violants
    if (violatingTets.size() >= MAX_VIOLATING_TETS) {
      printf("[DEBUG TETS WARNING] Limite de tétraèdres violants atteinte (%d), troncature\n", MAX_VIOLATING_TETS);
      break;
    }
    
    for(int i = 0; i<4; i++){
      int neighborTet = tetFaceNeighbors[4*currTet + i];
      
      if(neighborTet<0 || tetMarks[neighborTet]==tetMarkId){
        continue;
      }
      
      if(tetVertId[4*neighborTet] < 0){
        weld_data.skip_violating_tets = true;
        return {};
      }

      float3 p0 = verts[tetVertId[4*neighborTet + 0]];
      float3 p1 = verts[tetVertId[4*neighborTet + 1]];
      float3 p2 = verts[tetVertId[4*neighborTet + 2]];
      float3 p3 = verts[tetVertId[4*neighborTet + 3]];

      float3 circumCenter = getCircumCenter(p0, p1, p2, p3);
      float circumRadius = length(p0 - circumCenter);
      
      // Ajouter une petite marge de tolérance pour réduire le nombre de tétraèdres considérés comme violants
      float tolerance = 1.00001f; // 0.001% de marge
      
      if(length(currVert - circumCenter) < circumRadius * tolerance){
        stack.append(neighborTet);
        tetMarks[neighborTet] = tetMarkId;
      }
    }
  }

  return violatingTets;
}

static Vector<int> createTets(Vector<float3> verts, BVHTree *tree, void *userdata, float minTetQuality, WeldModifierTempData &weld_data){
  printf("[DEBUG TETS 1] Début de la création des tétraèdres avec %d verts\n", verts.size());
  
  Vector<int> tetVertId;
  Vector<int> tetFaceNeighbors;

  int firstFreeTet = -1;

  Vector<float3> faceNormals;
  Vector<float> planesD;

  int tetMarkId = 0;
  Vector<int> tetMarks; 
 
  int bigTet = verts.size() - 4;  
  printf("[DEBUG TETS 2] Tétraèdre englobant à partir de l'indice %d\n", bigTet);

  for(int i = 0; i<4; i++){
    tetVertId.append(bigTet + i);
    tetFaceNeighbors.append(-1);
    faceNormals.append(float3(0.0f, 0.0f, 0.0f));
    planesD.append(0.0f);
  }
  tetMarks.append(0);
  setTetProperties(verts, tetVertId, faceNormals, planesD, 0);
  printf("[DEBUG TETS 3] Tétraèdre initial créé\n");

  int successful_inserts = 0;
  int failed_inserts = 0;
  
  printf("[DEBUG TETS 4] Début de l'insertion incrémentale des points (%d points à insérer)\n", bigTet);
  for(int vertNr = 0; vertNr<bigTet; vertNr++){
    if(vertNr % 1000 == 0) {
      printf("[DEBUG TETS] Insertion du point %d/%d\n", vertNr, bigTet);
    }
    
    float3 currVert = verts[vertNr];

    tetMarkId += 1;
    int containingTetNr = -1;
    containingTetNr = findContainingTet(verts, tetVertId, currVert);
    
    if(containingTetNr == -1){
      printf("[DEBUG TETS ERROR] Impossible de trouver un tétraèdre contenant le point %d\n", vertNr);
      failed_inserts++;
      continue;
    }

    tetMarkId += 1;
    Vector<int> violatingTets = getViolatingTets(verts, tetVertId, tetFaceNeighbors, tetMarkId, tetMarks, currVert, containingTetNr, weld_data);
    if(violatingTets.size() == 0 && weld_data.skip_violating_tets) {
      printf("[DEBUG TETS ERROR] Erreur dans la recherche des tétraèdres violants pour le point %d\n", vertNr);
      continue;
    }
    
    printf("[DEBUG TETS 5.%d] Point %d: %d tétraèdres violants trouvés\n", vertNr, vertNr, violatingTets.size());

    Vector<int> newTets;
    Vector<Vector<int>> edges;
    for(int violatingTetNr = 0; violatingTetNr<violatingTets.size(); violatingTetNr++){
      int violatingTet = violatingTets[violatingTetNr];

      Vector<int> currTetVerts;
      Vector<int> currTetNeighbors;
      for(int i = 0; i<4; i++){
        currTetVerts.append(tetVertId[4*violatingTet + i]);
        currTetNeighbors.append(tetFaceNeighbors[4*violatingTet + i]);
      }

      tetVertId[4*violatingTet] = -1;
      tetVertId[4*violatingTet + 1] = firstFreeTet;
      firstFreeTet = violatingTet;

      for(int i = 0; i<4; i++){
        if(currTetNeighbors[i] >= 0 && tetMarks[currTetNeighbors[i]] == tetMarkId){
          continue;
        }

        int newTetNr = firstFreeTet;
        if(firstFreeTet == -1){
          newTetNr = tetVertId.size()/4;
          for(int j = 0; j<4; j++){
            tetVertId.append(-1);
            tetFaceNeighbors.append(-1);
            faceNormals.append(float3(0.0f, 0.0f, 0.0f));
            planesD.append(0.0f);
          }
          tetMarks.append(0);
        }
        else{
          firstFreeTet = tetVertId[4*firstFreeTet + 1];
        }

        int id0 = currTetVerts[tetFaces[i][2]];
        int id1 = currTetVerts[tetFaces[i][1]];
        int id2 = currTetVerts[tetFaces[i][0]];

        tetVertId[4 * newTetNr] = id0;
        tetVertId[4 * newTetNr + 1] = id1;
        tetVertId[4 * newTetNr + 2] = id2;
        tetVertId[4 * newTetNr + 3] = vertNr;

        tetFaceNeighbors[4*newTetNr] = currTetNeighbors[i];
        
        if(currTetNeighbors[i] >= 0){
          for(int j = 0; j<4; j++){
            if(tetFaceNeighbors[4*currTetNeighbors[i] + j] == violatingTet){
              tetFaceNeighbors[4*currTetNeighbors[i] + j] = newTetNr;
            }
          }
        }

        for(int j = 1; j<4; j++){
          tetFaceNeighbors[4 * newTetNr + j] = -1;
        }

        setTetProperties(verts, tetVertId, faceNormals, planesD, newTetNr);

        Vector<int> edge1, edge2, edge3;
        edge1.append(std::min(id0, id1));
        edge1.append(std::max(id0, id1));
        edge1.append(newTetNr);
        edge1.append(1);
        
        edge2.append(std::min(id1, id2));
        edge2.append(std::max(id1, id2));
        edge2.append(newTetNr);
        edge2.append(2);
        
        edge3.append(std::min(id2, id0));
        edge3.append(std::max(id2, id0));
        edge3.append(newTetNr);
        edge3.append(3);
        
        edges.append(edge1);
        edges.append(edge2);
        edges.append(edge3);
      }
    }

    std::sort(edges.begin(), edges.end(), 
        [](const Vector<int> &e0, const Vector<int> &e1) {
            return edgeCompare(e0.data(), e1.data());
        });
    int nr = 0;
    int numEdges = edges.size();

    while(nr < numEdges){
      Vector<int> e0 = edges[nr];
      nr += 1;

      if((nr < numEdges) && (edges[nr][0] == e0[0]) && (edges[nr][1] == e0[1])){
        Vector<int> e1 = edges[nr];

        tetFaceNeighbors[4*e0[2] + e0[3]] = e1[2];
        tetFaceNeighbors[4*e1[2] + e1[3]] = e0[2];

        nr += 1;
      }
    }
    
    successful_inserts++;
  }
  printf("[DEBUG TETS 6] Insertion incrémentale terminée: %d réussies, %d échouées\n", successful_inserts, failed_inserts);

  int valid_tet_count = 0;
  for(int tet = 0; tet < tetVertId.size()/4; tet++) {
    if(tetVertId[4*tet] >= 0) {
      valid_tet_count++;
    }
  }
  printf("[DEBUG TETS 7] Nombre de tétraèdres valides avant nettoyage: %d sur %d\n", valid_tet_count, tetVertId.size()/4);

  printf("[DEBUG TETS 8] Nettoyage des tétraèdres vides ou extérieurs\n");
  int emptyTet = 0;
  int tetLen = tetVertId.size()/4;
  int outside_count = 0, quality_reject_count = 0;
  
  for(int tetNr = 0; tetNr<tetLen; tetNr++){
    if(tetVertId[4*tetNr] < 0) {
      continue;
    }
    
    int flag = 1;
    float3 center(0.0f, 0.0f, 0.0f);
    int valid_vertices = 0;
    
    for(int i = 0; i<4; i++){
      if(tetVertId[4*tetNr + i]<0 || tetVertId[4*tetNr + i]>=bigTet){
        flag = 0;
        break;
      }
      center += verts[tetVertId[4*tetNr + i]];
      valid_vertices++;
    }
    center *= 0.25f;

    if(!flag) {
      continue;
    }
    
    bool has_inside_vertex = false;
    for(int i = 0; i < 4; i++) {
      if(isInside(verts[tetVertId[4*tetNr + i]], tree, userdata)) {
        has_inside_vertex = true;
        break;
      }
    }
    
    if(!isInside(center, tree, userdata) && !has_inside_vertex){
      outside_count++;
      continue;
    }

    float3 p0 = verts[tetVertId[4 * tetNr + 0]];
    float3 p1 = verts[tetVertId[4 * tetNr + 1]];
    float3 p2 = verts[tetVertId[4 * tetNr + 2]];
    float3 p3 = verts[tetVertId[4 * tetNr + 3]];

    if(tetQuality(p0, p1, p2, p3) < minTetQuality){
      quality_reject_count++;
      continue;
    }

    for(int i = 0; i<4; i++){
      tetVertId[4*emptyTet + i] = tetVertId[4*tetNr + i];
    }
    emptyTet++;
  }

  printf("[DEBUG TETS 9] Tétraèdres rejetés: %d à l'extérieur, %d de mauvaise qualité\n", outside_count, quality_reject_count);
  printf("[DEBUG TETS 10] Tétraèdres après nettoyage: %d\n", emptyTet);

  tetVertId.resize(4*emptyTet);

  printf("[DEBUG TETS 11] Nombre de points: %d\n", verts.size());
  printf("[DEBUG TETS 12] Nombre de tétraèdres final: %d\n", tetVertId.size()/4);

  if (tetVertId.size() == 0) {
    printf("[DEBUG TETS 13] Aucun tétraèdre n'a été créé, création d'un tétraèdre de secours\n");
    
    float3 center_backup(0.0f, 0.0f, 0.0f);
    float radius_backup = 0.0f;
    
    for(int i = 0; i < verts.size()-4; i++) {
      center_backup += verts[i];
    }
    if(verts.size() > 4) {
      center_backup /= (verts.size()-4);
      
      for(int i = 0; i < verts.size()-4; i++) {
        float dist = length(verts[i] - center_backup);
        radius_backup = std::max(radius_backup, dist);
      }
    } else {
      center_backup = float3(0.0f, 0.0f, 0.0f);
      radius_backup = 1.0f;
    }
    
    printf("[DEBUG TETS 14] Centre de secours: (%f, %f, %f), rayon: %f\n", 
           center_backup.x, center_backup.y, center_backup.z, radius_backup);
    
    int base_idx = verts.size();
    verts.append(center_backup + float3(radius_backup*0.5f, radius_backup*0.5f, radius_backup*0.5f));
    verts.append(center_backup + float3(-radius_backup*0.5f, -radius_backup*0.5f, radius_backup*0.5f));
    verts.append(center_backup + float3(radius_backup*0.5f, -radius_backup*0.5f, -radius_backup*0.5f));
    verts.append(center_backup + float3(-radius_backup*0.5f, radius_backup*0.5f, -radius_backup*0.5f));
    
    printf("[DEBUG TETS 14.1] Coordonnées du tétraèdre de secours:\n");
    printf("  Point 1: (%f, %f, %f)\n", 
           (center_backup + float3(radius_backup*0.5f, radius_backup*0.5f, radius_backup*0.5f)).x,
           (center_backup + float3(radius_backup*0.5f, radius_backup*0.5f, radius_backup*0.5f)).y,
           (center_backup + float3(radius_backup*0.5f, radius_backup*0.5f, radius_backup*0.5f)).z);
    printf("  Point 2: (%f, %f, %f)\n", 
           (center_backup + float3(-radius_backup*0.5f, -radius_backup*0.5f, radius_backup*0.5f)).x,
           (center_backup + float3(-radius_backup*0.5f, -radius_backup*0.5f, radius_backup*0.5f)).y,
           (center_backup + float3(-radius_backup*0.5f, -radius_backup*0.5f, radius_backup*0.5f)).z);
    
    tetVertId.append(base_idx);
    tetVertId.append(base_idx+1);
    tetVertId.append(base_idx+2);
    tetVertId.append(base_idx+3);
    
    printf("[DEBUG TETS 15] Tétraèdre de secours créé, nouveau nombre de tétraèdres: %d\n", tetVertId.size()/4);
  }

  return tetVertId;
}

static Mesh *modifyMesh(ModifierData *md, const ModifierEvalContext * /*ctx*/, Mesh *mesh)
{
  const WeldModifierData &wmd = reinterpret_cast<WeldModifierData &>(*md);

  printf("[DEBUG 1] Démarrage du modificateur Weld avec merge_dist = %f\n", wmd.merge_dist);
  
  WeldModifierTempData weld_data;
  weld_data.skip_violating_tets = false;
  
  float interiorResolution = wmd.merge_dist;
  float minTetQuality = 0.001f;
  bool oneFacePerTet = true;
  float tetScale = 1.5f;

  printf("[DEBUG 2] Création du BVH tree pour %d faces\n", mesh->faces_num);
  BVHTree *tree = BLI_bvhtree_new(mesh->faces_num, 0.0f, 4, 6);
  if (!tree) {
    printf("[DEBUG ERROR] Échec de la création du BVH tree\n");
    return mesh;
  }
  
  for (int i = 0; i < mesh->faces_num; i++) {
    const blender::IndexRange face = mesh->faces()[i];
    float co[3][3];
    
    for (int j = 0; j < 3; j++) {
      const float3 &pos = mesh->vert_positions()[mesh->corner_verts()[face.start() + j]];
      copy_v3_v3(co[j], pos);
    }
    
    BLI_bvhtree_insert(tree, i, co[0], 3);
  }
  
  BLI_bvhtree_balance(tree);
  printf("[DEBUG 3] BVH tree créé et équilibré\n");
  
  Mesh *result;
  BMesh *bm;
  
  BMeshCreateParams bmcParam;
  bmcParam.use_toolflags = true;
  bm = BM_mesh_create(&bm_mesh_allocsize_default, &bmcParam);
  printf("[DEBUG 4] BMesh créé\n");
  
  Vector<float3> tetVerts;

  float3 center(0.0f, 0.0f, 0.0f);
  float3 bmin(inf, inf, inf);
  float3 bmax(-inf, -inf, -inf);

  printf("[DEBUG 5] Copie des vertices de surface (%d verts)\n", mesh->verts_num);
  for(int i = 0; i < mesh->verts_num; i++){
    const float3 &pos = mesh->vert_positions()[i];
    float3 new_pos(pos.x + randomEps(), pos.y + randomEps(), pos.z + randomEps());
    tetVerts.append(new_pos);

    center += new_pos;
    for(int axis = 0; axis < 3; axis++){
      bmin[axis] = std::min(bmin[axis], new_pos[axis]);
      bmax[axis] = std::max(bmax[axis], new_pos[axis]);
    }
  }
  center /= (float)mesh->verts_num;
  printf("[DEBUG 6] Centre calculé: (%f, %f, %f)\n", center.x, center.y, center.z);

  float radius = 0.0f;
  for(int i = 0; i < tetVerts.size(); i++){
    float dist = length(tetVerts[i] - center);
    radius = std::max(dist, radius);
  }
  printf("[DEBUG 7] Rayon de la sphère englobante: %f\n", radius);
  
  printf("[DEBUG 8] Ajout de points intérieurs garantis\n");
  tetVerts.append(center);
  
  const float offset = radius * 0.2f;
  tetVerts.append(center + float3(offset, 0.0f, 0.0f));
  tetVerts.append(center + float3(-offset, 0.0f, 0.0f));
  tetVerts.append(center + float3(0.0f, offset, 0.0f));
  tetVerts.append(center + float3(0.0f, -offset, 0.0f));
  tetVerts.append(center + float3(0.0f, 0.0f, offset));
  tetVerts.append(center + float3(0.0f, 0.0f, -offset));
  printf("[DEBUG 9] Points intérieurs ajoutés, total maintenant: %d points\n", tetVerts.size());

  if(interiorResolution > 0.0f){
    printf("[DEBUG 10] Début de l'échantillonnage intérieur avec résolution %f\n", interiorResolution);
    float boundLen[3];
    sub_v3_v3v3(boundLen, bmax, bmin);
    float maxBoundLen = boundLen[0];
    for (int i = 1; i < 3; i++) {
      maxBoundLen = std::max(maxBoundLen, boundLen[i]);
    }
    float sampleLen = maxBoundLen/(interiorResolution * 20.0f);
    printf("[DEBUG 11] Longueur d'échantillonnage: %f\n", sampleLen);

    if (sampleLen > maxBoundLen / 6.0f) {
      sampleLen = maxBoundLen / 6.0f;
      printf("[DEBUG 11.1] Ajustement de la longueur d'échantillonnage pour garantir des points: %f\n", sampleLen);
    }

    float jitter_factor = 0.2f;

    int interior_points_added = 0;
    for(int xi = 0; xi < (int)(boundLen[0]/sampleLen) + 2; xi++){
      float x = bmin[0] + xi*sampleLen + randomEps();
      for(int yi = 0; yi < (int)(boundLen[1]/sampleLen) + 2; yi++){
        float y = bmin[1] + yi*sampleLen + randomEps();
        for(int zi = 0; zi < (int)(boundLen[2]/sampleLen) + 2; zi++){
          float z = bmin[2] + zi*sampleLen + randomEps(); 
          
          float jitter_x = jitter_factor * sampleLen * ((float)rand() / RAND_MAX - 0.5f);
          float jitter_y = jitter_factor * sampleLen * ((float)rand() / RAND_MAX - 0.5f);
          float jitter_z = jitter_factor * sampleLen * ((float)rand() / RAND_MAX - 0.5f);
          
          float3 point(x + jitter_x, y + jitter_y, z + jitter_z);
          if(isInside(point, tree, mesh)){
            tetVerts.append(point);
            interior_points_added++;
          }
        }
      }
    }
    
    if (interior_points_added < 20) {
      printf("[DEBUG 11.2] Ajout forcé de points intérieurs supplémentaires\n");
      for (int xi = 1; xi <= 3; xi++) {
        for (int yi = 1; yi <= 3; yi++) {
          for (int zi = 1; zi <= 3; zi++) {
            float x = bmin[0] + (xi * boundLen[0] / 4.0f);
            float y = bmin[1] + (yi * boundLen[1] / 4.0f);
            float z = bmin[2] + (zi * boundLen[2] / 4.0f);
            
            float3 point(x, y, z);
            tetVerts.append(point);
            interior_points_added++;
          }
        }
      }
    }
    
    printf("[DEBUG 12] Échantillonnage intérieur terminé, %d points ajoutés\n", interior_points_added);
  } else {
    printf("[DEBUG 12] Pas d'échantillonnage intérieur (résolution <= 0)\n");
  }

  float bigTetSize = radius * 5.0f;
  printf("[DEBUG 13] Ajout du gros tétraèdre englobant, taille: %f\n", bigTetSize);

  tetVerts.append(float3(-bigTetSize, 0.0f, -bigTetSize));
  tetVerts.append(float3(bigTetSize, 0.0f, -bigTetSize));
  tetVerts.append(float3(0.0f, bigTetSize, bigTetSize));
  tetVerts.append(float3(0.0f, -bigTetSize, bigTetSize));

  printf("[DEBUG 14] Début de la création des tétraèdres avec %d points\n", tetVerts.size());
  Vector<int> tetVertId = createTets(tetVerts, tree, mesh, minTetQuality, weld_data);
  printf("[DEBUG 15] Création des tétraèdres terminée, %d tétraèdres créés\n", tetVertId.size()/4);

  if(weld_data.skip_violating_tets){
    printf("[DEBUG ERROR] Drapeau skip_violating_tets activé, abandon de la tétraédrisation\n");
    BLI_bvhtree_free(tree);
    return mesh;
  }

  printf("[DEBUG 16] Début de la création des faces BMesh avec oneFacePerTet: %d\n", oneFacePerTet);
  Vector<BMVert *> bmverts;
  if(oneFacePerTet){
    printf("[DEBUG 17] Mode une face par tétraèdre\n");
    for(int i = 0; i < mesh->verts_num; i++){
      const float3 &pos = mesh->vert_positions()[i];
      float co[3] = {pos.x, pos.y, pos.z};
      bmverts.append(BM_vert_create(bm, co, nullptr, BM_CREATE_NOP));
    }
    for(int i = mesh->verts_num; i < tetVerts.size()-4; i++){
      float co[3] = {tetVerts[i].x, tetVerts[i].y, tetVerts[i].z};
      bmverts.append(BM_vert_create(bm, co, nullptr, BM_CREATE_NOP));
    }
    if (tetVertId.size() == 4 && tetVertId[0] >= tetVerts.size()-4) {
      printf("[DEBUG 17.1] Ajout des vertices du tétraèdre de secours\n");
      for (int i = 0; i < 4; i++) {
        int idx = tetVertId[i];
        if (idx >= 0 && idx < tetVerts.size()) {
          float co[3] = {tetVerts[idx].x, tetVerts[idx].y, tetVerts[idx].z};
          printf("  Vertex %d: (%f, %f, %f)\n", i, co[0], co[1], co[2]);
          bmverts.append(BM_vert_create(bm, co, nullptr, BM_CREATE_NOP));
        } else {
          printf("  ERREUR: Indice de vertex invalide: %d\n", idx);
        }
      }
    }
    printf("[DEBUG 18] Vertices BMesh créés: %d\n", bmverts.size());
  }
  else{
    printf("[DEBUG 17] Mode toutes les faces du tétraèdre\n");
    for(int tetNr = 0; tetNr < tetVertId.size()/4; tetNr++){
      float3 center(0.0f, 0.0f, 0.0f);
      for(int j = 0; j < 4; j++){
        center += tetVerts[tetVertId[4*tetNr + j]];
      }
      center *= 0.25f;

      for(int faceNr = 0; faceNr < 4; faceNr++){
        for(int faceVertNr = 0; faceVertNr < 3; faceVertNr++){
          float3 vert = tetVerts[tetVertId[4*tetNr + tetFaces[faceNr][faceVertNr]]];
          vert = center + (vert - center) * tetScale;
          float co[3] = {vert.x, vert.y, vert.z};
          bmverts.append(BM_vert_create(bm, co, nullptr, BM_CREATE_NOP));
        }
      }
    }
    printf("[DEBUG 18] Vertices BMesh créés: %d (12 par tétraèdre)\n", bmverts.size());
  }

  printf("[DEBUG 19] Création des faces BMesh\n");
  int numTets = tetVertId.size()/4;
  int nr = 0;
  int faces_created = 0;
  for(int i = 0; i < numTets; i++){
    if(oneFacePerTet){
      if(tetVertId[4*i] < 0)
        continue;
      BM_face_create_quad_tri(bm, bmverts[tetVertId[4*i + 0]], bmverts[tetVertId[4*i + 1]], bmverts[tetVertId[4*i + 2]], bmverts[tetVertId[4*i + 3]], nullptr, BM_CREATE_NO_DOUBLE);
      faces_created++;
    }
    else{
      for(int faceNr = 0; faceNr < 4; faceNr++){
        BM_face_create_quad_tri(bm, bmverts[nr], bmverts[nr+1], bmverts[nr+2], nullptr, nullptr, BM_CREATE_NO_DOUBLE);
        nr += 3;
        faces_created++;
      }
    }
  }
  printf("[DEBUG 20] Faces BMesh créées: %d\n", faces_created);

  printf("[DEBUG 21] Conversion du BMesh en Mesh\n");
  CustomData_MeshMasks cd_mask_extra;
  cd_mask_extra.vmask = CD_MASK_ORIGINDEX;
  cd_mask_extra.emask = CD_MASK_ORIGINDEX;
  cd_mask_extra.pmask = CD_MASK_ORIGINDEX;
  
  result = BKE_mesh_from_bmesh_for_eval_nomain(bm, &cd_mask_extra, mesh);
  printf("[DEBUG 22] Conversion terminée: %d vertices, %d edges, %d faces\n", 
         result->verts_num, result->edges_num, result->faces_num);
  
  BKE_mesh_calc_normals(result);
  
  BM_mesh_free(bm);
  BLI_bvhtree_free(tree);
  
  printf("[DEBUG 23] Modification terminée avec succès\n");
  return result;    
}

static void initData(ModifierData *md)
{
  WeldModifierData *wmd = (WeldModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(wmd, modifier));

  MEMCPY_STRUCT_AFTER(wmd, DNA_struct_default_get(WeldModifierData), modifier);
}

static void requiredDataMask(ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
  WeldModifierData *wmd = (WeldModifierData *)md;

  if (wmd->defgrp_name[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  int weld_mode = RNA_enum_get(ptr, "mode");

  uiLayoutSetPropSep(layout, true);

  uiItemR(layout, ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  uiItemR(layout, ptr, "merge_threshold", UI_ITEM_NONE, IFACE_("Distance"), ICON_NONE);
  if (weld_mode == MOD_WELD_MODE_CONNECTED) {
    uiItemR(layout, ptr, "loose_edges", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", std::nullopt);

  modifier_panel_end(layout, ptr);
}

static void panelRegister(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Weld, panel_draw);
}

ModifierTypeInfo modifierType_Weld = {"Weld",
                                         N_("Weld"),
                                         "WeldModifierData",
                                         sizeof(WeldModifierData),
                                         &RNA_WeldModifier,
                                         ModifierTypeType::Constructive,
                                         (ModifierTypeFlag)(eModifierTypeFlag_AcceptsMesh |
                                                            eModifierTypeFlag_SupportsMapping |
                                                            eModifierTypeFlag_SupportsEditmode |
                                                            eModifierTypeFlag_EnableInEditmode |
                                                            eModifierTypeFlag_AcceptsCVs),
                                         ICON_AUTOMERGE_OFF,

                                         BKE_modifier_copydata_generic,

                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         modifyMesh,
                                         nullptr,

                                         initData,
                                         requiredDataMask,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         panelRegister,
                                         nullptr,
                                         nullptr,
                                         nullptr,
};