/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]] 
void boolean_math_and(float a, float b, out float result)
{
  result = float(a != 0.0f && b != 0.0f);
}

[[node]] 
void boolean_math_or(float a, float b, out float result)
{
  result = float(a != 0.0f || b != 0.0f);
}

[[node]] 
void boolean_math_not(float a, float b, out float result)
{
  result = float(a == 0.0f);
}

[[node]] 
void boolean_math_nand(float a, float b, out float result)
{
  result = float(!(a != 0.0f && b != 0.0f));
}

[[node]] 
void boolean_math_nor(float a, float b, out float result)
{
  result = float(a == 0.0f && b == 0.0f);
}

[[node]] 
void boolean_math_xnor(float a, float b, out float result)
{
  result = float((a != 0.0f) == (b != 0.0f));
}

[[node]] 
void boolean_math_xor(float a, float b, out float result)
{
  result = float((a != 0.0f) != (b != 0.0f));
}

[[node]] 
void boolean_math_imply(float a, float b, out float result)
{
  result = float(a == 0.0f || b != 0.0f);
}

[[node]] 
void boolean_math_nimply(float a, float b, out float result)
{
  result = float(a != 0.0f && b == 0.0f);
}