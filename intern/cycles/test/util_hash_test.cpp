/* SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#if defined(__x86_64__) || defined(_M_X64)
#  define __KERNEL_SSE__
#  define __KERNEL_SSE2__
#  define __KERNEL_SSE3__
#  define __KERNEL_SSSE3__
#  define __KERNEL_SSE42__
#endif

#include "util/hash.h"

CCL_NAMESPACE_BEGIN

TEST(hash, float2_to_float2)
{
  {
    const float2 c = hash_float2_to_float2(make_float2(0, 1));
    EXPECT_NEAR(c.x, 0.94104540348052979f, 1.0e-9f);
    EXPECT_NEAR(c.y, 0.57971519231796265f, 1.0e-9f);
  }
  {
    const float2 c = hash_float2_to_float2(make_float2(1.5f, -0.1f));
    EXPECT_NEAR(c.x, 0.36130267381668091f, 1.0e-9f);
    EXPECT_NEAR(c.y, 0.47897535562515259f, 1.0e-9f);
  }
}

TEST(hash, float3_to_float3)
{
  {
    const float3 c = hash_float3_to_float3(make_float3(0, 1, 2));
    EXPECT_NEAR(c.x, 0.84405595064163208f, 1.0e-9f);
    EXPECT_NEAR(c.y, 0.10765662044286728f, 1.0e-9f);
    EXPECT_NEAR(c.z, 0.32112202048301697f, 1.0e-9f);
  }
  {
    const float3 c = hash_float3_to_float3(make_float3(1.5f, -0.1f, 12345.67f));
    EXPECT_NEAR(c.x, 0.086377404630184174f, 1.0e-9f);
    EXPECT_NEAR(c.y, 0.66652321815490723f, 1.0e-9f);
    EXPECT_NEAR(c.z, 0.30510097742080688f, 1.0e-9f);
  }
}

TEST(hash, float4_to_float4)
{
  {
    const float4 c = hash_float4_to_float4(make_float4(0, 1, 2, 3));
    EXPECT_NEAR(c.x, 0.84419256448745728f, 1.0e-9f);
    EXPECT_NEAR(c.y, 0.94662964344024658f, 1.0e-9f);
    EXPECT_NEAR(c.z, 0.5574309229850769f, 1.0e-9f);
    EXPECT_NEAR(c.w, 0.98514074087142944f, 1.0e-9f);
  }
  {
    const float4 c = hash_float4_to_float4(make_float4(1.5f, -0.1f, 12345.67f, -310.73046875f));
    EXPECT_NEAR(c.x, 0.2290133535861969f, 1.0e-9f);
    EXPECT_NEAR(c.y, 0.95477765798568726f, 1.0e-9f);
    EXPECT_NEAR(c.z, 0.40136802196502686f, 1.0e-9f);
    EXPECT_NEAR(c.w, 0.65203464031219482f, 1.0e-9f);
  }
}

CCL_NAMESPACE_END
