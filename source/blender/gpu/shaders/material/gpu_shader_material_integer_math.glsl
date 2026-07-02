/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void integer_math_add(float a, float b, out float result)
{
  result = float(int(a) + int(b));
}

[[node]]
void integer_math_subtract(float a, float b, out float result)
{
  result = float(int(a) - int(b));
}

[[node]]
void integer_math_multiply(float a, float b, out float result)
{
  result = float(int(a) * int(b));
}

[[node]]
void integer_math_divide(float a, float b, out float result)
{
  result = float((int(b) != 0) ? (int(a) / int(b)) : 0);
}

[[node]]
void integer_math_power(float base, float exponent, out float result)
{
  result = float(int(pow(int(base), int(exponent))));
}

[[node]]
void integer_math_multiply_add(float a, float b, float c, out float result)
{
  result = float(int(a) * int(b) + int(c));
}

[[node]]
void integer_math_absolute(float a, out float result)
{
  result = float(abs(int(a)));
}

[[node]]
void integer_math_negate(float a, out float result)
{
  result = float(-int(a));
}
