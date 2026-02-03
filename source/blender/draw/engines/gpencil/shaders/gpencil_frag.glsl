/* SPDX-FileCopyrightText: 2020-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpencil_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(gpencil_geometry)

#include "draw_colormanagement_lib.glsl"
#include "draw_grease_pencil_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"

float3 gpencil_lighting()
{
  float3 light_accum = float3(0.0f);
  for (int i = 0; i < GPENCIL_LIGHT_BUFFER_LEN; i++) {
    if (float3(gp_lights[i]._color).x == -1.0f) {
      break;
    }
    float3 L = gp_lights[i]._position - gp_interp.pos;
    float vis = 1.0f;
    gpLightType type = gpLightType(floatBitsToUint(gp_lights[i]._type));
    /* Spot Attenuation. */
    if (type == GP_LIGHT_TYPE_SPOT) {
      float3x3 rot_scale = float3x3(gp_lights[i]._right, gp_lights[i]._up, gp_lights[i]._forward);
      float3 local_L = rot_scale * L;
      local_L /= abs(local_L.z);
      float ellipse = inversesqrt(length_squared(local_L));
      vis *= smoothstep(
          0.0f, 1.0f, (ellipse - gp_lights[i]._spot_size) / gp_lights[i]._spot_blend);
      /* Also mask +Z cone. */
      vis *= step(0.0f, local_L.z);
    }
    /* Inverse square decay. Skip for suns. */
    float L_len_sqr = length_squared(L);
    if (type < GP_LIGHT_TYPE_SUN) {
      vis /= L_len_sqr;
    }
    else {
      L = gp_lights[i]._forward;
      L_len_sqr = 1.0f;
    }
    /* Lambertian falloff */
    if (type != GP_LIGHT_TYPE_AMBIENT) {
      L /= sqrt(L_len_sqr);
      vis *= clamp(dot(gp_normal, L), 0.0f, 1.0f);
    }
    light_accum += vis * gp_lights[i]._color;
  }
  /* Clamp to avoid NaNs. */
  return clamp(light_accum, 0.0f, 1e10f);
}

float4 get_color(float2 uv)
{
  float4 col;
  if (flag_test(gp_interp_flat.mat_flag, GP_STROKE_TEXTURE_USE)) {
    bool premul = flag_test(gp_interp_flat.mat_flag, GP_STROKE_TEXTURE_PREMUL);
    col = texture_read_as_linearrgb(gp_stroke_tx, premul, uv);
  }
  else if (flag_test(gp_interp_flat.mat_flag, GP_FILL_TEXTURE_USE)) {
    bool use_clip = flag_test(gp_interp_flat.mat_flag, GP_FILL_TEXTURE_CLIP);
    float2 uvs = (use_clip) ? clamp(uv, 0.0, 1.0) : uv;
    bool premul = flag_test(gp_interp_flat.mat_flag, GP_FILL_TEXTURE_PREMUL);
    col = texture_read_as_linearrgb(gp_fill_tx, premul, uvs);
  }
  else if (flag_test(gp_interp_flat.mat_flag, GP_FILL_GRADIENT_USE)) {
    bool radial = flag_test(gp_interp_flat.mat_flag, GP_FILL_GRADIENT_RADIAL);
    float fac = clamp(radial ? length(uv * 2.0 - 1.0) : uv.x, 0.0, 1.0);
    uint matid = gp_interp_flat.mat_flag >> GPENCIl_MATID_SHIFT;
    col = mix(gp_materials[matid].fill_color, gp_materials[matid].fill_mix_color, fac);
  }
  else /* SOLID */ {
    col = float4(1.0f);
  }
  col.rgb *= col.a;

  /* Composite all other colors on top of texture color.
   * Everything is pre-multiply by `col.a` to have the stencil effect. */
  col = col * gp_interp.color_mul + col.a * gp_interp.color_add;

  col.rgb *= gpencil_lighting();

  if (flag_test(gp_interp_flat.mat_flag, GP_STROKE_ALIGNMENT))  // dot and squares
  {
    uv = uv * 2.0 - 1.0;
    if (flag_test(gp_interp_flat.mat_flag, GP_STROKE_DOTS)) {
      col *= gpencil_stroke_hardess_mask(length(uv), gp_interp_noperspective.hardness);
    }
    else {
      uv = abs(uv);
      col *= gpencil_stroke_hardess_mask(max(uv.x, uv.y), gp_interp_noperspective.hardness);
    }
  }

  return col;
}

