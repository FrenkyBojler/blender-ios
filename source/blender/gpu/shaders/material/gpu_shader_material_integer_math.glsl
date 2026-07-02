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
  const int ib = int(b);
  result = float((ib != 0) ? (int(a) / ib) : 0);
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
void integer_math_sign(float a, out float result)
{
  const int ia = int(a);
  result = float(int(ia > 0) - int(ia < 0));
}

[[node]]
void integer_math_minimum(float a, float b, out float result)
{
  result = float(min(int(a), int(b)));
}

[[node]]
void integer_math_maximum(float a, float b, out float result)
{
  result = float(max(int(a), int(b)));
}

[[node]]
void integer_math_negate(float a, out float result)
{
  result = float(-int(a));
}
