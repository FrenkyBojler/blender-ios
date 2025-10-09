/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

void main()
{
  imageAtomicXor(img_atomic_2D, int2(0), uint(1u << gl_LocalInvocationIndex));
  imageAtomicXor(img_atomic_2D_array, int3(0, 0, 0), uint(1u << gl_LocalInvocationIndex));
  imageAtomicXor(img_atomic_2D_array, int3(0, 0, 1), uint(1u << gl_LocalInvocationIndex));
  imageAtomicXor(img_atomic_3D, int3(0, 0, 0), uint(1u << gl_LocalInvocationIndex));
  imageAtomicXor(img_atomic_3D, int3(0, 0, 1), uint(1u << gl_LocalInvocationIndex));
}
