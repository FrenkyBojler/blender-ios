/* SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/types_uint4.h"

CCL_NAMESPACE_BEGIN

#if !defined(__KERNEL_METAL__)
ccl_device_inline uint4 operator+(const uint4 a, const uint4 b)
{
  return make_uint4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
}

ccl_device_inline uint4 operator+=(uint4 &a, const uint4 b)
{
  return a = a + b;
}

ccl_device_inline uint4 operator-(const uint4 a, const uint4 b)
{
  return make_uint4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w);
}

ccl_device_inline uint4 operator-=(uint4 &a, const uint4 b)
{
  return a = a - b;
}

ccl_device_inline uint4 operator*(const uint4 a, const uint4 b)
{
  return make_uint4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w);
}

ccl_device_inline uint4 operator>>(const uint4 a, const int i)
{
  return make_uint4(a.x >> i, a.y >> i, a.z >> i, a.w >> i);
}

ccl_device_inline uint4 operator<<(const uint4 a, const int i)
{
  return make_uint4(a.x << i, a.y << i, a.z << i, a.w << i);
}

ccl_device_inline uint4 operator&(const uint4 a, const uint4 b)
{
  return make_uint4(a.x & b.x, a.y & b.y, a.z & b.z, a.w & b.w);
}

ccl_device_inline uint4 operator|(const uint4 a, const uint4 b)
{
  return make_uint4(a.x | b.x, a.y | b.y, a.z | b.z, a.w | b.w);
}

ccl_device_inline uint4 operator^(const uint4 a, const uint4 b)
{
  return make_uint4(a.x ^ b.x, a.y ^ b.y, a.z ^ b.z, a.w ^ b.w);
}
#endif

CCL_NAMESPACE_END
