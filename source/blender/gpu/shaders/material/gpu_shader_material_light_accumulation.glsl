/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_light_accumulation(
    float4 diffuse, float4 glossy, float4 transmission, float weight, Closure &result)
{
  node_light_accumulation_impl(diffuse, glossy, transmission, weight, result);
}