float4 get_dot_color(float2 uv, int i)
{
  float4 col = get_color(uv);
  float rand = mod(sin(mod(i * 437.532124, 1.0) * 75.4368634), 1.0);
  col.rgb *= rand * 0.8 + 0.2;

  return col;
}

float2 rot_uv(float2 uv, float2 x_axis)
{
  /* Rotate 90 degrees counter-clockwise. */
  float2 y_axis = -float2(-x_axis.y, x_axis.x);
  uv = float2x2(x_axis, y_axis) * uv;
  uv.y *= -1.0f;

  return uv;
}

float4 alpha_over(float4 base, float4 over)
{
  return (1.0 - over.w) * base + over;
}

float4 to_cam(float4 a)
{
  if (drw_view_is_perspective()) {
    return float4(a.x / a.z, a.y / a.z, a.z, a.w / a.z);
  }
  return a;
}

float4 from_cam(float4 a)
{
  if (drw_view_is_perspective()) {
    return float4(a.x * a.z, a.y * a.z, a.z, a.w * a.z);
  }
  return a;
}

float i_to_t(float i, float4 p1, float4 p2)
{
  float i_start = gp_interp_flat.point_length.x;
  float i_end = gp_interp_flat.point_length.y;
  float point_density = gp_interp_flat.point_length.z;
  float i_delta = i_end - i_start;

  uint placement_mode = gp_interp_flat.mat_flag & GP_DOTS_PLACEMENT_MODE;

  if (placement_mode == GP_DOTS_PLACEMENT_MODE_RADIUS) {
    float4 P1 = from_cam(p1);
    float4 P2 = from_cam(p2);
    float l = length(P1.xyz - P2.xyz);

    float r1 = P1.w;
    float r2 = P2.w;
    float a = r2 - r1;
    if (abs(a) < 0.001) {
      return (i / point_density - i_start) * r1 / l;
    }

    float E = (l + a) / (l - a);
    float E_i = pow(E, (i / point_density - i_start) / 2.0);

    return r1 * (E_i - 1.0) / a;
  }
  else if (placement_mode == GP_DOTS_PLACEMENT_MODE_LENGTH ||
           placement_mode == GP_DOTS_PLACEMENT_MODE_NUMBER)
  {
    return (i / point_density - i_start) / i_delta;
  }
  else { /* GP_DOTS_PLACEMENT_MODE_SINGLE */
    return 0.0;
  }
}

float t_to_i(float t, float4 p1, float4 p2)
{
  float i_start = gp_interp_flat.point_length.x;
  float i_end = gp_interp_flat.point_length.y;
  float point_density = gp_interp_flat.point_length.z;
  float i_delta = i_end - i_start;

  uint placement_mode = gp_interp_flat.mat_flag & GP_DOTS_PLACEMENT_MODE;

  if (placement_mode == GP_DOTS_PLACEMENT_MODE_RADIUS) {
    float4 P1 = from_cam(p1);
    float4 P2 = from_cam(p2);
    float l = length(P1.xyz - P2.xyz);

    float r1 = P1.w;
    float r2 = P2.w;
    float a = r2 - r1;
    if (abs(a) < 0.001) {
      return (t * l / P1.w + i_start) * point_density;
    }

    float E = (l + a) / (l - a);
    float E_i = t * a / r1 + 1.0;

    return (2.0 * log(E_i) / log(E) + i_start) * point_density;
  }
  else if (placement_mode == GP_DOTS_PLACEMENT_MODE_LENGTH ||
           placement_mode == GP_DOTS_PLACEMENT_MODE_NUMBER)
  {
    return (t * i_delta + i_start) * point_density;
  }
  else { /* GP_DOTS_PLACEMENT_MODE_SINGLE */
    return 0.0;
  }
}

int round_q(float fnum)
{
  if (mod(fnum, 1.0) < 0.0001) {
    return int(floor(fnum));
  }
  return int(ceil(fnum));
}

float screen_t_to_local_t(float screen_t, float z1, float z2)
{
  float f = (1.0 - screen_t);

  float k = z2 / z1 - 1.0;
  float local_t = screen_t / (k * f + 1.0);

  return local_t;
}

