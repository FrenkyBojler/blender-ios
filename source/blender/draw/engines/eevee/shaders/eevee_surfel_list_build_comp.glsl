/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Read the result of the sorted buffer and update the `prev` and `next` surfel id inside each
 * surfel structure. This step also transform the linked list into a graph in order to avoid lost
 * energy from almost coplanar surfaces.
 *
 * Dispatched as 1 thread per list.
 */

#include "infos/eevee_lightprobe_volume_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_surfel_list_build)

#include "gpu_shader_index_range_lib.glsl"

/**
 * Return true if link from `surfel[a]` to `surfel[b]` is valid.
 * WARNING: this function is not commutative : `f(a, b) != f(b, a)`
 */
bool is_valid_surfel_link(int a, int b)
{
  float3 link_vector = normalize(surfel_buf[b].position - surfel_buf[a].position);
  float link_angle_cos = dot(surfel_buf[a].normal, link_vector);
  bool is_coplanar = abs(link_angle_cos) < 1.0e-3f;
  return !is_coplanar;
}

void main()
{
  int list_id = int(gl_GlobalInvocationID.x);
  if (list_id >= list_info_buf.list_max) {
    return;
  }

  IndexRange list_range = IndexRange(list_range_buf[list_id * 2 + 0],
                                     list_range_buf[list_id * 2 + 1]);

  const int first_item = list_range.start();
  const int last_item = list_range.last();

  const int sorted_list_first = sorted_surfel_id_buf[first_item];
  /* Update surfels linked list. */
  int prev = -1;
  int curr = sorted_surfel_id_buf[first_item];
  for (int i = first_item; i <= last_item; i++) {
    int next = (i == last_item) ? -1 : sorted_surfel_id_buf[i + 1];
    surfel_buf[curr].next = next;
    surfel_buf[curr].prev = prev;
    curr = next;
    prev = curr;
  }

  /* Update list start for irradiance sample capture. */
  list_start_buf[list_id] = sorted_list_first;

  /* Now that we have a sorted list, try to avoid connection from coplanar surfels.
   * For that we disconnect them and link them to the first non-coplanar surfel.
   * Note that this changes the list to a tree, which doesn't affect the rest of the algorithm.
   *
   * This is a really important step since it allows to clump more surfels into one ray list and
   * avoid light leaking through surfaces. If we don't disconnect coplanar surfels, we loose many
   * good rays by evaluating null radiance transfer between the coplanar surfels for rays that
   * are not directly perpendicular to the surface. */

  /* Mutable `foreach`. */
  for (int i = sorted_list_first, next = 0; i > -1; i = next) {
    next = surfel_buf[i].next;

    int valid_next = surfel_buf[i].next;
    int valid_prev = surfel_buf[i].prev;

    /* Search the list for the first valid next and previous surfel. */
    while (valid_next > -1) {
      if (is_valid_surfel_link(i, valid_next)) {
        break;
      }
      valid_next = surfel_buf[valid_next].next;
    }
    while (valid_prev > -1) {
      if (is_valid_surfel_link(i, valid_prev)) {
        break;
      }
      valid_prev = surfel_buf[valid_prev].prev;
    }

    surfel_buf[i].next = valid_next;
    surfel_buf[i].prev = valid_prev;
  }
}
