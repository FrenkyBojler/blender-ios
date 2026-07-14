/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void node_light_accumulation(
    float4 diffuse, float4 glossy, float4 transmission, float weight, Closure &result)
{
  // node_light_accumulation_impl(diffuse, glossy, transmission, weight, result);
  diffuse = max(diffuse, float4(0.0f));
  glossy = max(glossy, float4(0.0f));
  transmission = max(transmission, float4(0.0f));

  ClosureEmission emission_data;
  emission_data.emission = (diffuse + glossy + transmission).rgb * weight;

  result = closure_eval(emission_data);
}
