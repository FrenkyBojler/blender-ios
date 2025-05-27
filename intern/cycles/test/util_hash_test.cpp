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
    EXPECT_NEAR(c.x, 0.14955954253673553f, 1.0e-9f);
    EXPECT_NEAR(c.y, 0.15225376188755035f, 1.0e-9f);
  }
  {
    const float2 c = hash_float2_to_float2(make_float2(1.5f, -0.1f));
    EXPECT_NEAR(c.x, 0.19543434679508209f, 1.0e-9f);
    EXPECT_NEAR(c.y, 0.67111784219741821f, 1.0e-9f);
  }
}

TEST(hash, float3_to_float3)
{
  {
    const float3 c = hash_float3_to_float3(make_float3(0, 1, 2));
    EXPECT_NEAR(c.x, 0.8184969425201416f, 1.0e-9f);
    EXPECT_NEAR(c.y, 0.39474216103553772f, 1.0e-9f);
    EXPECT_NEAR(c.z, 0.2105463445186615f, 1.0e-9f);
  }
  {
    const float3 c = hash_float3_to_float3(make_float3(1.5f, -0.1f, 12345.67f));
    EXPECT_NEAR(c.x, 0.12775042653083801f, 1.0e-9f);
    EXPECT_NEAR(c.y, 0.085053838789463043f, 1.0e-9f);
    EXPECT_NEAR(c.z, 0.81869912147521973f, 1.0e-9f);
  }
}

TEST(hash, float4_to_float4)
{
  {
    const float4 c = hash_float4_to_float4(make_float4(0, 1, 2, 3));
    EXPECT_NEAR(c.x, 0.3785756528377533f, 1.0e-9f);
    EXPECT_NEAR(c.y, 0.10118526220321655f, 1.0e-9f);
    EXPECT_NEAR(c.z, 0.23767852783203125f, 1.0e-9f);
    EXPECT_NEAR(c.w, 0.72035640478134155f, 1.0e-9f);
  }
  {
    const float4 c = hash_float4_to_float4(make_float4(1.5f, -0.1f, 12345.67f, -310.73046875f));
    EXPECT_NEAR(c.x, 0.58864277601242065f, 1.0e-9f);
    EXPECT_NEAR(c.y, 0.91455036401748657f, 1.0e-9f);
    EXPECT_NEAR(c.z, 0.44810330867767334f, 1.0e-9f);
    EXPECT_NEAR(c.w, 0.99361437559127808f, 1.0e-9f);
  }
}

CCL_NAMESPACE_END
