/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "common_view_lib.glsl"
#include "select_lib.glsl"

void main()
{
  fragColor = finalColor;
  lineOutput = vec4(0.0);

  select_id_output(select_id);
}