float2 uneven_capsule_intersection(float2 p0, float2 p1, float2 p2, float r1, float r2)
{
  float l = distance(p1, p2);

  float local_dis_sq = dot(p2 - p1, p2 - p1);
  float X = (dot(p0 - p1, p2 - p1) / local_dis_sq) * l;
  float2 p_t = p1 + (p2 - p1) * (X / l);
  float Y = distance(p_t, p0);

  float a = l * l - (r2 - r1) * (r2 - r1);
  float b = -2.0 * (r1 * (r2 - r1) + l * X);
  float c = Y * Y + X * X - r1 * r1;

  float discriminant = b * b - 4.0 * a * c;
  if (discriminant < 0.0) {
    return float2(-1.0, -1.0);
  }

  /* The quadratic equation. */
  float2 t = (float2(-1.0, 1.0) * sqrt(discriminant) - b) / (2.0 * a);

  if (r1 < r2) {
    if (l - r2 < -r1) {
      return float2(t.x, 1.0);
    }
  }
  else {
    if (l + r2 < r1) {
      return float2(0.0, t.y);
    }
  }

  return t;
}

int min_bound(float4 p1, float4 p2)
{
  return round_q(t_to_i(0.0, p1, p2));
}

int max_bound(float4 p1, float4 p2)
{
  return round_q(t_to_i(1.0, p1, p2));
}

int2 get_bounds(float2 p0, float4 p1, float4 p2)
{
  uint placement_mode = gp_interp_flat.mat_flag & GP_DOTS_PLACEMENT_MODE;
  if (placement_mode == GP_DOTS_PLACEMENT_MODE_SINGLE) {
    return int2(0, 1);
  }

  int min_lower = min_bound(p1, p2);
  int max_upper = max_bound(p1, p2);

  int lower = 0;
  int upper = 1000000000;

  if (!(p1.z > 0 && p2.z > 0)) {
    return int2(min_lower, max_upper);
  }

  float r1 = p1.w;
  float r2 = p2.w;

  bool is_squares = !flag_test(gp_interp_flat.mat_flag, GP_STROKE_DOTS);

  if (is_squares) {
    r1 *= M_SQRT2;
    r2 *= M_SQRT2;
  }

  float2 ts = uneven_capsule_intersection(p0, p1.xy, p2.xy, r1, r2);

  if (ts.x == -1 && ts.y == -1) {
    return int2(0, 0);
  }

  if (ts.y < 0.0 || ts.x > 1.0) {
    return int2(0, 0);
  }

  float t_min = screen_t_to_local_t(saturate(ts.x), p1.z, p2.z);
  float t_max = screen_t_to_local_t(saturate(ts.y), p1.z, p2.z);

  lower = int(floor(t_to_i(t_min, p1, p2)));
  upper = int(ceil(t_to_i(t_max, p1, p2))) + 1;

  lower = max(min_lower, lower);
  upper = min(max_upper, upper);

  return int2(lower, upper);
}

float3 ndc_to_view(float4 ndc)
{
  if (drw_view_is_perspective()) {
    float3 view = point_ndc_to_view(ndc);
    view.z *= -1.0;
    return view;
  }
  float aspect = viewport_size.x / viewport_size.y;
  return float3(ndc.xy / float2(1.0, aspect), 1.0);
}

