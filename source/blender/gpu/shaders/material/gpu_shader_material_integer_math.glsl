/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"

#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_safe_lib.glsl"

[[node]]
void integer_math_add(float a, float b, float c, float &result)
{
  result = float(int(a) + int(b));
}

[[node]]
void integer_math_subtract(float a, float b, float c, float &result)
{
  result = float(int(a) - int(b));
}

[[node]]
void integer_math_multiply(float a, float b, float c, float &result)
{
  result = float(int(a) * int(b));
}

[[node]]
void integer_math_divide(float a, float b, float c, float &result)
{
  int int_a = int(a);
  int int_b = int(b);
  if (int_b != 0) {
    result = float(int_a / int_b);
  }
  else {
    result = 0.0f;
  }
}

[[node]]
void integer_math_divide_floor(float a, float b, float c, float &result)
{
  int int_a = int(a);
  int int_b = int(b);
  if (int_b != 0) {
    result = floor(float(int_a) / float(int_b));
  }
  else {
    result = 0.0f;
  }
}

[[node]]
void integer_math_divide_ceil(float a, float b, float c, float &result)
{
  int int_a = int(a);
  int int_b = int(b);
  if (int_b != 0) {
    result = ceil(float(int_a) / float(int_b));
  }
  else {
    result = 0.0f;
  }
}

[[node]]
void integer_math_divide_round(float a, float b, float c, float &result)
{
  int int_a = int(a);
  int int_b = int(b);
  if (int_b != 0) {
    result = floor(float(int_a) / float(int_b) + 0.5f);
  }
  else {
    result = 0.0f;
  }
}

[[node]]
void integer_math_power(float a, float b, float c, float &result)
{
  int int_a = int(a);
  int int_b = int(b);
  
  if (int_a == 0 && int_b <= -1) {
    result = 0.0f;
  }
  else {
    result = float(int(compatible_pow(float(int_a), float(int_b))));
  }
}

[[node]]
void integer_math_multiply_add(float a, float b, float c, float &result)
{
  int int_a = int(a);
  int int_b = int(b);
  int int_c = int(c);
  result = float(int_a * int_b + int_c);
}

[[node]]
void integer_math_floored_modulo(float a, float b, float c, float &result)
{
  int int_a = int(a);
  int int_b = int(b);
  result = (int_b != 0) ? (float(int_a) - float(int_b) * floor(float(int_a) / float(int_b))) : 0.0f;
}

[[node]]
void integer_math_modulo(float a, float b, float c, float &result)
{
  result = compatible_mod(float(int(a)), float(int(b)));
}

[[node]]
void integer_math_absolute(float a, float b, float c, float &result)
{
  result = abs(float(int(a)));
}

[[node]]
void integer_math_sign(float a, float b, float c, float &result)
{
 result = sign(float(int(a)));
}

[[node]]
void integer_math_minimum(float a, float b, float c, float &result)
{
  result = min(float(int(a)), float(int(b)));
}

[[node]]
void integer_math_maximum(float a, float b, float c, float &result)
{
  result = max(float(int(a)), float(int(b)));
}

[[node]]
void integer_math_gcd(float a, float b, float c, float &result)
{
  int int_a = int(abs(a));
  int int_b = int(abs(b));
  while (int_b != 0) {
    int remainder = int_a % int_b;
    int_a = int_b;
    int_b = remainder;
  }
  result = float(int_a);
}

[[node]]
void integer_math_lcm(float a, float b, float c, float &result)
{
  int int_a = int(abs(a));
  int int_b = int(abs(b));
  if (int_a != 0 && int_b != 0) {
    int temp_a = int_a;
    int temp_b = int_b;
    while (temp_b != 0) {
      int remainder = temp_a % temp_b;
      temp_a = temp_b;
      temp_b = remainder;
    }
    result = float(int_a / temp_a * int_b);
  }
  else {
    result = 0.0f;
  }
}

[[node]]
void integer_math_negate(float a, float b, float c, float &result)
{
  result = -1.0f * float(int(a));
}