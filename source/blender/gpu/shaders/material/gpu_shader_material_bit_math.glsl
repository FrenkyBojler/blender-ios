/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void bit_math_and(float a, float b, out float result)
{
  result = float(int(a) & int(b));
}

[[node]]
void bit_math_or(float a, float b, out float result)
{
  result = float(int(a) | int(b));
}

[[node]]
void bit_math_xor(float a, float b, out float result)
{
  result = float(int(a) ^ int(b));
}

[[node]]
void bit_math_not(float a, out float result)
{
  result = float(~int(a));
}
