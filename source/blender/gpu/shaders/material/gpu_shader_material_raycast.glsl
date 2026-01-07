/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_raycast(float3 position,
                  float3 direction,
                  float length,
                  float &is_hit,
                  float &hit_distance,
                  float3 &hit_position,
                  float3 &hit_normal)
{
  bool hit = false;
  raycast_eval(position, direction, length, false, hit, hit_distance, hit_position, hit_normal);
  is_hit = hit ? 1.0f : 0.0f;
}

[[node]]
void node_raycast_only_local(float3 position,
                             float3 direction,
                             float length,
                             float &is_hit,
                             float &hit_distance,
                             float3 &hit_position,
                             float3 &hit_normal)
{
  bool hit = false;
  raycast_eval(position, direction, length, true, hit, hit_distance, hit_position, hit_normal);
  is_hit = hit ? 1.0f : 0.0f;
}