void main()
{
  uint placement_mode = gp_interp_flat.mat_flag & GP_DOTS_PLACEMENT_MODE;
  bool is_multi_dot = placement_mode != GP_DOTS_PLACEMENT_MODE_SINGLE;

  if (flag_test(gp_interp_flat.mat_flag, GP_FILL))  // fill
  {
    frag_color = get_color(gp_interp.uv);
  }
  else {
    if (flag_test(gp_interp_flat.mat_flag, GP_STROKE_ALIGNMENT))  // dot and squares
    {
      if (is_multi_dot) {
        float radius1 = screen_space_to_radius(gp_interp_flat.sspos_1);
        float radius2 = screen_space_to_radius(gp_interp_flat.sspos_2);

        float4 ndc1 = screen_space_to_ndc(gp_interp_flat.sspos_1, viewport_size);
        float4 ndc2 = screen_space_to_ndc(gp_interp_flat.sspos_2, viewport_size);

        float3 v1 = ndc_to_view(ndc1);
        float3 v2 = ndc_to_view(ndc2);

        float3 view_dir = ndc_to_view(float4(gl_FragCoord.xy / viewport_size.xy, 0.0, 1.0) * 2.0 -
                                      1.0);
        float2 view_coord = view_dir.xy / view_dir.z;

        /* TODO. Calculate without finite deference. */
        float dx = 15.0;
        float3 dview_dir = ndc_to_view(
            float4((gl_FragCoord.xy + float2(dx, 0.0)) / viewport_size.xy, 0.0, 1.0) * 2.0 - 1.0);
        float2 dview_coord = dview_dir.xy / dview_dir.z;
        float2 dv_dx = (dview_coord - view_coord) / dx;
        float scale_fac = length(dv_dx);

        float4 P1 = float4(v1, radius1 * scale_fac);
        float4 P2 = float4(v2, radius2 * scale_fac);

        float4 p1 = to_cam(P1);
        float4 p2 = to_cam(P2);

        int2 bounds = get_bounds(view_coord, p1, p2);
        int lower = bounds.x;
        int upper = bounds.y;

        frag_color = float4(0.0f);
        /* Loop through backwards so we can break early. */
        for (int i = upper - 1; i >= lower; i--) {
          float t = i_to_t(i, p1, p2);

          float4 pos = to_cam(P1 + (P2 - P1) * t);

          float2 uv = (view_coord - pos.xy) / pos.w;
          uv = rot_uv(uv, gp_interp_flat.aspect.zw);

          frag_color = alpha_over(get_dot_color(uv * 0.5 + 0.5, i), frag_color);

          /* Break early if full opacity. */
          if (frag_color.w > 0.999) {
            break;
          }
        }
      }
      else {
        float2 uv = (gl_FragCoord.xy - gp_interp_flat.sspos_1.xy) / gp_interp_flat.sspos_1.w;

        int i = int(gp_interp_flat.point_length.x);

        /* TEMP CODE */
        if (gl_FragCoord.x / viewport_size.x < 0.5) {
          uv = rot_uv(uv, gp_interp_flat.aspect.zw);

          uv = uv * 0.5 + 0.5;
        }
        else {
          uv = gp_interp.uv;
        }

        frag_color = get_dot_color(uv, i);
      }
    }
    else {  // line
      frag_color = get_color(gp_interp.uv);
      frag_color *= gpencil_stroke_mask(gp_interp_flat.sspos_1.xy,
                                        gp_interp_flat.sspos_2.xy,
                                        gp_interp_flat.sspos_0,
                                        gp_interp_flat.sspos_3,
                                        gp_interp.uv,
                                        gp_interp_flat.mat_flag,
                                        gp_interp_noperspective.thickness.x,
                                        gp_interp_noperspective.hardness,
                                        gp_interp_noperspective.thickness.zw);
    }
  }

  /* To avoid aliasing artifacts, we reduce the opacity of small strokes. */
  frag_color *= smoothstep(0.0f, 1.0f, gp_interp_noperspective.thickness.y);

  /* Holdout materials. */
  if (flag_test(gp_interp_flat.mat_flag, GP_STROKE_HOLDOUT | GP_FILL_HOLDOUT)) {
    revealColor = frag_color.aaaa;
  }
  else {
    /* NOT holdout materials.
     * For compatibility with colored alpha buffer.
     * Note that we are limited to mono-chromatic alpha blending here
     * because of the blend equation and the limit of 1 color target
     * when using custom color blending. */
    revealColor = float4(0.0f, 0.0f, 0.0f, frag_color.a);

    if (frag_color.a < 0.001f) {
      gpu_discard_fragment();
      return;
    }
  }

  float2 fb_size = max(float2(textureSize(gp_scene_depth_tx, 0).xy),
                       float2(textureSize(gp_mask_tx, 0).xy));
  float2 uvs = gl_FragCoord.xy / fb_size;
  /* Manual depth test */
  float scene_depth = texture(gp_scene_depth_tx, uvs).r;
  if (gl_FragCoord.z > scene_depth) {
    gpu_discard_fragment();
    return;
  }

  /* FIXME(fclem): Grrr. This is bad for performance but it's the easiest way to not get
   * depth written where the mask obliterate the layer. */
  float mask = texture(gp_mask_tx, uvs).r;
  if (mask < 0.001f) {
    gpu_discard_fragment();
    return;
  }

  /* We override the fragment depth using the fragment shader to ensure a constant value.
   * This has a cost as the depth test cannot happen early.
   * We could do this in the vertex shader but then perspective interpolation of uvs and
   * fragment clipping gets really complicated. */
  if (gp_interp_flat.depth >= 0.0f) {
    gl_FragDepth = gp_interp_flat.depth;
  }
  else {
    gl_FragDepth = gl_FragCoord.z;
  }
}
