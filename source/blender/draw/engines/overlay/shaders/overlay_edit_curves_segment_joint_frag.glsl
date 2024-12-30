/* SPDX-FileCopyrightText: 2018-2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

void main()
{
  if (dot(uv_coord, uv_coord) <= 1.0) {
    fragColor = finalColor;
  }
  else {
    discard;
  }
}
