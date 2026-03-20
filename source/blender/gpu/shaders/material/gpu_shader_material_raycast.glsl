/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_raycast_world(float3 position,
                        float3 direction,
                        float3 offset /*unused*/,
                        float length,
                        float &is_hit,
                        float &is_self_hit,
                        float &hit_distance,
                        float3 &hit_position,
                        float3 &hit_normal)
{
  bool hit = false;
  bool self_hit = false;
  raycast_world_eval(
      position, direction, length, false, hit, self_hit, hit_distance, hit_position, hit_normal);
  is_hit = hit ? 1.0f : 0.0f;
  is_self_hit = self_hit ? 1.0f : 0.0f;
}

[[node]]
void node_raycast_world_only_local(float3 position,
                                   float3 direction,
                                   float3 offset /*unused*/,
                                   float length,
                                   float &is_hit,
                                   float &is_self_hit,
                                   float &hit_distance,
                                   float3 &hit_position,
                                   float3 &hit_normal)
{
  bool hit = false;
  bool self_hit = false;
  raycast_world_eval(
      position, direction, length, true, hit, self_hit, hit_distance, hit_position, hit_normal);
  is_hit = hit ? 1.0f : 0.0f;
  is_self_hit = self_hit ? 1.0f : 0.0f;
}

[[node]]
void node_raycast_offset(float3 position /*unused*/,
                         float3 direction /*unused*/,
                         float3 offset,
                         float length,
                         float &is_hit,
                         float &is_self_hit,
                         float &hit_distance,
                         float3 &hit_position,
                         float3 &hit_normal)
{
  bool hit = false;
  bool self_hit = false;
  raycast_offset_eval(
      offset, length, false, hit, self_hit, hit_distance, hit_position, hit_normal);
  is_hit = hit ? 1.0f : 0.0f;
  is_self_hit = self_hit ? 1.0f : 0.0f;
}

[[node]]
void node_raycast_offset_only_local(float3 position /*unused*/,
                                    float3 direction /*unused*/,
                                    float3 offset,
                                    float length,
                                    float &is_hit,
                                    float &is_self_hit,
                                    float &hit_distance,
                                    float3 &hit_position,
                                    float3 &hit_normal)
{
  bool hit = false;
  bool self_hit = false;
  raycast_offset_eval(offset, length, true, hit, self_hit, hit_distance, hit_position, hit_normal);
  is_hit = hit ? 1.0f : 0.0f;
  is_self_hit = self_hit ? 1.0f : 0.0f;
}
