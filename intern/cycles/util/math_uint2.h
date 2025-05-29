/* SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/types_uint2.h"

CCL_NAMESPACE_BEGIN

#if !defined(__KERNEL_METAL__)
ccl_device_inline uint2 operator+(const uint2 a, const uint2 b)
{
  return make_uint2(a.x + b.x, a.y + b.y);
}

ccl_device_inline uint2 operator+=(uint2 &a, const uint2 b)
{
  return a = a + b;
}

ccl_device_inline uint2 operator-(const uint2 a, const uint2 b)
{
  return make_uint2(a.x - b.x, a.y - b.y);
}

ccl_device_inline uint2 operator-=(uint2 &a, const uint2 b)
{
  return a = a - b;
}

ccl_device_inline uint2 operator*(const uint2 a, const uint2 b)
{
  return make_uint2(a.x * b.x, a.y * b.y);
}

ccl_device_inline uint2 operator>>(const uint2 a, const int i)
{
  return make_uint2(a.x >> i, a.y >> i);
}

ccl_device_inline uint2 operator<<(const uint2 a, const int i)
{
  return make_uint2(a.x << i, a.y << i);
}

ccl_device_inline uint2 operator&(const uint2 a, const uint2 b)
{
  return make_uint2(a.x & b.x, a.y & b.y);
}

ccl_device_inline uint2 operator|(const uint2 a, const uint2 b)
{
  return make_uint2(a.x | b.x, a.y | b.y);
}

ccl_device_inline uint2 operator^(const uint2 a, const uint2 b)
{
  return make_uint2(a.x ^ b.x, a.y ^ b.y);
}
#endif

CCL_NAMESPACE_END
