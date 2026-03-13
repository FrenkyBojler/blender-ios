/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void invert_rotation(float4 rotation, float4 &result)
{
  result = float4(rotation.x, -rotation.y, -rotation.z, -rotation.w);
}
