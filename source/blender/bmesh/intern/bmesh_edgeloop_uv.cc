/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "bmesh_edgeloop_uv.hh"
#include "bmesh.hh"
#include <queue>
using namespace blender;

bool UV_get_edgeloops(
    const Scene *scene,
    BMesh *bm,
    blender::Vector<blender::Vector<blender::Vector<BMLoop *>>> *edgeloops,
    blender::FunctionRef<bool(const Scene *scene, BMLoop *l, const BMUVOffsets offsets)> callback)

{
  BMVert *v;
  BMLoop *l;
  BMIter viter, liter;
  const BMUVOffsets offsets = BM_uv_map_offsets_get(bm);

  std::queue<BMLoop *> queue;
  BM_ITER_MESH (v, &viter, bm, BM_VERTS_OF_MESH) {
    BM_ITER_ELEM (l, &liter, v, BM_LOOPS_OF_VERT) {
      if (callback(scene, l, offsets) and (l->head.index != -1 and l->head.index != -2)) {
        queue.push(l);
      }
      else {
        continue;
      }
      blender::Vector<blender::Vector<BMLoop *>> edgeloop;

      /*This will be used to check whether the current continuous selection is a valid edge loop or
       * not*/

      bool valid_edgeloop = true;

      // while the queue is not empty
      while (!queue.empty()) {
        // pop the front of queue and store in queuel
        BMLoop *queuel = queue.front();
        queue.pop();

        // get UV location of queuel
        float *luv = BM_ELEM_CD_GET_FLOAT_P(queuel, offsets.uv);

        // get the vertex of queuel
        BMVert *queuev = queuel->v;

        // vector of current UV coord
        blender::Vector<BMLoop *> uvcoord;

        // iterate over the loops of the vertex
        BMLoop *currl;
        BMIter liter2;
        int selectedconnections = 0;
        BM_ITER_ELEM (currl, &liter2, queuev, BM_LOOPS_OF_VERT) {
          float *currluv = BM_ELEM_CD_GET_FLOAT_P(currl, offsets.uv);
          if (currluv[0] != luv[0] || currluv[1] != luv[1]) {
            continue;
          }
          BM_elem_index_set(currl, -2);
          uvcoord.append(currl);

          if (callback(scene, currl->next, offsets)) {
            selectedconnections++;
            if (currl->next->head.index != -1 and currl->next->head.index != -2) {
              queue.push(currl->next);
            }
          }
          if (callback(scene, currl->prev, offsets)) {
            selectedconnections++;
            if (currl->prev->head.index != -1 and currl->prev->head.index != -2) {
              queue.push(currl->prev);
            }
          }
        }
        if (selectedconnections > 2 || selectedconnections < 1) {
          valid_edgeloop = false;
        }
        else if (selectedconnections == 1) {
          for (BMLoop *endpointloop : uvcoord) {
            BM_elem_index_set(endpointloop, -1);
          }
        }
        if (valid_edgeloop) {
          edgeloop.append(uvcoord);
        }
      }
      if (edgeloop.size() != 0 && valid_edgeloop) {
        edgeloops->append(edgeloop);
      }
    }
  }
  return true;
}
