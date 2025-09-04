/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Directive for resetting the line numbering so the failing tests lines can be printed.
 * This conflict with the shader compiler error logging scheme.
 * Comment out for correct compilation error line. */
#if 1 /* WORKAROUND: GLSL shader compilation mutate line directives searching `#line` pattern. */
#  line 10
#endif

#include "eevee_gbuffer_write_lib.glsl"
#include "gpu_shader_test_lib.glsl"

#define TEST(a, b) if (true)
// #define EXPECT_EQ(a, b) a
// #define EXPECT_EQ(a, b) a
// #define EXPECT_NEAR(a, b, c) a

gbuffer::InputClosures gbuffer_new()
{
  gbuffer::InputClosures data;
  data.closure[0].type = CLOSURE_NONE_ID;
  data.closure[0].weight = 0.0f;
  data.closure[1].type = CLOSURE_NONE_ID;
  data.closure[1].weight = 0.0f;
  data.closure[2].type = CLOSURE_NONE_ID;
  data.closure[2].weight = 0.0f;
  return data;
}

void main()
{
  gbuffer::InputClosures data_in;
  float3 Ng = float3(1.0f, 0.0f, 0.0f);
  float3 N = Ng;
  float thickness = 0.2f;

  TEST(eevee_gbuffer, ClosureDiffuse)
  {
    data_in = gbuffer_new();
    data_in.closure[1].type = CLOSURE_BSDF_DIFFUSE_ID;
    data_in.closure[1].weight = 1.0f;
    data_in.closure[1].color = float3(0.1f, 0.2f, 0.3f);
    data_in.closure[1].N = normalize(float3(0.2f, 0.1f, 0.3f));

    gbuffer::Packed data_out = gbuffer::pack(data_in, Ng, N, thickness, false);
    gbuffer::Header header = gbuffer::Header::from_data(data_out.header);

    EXPECT_EQ(uint(data_out.used_layers), 0u);
    EXPECT_EQ(uint3(header.empty_bins()), uint3(1, 0, 1));
    EXPECT_EQ(header.closure_len(), 1);

    ClosureUndetermined out_diffuse;
    out_diffuse.type = gbuffer::mode_to_closure_type(header.bin_type(1));
    out_diffuse.color = gbuffer::closure_color_unpack(data_out.closure[0]);
    out_diffuse.N = gbuffer::normal_unpack(data_out.normal[0]);

    EXPECT_EQ(out_diffuse.type, CLOSURE_BSDF_DIFFUSE_ID);
    EXPECT_NEAR(out_diffuse.color, data_in.closure[1].color, 1e-5f);
    EXPECT_NEAR(out_diffuse.N, data_in.closure[1].N, 1e-5f);
  }

  TEST(eevee_gbuffer, ClosureSubsurface)
  {
    data_in = gbuffer_new();
    data_in.closure[0].type = CLOSURE_BSSRDF_BURLEY_ID;
    data_in.closure[0].weight = 1.0f;
    data_in.closure[0].color = float3(0.1f, 0.2f, 0.3f);
    data_in.closure[0].data.rgb = float3(0.2f, 0.3f, 0.4f);
    data_in.closure[0].N = normalize(float3(0.2f, 0.1f, 0.3f));

    gbuffer::Packed data_out = gbuffer::pack(data_in, Ng, N, thickness, false);
    gbuffer::Header header = gbuffer::Header::from_data(data_out.header);

    EXPECT_EQ(uint(data_out.used_layers), ADDITIONAL_DATA);
    EXPECT_EQ(uint3(header.empty_bins()), uint3(1, 1, 0));
    EXPECT_EQ(header.closure_len(), 1);

    ClosureUndetermined out_sss_burley;
    out_sss_burley.type = gbuffer::mode_to_closure_type(header.bin_type(1));
    out_sss_burley.color = gbuffer::closure_color_unpack(data_out.closure[0]);
    out_sss_burley.N = gbuffer::normal_unpack(data_out.normal[0]);
    gbuffer::Subsurface::unpack_additional(out_sss_burley, data_out.closure[1]);

    EXPECT_EQ(uint(data_out.used_layers), 0u);
    EXPECT_EQ(uint3(header.empty_bins()), uint3(1, 0, 1));
    EXPECT_EQ(header.closure_len(), 1);

    EXPECT_EQ(out_sss_burley.type, CLOSURE_BSSRDF_BURLEY_ID);
    EXPECT_EQ(data_in.closure[0].type, CLOSURE_BSSRDF_BURLEY_ID);
    EXPECT_NEAR(data_in.closure[0].color, out_sss_burley.color, 1e-5f);
    EXPECT_NEAR(data_in.closure[0].N, out_sss_burley.N, 1e-5f);
    EXPECT_NEAR(
        data_in.closure[0].data.rgb, to_closure_subsurface(out_sss_burley).sss_radius, 1e-5f);
  }

#if 0
  TEST(eevee_gbuffer, ClosureTranslucent)
  {
    data_in = gbuffer_new();
    data_in.closure[0].type = CLOSURE_BSDF_TRANSLUCENT_ID;
    data_in.closure[0].weight = 1.0f;
    data_in.closure[0].color = float3(0.1f, 0.2f, 0.3f);
    data_in.closure[0].N = normalize(float3(0.2f, 0.1f, 0.3f));

    g_data_packed = gbuffer_pack(data_in, Ng);
    data_out = gbuffer_read(header_tx, closure_tx, normal_tx, int2(0));

    EXPECT_EQ(g_data_packed.data_len, 1);
    EXPECT_EQ(data_out.closure_count, 1);

    ClosureUndetermined out_translucent = gbuffer_closure_get(data_out, 0);

    EXPECT_EQ(out_translucent.type, CLOSURE_BSDF_TRANSLUCENT_ID);
    EXPECT_EQ(data_in.closure[0].type, CLOSURE_BSDF_TRANSLUCENT_ID);
    EXPECT_NEAR(data_in.closure[0].color, out_translucent.color, 1e-5f);
    EXPECT_NEAR(data_in.closure[0].N, out_translucent.N, 1e-5f);
  }

  TEST(eevee_gbuffer, ClosureReflection)
  {
    data_in = gbuffer_new();
    data_in.closure[0].type = CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID;
    data_in.closure[0].weight = 1.0f;
    data_in.closure[0].color = float3(0.1f, 0.2f, 0.3f);
    data_in.closure[0].data.x = 0.4f;
    data_in.closure[0].N = normalize(float3(0.2f, 0.1f, 0.3f));

    g_data_packed = gbuffer_pack(data_in, Ng);
    data_out = gbuffer_read(header_tx, closure_tx, normal_tx, int2(0));

    EXPECT_EQ(g_data_packed.data_len, 2);
    EXPECT_EQ(data_out.closure_count, 1);

    ClosureUndetermined out_reflection = gbuffer_closure_get(data_out, 0);

    EXPECT_EQ(out_reflection.type, CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID);
    EXPECT_EQ(data_in.closure[0].type, CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID);
    EXPECT_NEAR(data_in.closure[0].color, out_reflection.color, 1e-5f);
    EXPECT_NEAR(data_in.closure[0].N, out_reflection.N, 1e-5f);
    EXPECT_NEAR(data_in.closure[0].data.r, out_reflection.data.r, 1e-5f);
  }

  TEST(eevee_gbuffer, ClosureRefraction)
  {
    data_in = gbuffer_new();
    data_in.closure[0].type = CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID;
    data_in.closure[0].weight = 1.0f;
    data_in.closure[0].color = float3(0.1f, 0.2f, 0.3f);
    data_in.closure[0].data.x = 0.4f;
    data_in.closure[0].data.y = 0.5f;
    data_in.closure[0].N = normalize(float3(0.2f, 0.1f, 0.3f));

    g_data_packed = gbuffer_pack(data_in, Ng);
    data_out = gbuffer_read(header_tx, closure_tx, normal_tx, int2(0));

    EXPECT_EQ(g_data_packed.data_len, 2);
    EXPECT_EQ(data_out.closure_count, 1);

    ClosureUndetermined out_refraction = gbuffer_closure_get(data_out, 0);

    EXPECT_EQ(out_refraction.type, CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID);
    EXPECT_EQ(data_in.closure[0].type, CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID);
    EXPECT_NEAR(data_in.closure[0].color, out_refraction.color, 1e-5f);
    EXPECT_NEAR(data_in.closure[0].N, out_refraction.N, 1e-5f);
    EXPECT_NEAR(data_in.closure[0].data.r, out_refraction.data.r, 1e-5f);
    EXPECT_NEAR(data_in.closure[0].data.g, out_refraction.data.g, 1e-5f);
  }

  TEST(eevee_gbuffer, ClosureCombination)
  {
    ClosureUndetermined in_cl0 = closure_new(CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID);
    in_cl0.weight = 1.0f;
    in_cl0.color = float3(0.1f, 0.2f, 0.3f);
    in_cl0.data.x = 0.4f;
    in_cl0.data.y = 0.5f;
    in_cl0.N = normalize(float3(0.2f, 0.1f, 0.3f));

    ClosureUndetermined in_cl1 = closure_new(CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID);
    in_cl1.weight = 1.0f;
    in_cl1.color = float3(0.4f, 0.5f, 0.6f);
    in_cl1.data.x = 0.6f;
    in_cl1.N = normalize(float3(0.2f, 0.3f, 0.4f));

    data_in = gbuffer_new();
    data_in.closure[0] = in_cl0;
    data_in.closure[1] = in_cl1;

    g_data_packed = gbuffer_pack(data_in, Ng);

    EXPECT_EQ(g_data_packed.data_len, 4);

    data_out = gbuffer_read(header_tx, closure_tx, normal_tx, int2(0));

    EXPECT_EQ(data_out.closure_count, 2);

    ClosureUndetermined out_cl0 = gbuffer_closure_get(data_out, 0);
    ClosureUndetermined out_cl1 = gbuffer_closure_get(data_out, 1);

    EXPECT_EQ(out_cl0.type, CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID);
    EXPECT_EQ(in_cl0.type, CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID);
    EXPECT_NEAR(in_cl0.color, out_cl0.color, 1e-5f);
    EXPECT_NEAR(in_cl0.N, out_cl0.N, 1e-5f);
    EXPECT_NEAR(in_cl0.data.r, out_cl0.data.r, 1e-5f);
    EXPECT_NEAR(in_cl0.data.g, out_cl0.data.g, 1e-5f);

    EXPECT_EQ(out_cl1.type, CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID);
    EXPECT_EQ(in_cl1.type, CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID);
    EXPECT_NEAR(in_cl1.color, out_cl1.color, 1e-5f);
    EXPECT_NEAR(in_cl1.N, out_cl1.N, 1e-5f);
    EXPECT_NEAR(in_cl1.data.r, out_cl1.data.r, 1e-5f);
  }

  TEST(eevee_gbuffer, ClosureColorless)
  {
    data_in = gbuffer_new();
    data_in.closure[0].type = CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID;
    data_in.closure[0].weight = 1.0f;
    data_in.closure[0].color = float3(0.1f, 0.1f, 0.1f);
    data_in.closure[0].data.x = 0.4f;
    data_in.closure[0].data.y = 0.5f;
    data_in.closure[0].N = normalize(float3(0.2f, 0.1f, 0.3f));

    data_in.closure[1].type = CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID;
    data_in.closure[1].weight = 1.0f;
    data_in.closure[1].color = float3(0.1f, 0.1f, 0.1f);
    data_in.closure[1].data.x = 0.4f;
    data_in.closure[1].N = normalize(float3(0.2f, 0.3f, 0.4f));

    g_data_packed = gbuffer_pack(data_in, Ng);

    EXPECT_EQ(g_data_packed.data_len, 2);

    data_out = gbuffer_read(header_tx, closure_tx, normal_tx, int2(0));

    EXPECT_EQ(data_out.closure_count, 2);

    ClosureUndetermined out_reflection = gbuffer_closure_get(data_out, 1);
    ClosureUndetermined out_refraction = gbuffer_closure_get(data_out, 0);

    EXPECT_EQ(out_refraction.type, CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID);
    EXPECT_EQ(data_in.closure[0].type, CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID);
    EXPECT_NEAR(data_in.closure[0].color, out_refraction.color, 1e-5f);
    EXPECT_NEAR(data_in.closure[0].N, out_refraction.N, 1e-5f);
    EXPECT_NEAR(data_in.closure[0].data.r, out_refraction.data.r, 1e-5f);
    EXPECT_NEAR(data_in.closure[0].data.g, out_refraction.data.g, 1e-5f);

    EXPECT_EQ(out_reflection.type, CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID);
    EXPECT_EQ(data_in.closure[1].type, CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID);
    EXPECT_NEAR(data_in.closure[1].color, out_reflection.color, 1e-5f);
    EXPECT_NEAR(data_in.closure[1].N, out_reflection.N, 1e-5f);
    EXPECT_NEAR(data_in.closure[1].data.r, out_reflection.data.r, 1e-5f);
  }
#endif
}
