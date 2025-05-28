/* SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/types_float3.h"
#include "util/types_uint3.h"

CCL_NAMESPACE_BEGIN

#ifndef __KERNEL_GPU__
ccl_device_inline uint3 operator+(const uint3 a, const uint3 b)
{
  return make_uint3(a.x + b.x, a.y + b.y, a.z + b.z);
}

ccl_device_inline uint3 operator+=(uint3 &a, const uint3 b)
{
  return a = a + b;
}

ccl_device_inline uint3 operator-(const uint3 a, const uint3 b)
{
  return make_uint3(a.x - b.x, a.y - b.y, a.z - b.z);
}

ccl_device_inline uint3 operator-=(uint3 &a, const uint3 b)
{
  return a = a - b;
}

ccl_device_inline uint3 operator*(const uint3 a, const uint3 b)
{
  return make_uint3(a.x * b.x, a.y * b.y, a.z * b.z);
}

ccl_device_inline uint3 operator>>(const uint3 a, const int i)
{
  return make_uint3(a.x >> i, a.y >> i, a.z >> i);
}

ccl_device_inline uint3 operator<<(const uint3 a, const int i)
{
  return make_uint3(a.x << i, a.y << i, a.z << i);
}

ccl_device_inline uint3 operator&(const uint3 a, const uint3 b)
{
  return make_uint3(a.x & b.x, a.y & b.y, a.z & b.z);
}

ccl_device_inline uint3 operator|(const uint3 a, const uint3 b)
{
  return make_uint3(a.x | b.x, a.y | b.y, a.z | b.z);
}

ccl_device_inline uint3 operator^(const uint3 a, const uint3 b)
{
  return make_uint3(a.x ^ b.x, a.y ^ b.y, a.z ^ b.z);
}
#endif

CCL_NAMESPACE_END
