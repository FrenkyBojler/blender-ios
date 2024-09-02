/* SPDX-FileCopyrightText: 2020-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma BLENDER_REQUIRE(common_gpencil_lib.glsl)
#pragma BLENDER_REQUIRE(common_colormanagement_lib.glsl)

float length_squared(vec2 v)
{
  return dot(v, v);
}
float length_squared(vec3 v)
{
  return dot(v, v);
}

vec3 gpencil_lighting(void)
{
  vec3 light_accum = vec3(0.0);
  for (int i = 0; i < GPENCIL_LIGHT_BUFFER_LEN; i++) {
    if (gp_lights[i]._color.x == -1.0) {
      break;
    }
    vec3 L = gp_lights[i]._position - gp_interp.pos;
    float vis = 1.0;
    gpLightType type = floatBitsToUint(gp_lights[i]._type);
    /* Spot Attenuation. */
    if (type == GP_LIGHT_TYPE_SPOT) {
      mat3 rot_scale = mat3(gp_lights[i]._right, gp_lights[i]._up, gp_lights[i]._forward);
      vec3 local_L = rot_scale * L;
      local_L /= abs(local_L.z);
      float ellipse = inversesqrt(length_squared(local_L));
      vis *= smoothstep(0.0, 1.0, (ellipse - gp_lights[i]._spot_size) / gp_lights[i]._spot_blend);
      /* Also mask +Z cone. */
      vis *= step(0.0, local_L.z);
    }
    /* Inverse square decay. Skip for suns. */
    float L_len_sqr = length_squared(L);
    if (type < GP_LIGHT_TYPE_SUN) {
      vis /= L_len_sqr;
    }
    else {
      L = gp_lights[i]._forward;
      L_len_sqr = 1.0;
    }
    /* Lambertian falloff */
    if (type != GP_LIGHT_TYPE_AMBIENT) {
      L /= sqrt(L_len_sqr);
      vis *= clamp(dot(gpNormal, L), 0.0, 1.0);
    }
    light_accum += vis * gp_lights[i]._color;
  }
  /* Clamp to avoid NaNs. */
  return clamp(light_accum, 0.0, 1e10);
}

vec4 get_color(vec2 uv)
{
  vec4 col;
  if (flag_test(gp_interp_flat.mat_flag, GP_STROKE_TEXTURE_USE)) {
    bool premul = flag_test(gp_interp_flat.mat_flag, GP_STROKE_TEXTURE_PREMUL);
    col = texture_read_as_linearrgb(gpStrokeTexture, premul, uv);
  }
  else if (flag_test(gp_interp_flat.mat_flag, GP_FILL_TEXTURE_USE)) {
    bool use_clip = flag_test(gp_interp_flat.mat_flag, GP_FILL_TEXTURE_CLIP);
    vec2 uvs = (use_clip) ? clamp(uv, 0.0, 1.0) : uv;
    bool premul = flag_test(gp_interp_flat.mat_flag, GP_FILL_TEXTURE_PREMUL);
    col = texture_read_as_linearrgb(gpFillTexture, premul, uvs);
  }
  else if (flag_test(gp_interp_flat.mat_flag, GP_FILL_GRADIENT_USE)) {
    bool radial = flag_test(gp_interp_flat.mat_flag, GP_FILL_GRADIENT_RADIAL);
    float fac = clamp(radial ? length(uv * 2.0 - 1.0) : uv.x, 0.0, 1.0);
    uint matid = gp_interp_flat.mat_flag >> GPENCIl_MATID_SHIFT;
    col = mix(gp_materials[matid].fill_color, gp_materials[matid].fill_mix_color, fac);
  }
  else /* SOLID */ {
    col = vec4(1.0);
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
      col *= gpencil_stroke_round_mask(length(uv), gp_interp_noperspective.hardness);
    }
    else {
      uv = abs(uv);
      col *= gpencil_stroke_round_mask(max(uv.x, uv.y), gp_interp_noperspective.hardness);
    }
  }

  return col;
}

vec4 get_dot_color(vec2 uv, int i)
{
  vec4 col = get_color(uv);
  float rand = mod(sin(mod(i * 437.532124, 1.0) * 75.4368634), 1.0);
  col.rgb *= rand * 0.8 + 0.2;

  return col;
}

vec4 alpha_over(vec4 base, vec4 over)
{
  return (1.0 - over.w) * base + over;
}

vec4 to_cam(vec4 a)
{
  if (ProjectionMatrix[3][3] == 0.0) {
    return vec4(a.x / a.z, a.y / a.z, a.z, a.w / a.z);
  }
  return a;
}

vec4 from_cam(vec4 a)
{
  if (ProjectionMatrix[3][3] == 0.0) {
    return vec4(a.x * a.z, a.y * a.z, a.z, a.w * a.z);
  }
  return a;
}

float i_to_t(float i, vec4 p1, vec4 p2)
{
  float i_start = gp_interp_flat.point_length.x;
  float i_end = gp_interp_flat.point_length.y;
  float point_density = gp_interp_flat.point_length.z;
  float i_delta = i_end - i_start;

  uint placement_mode = gp_interp_flat.mat_flag & GP_DOTS_PLACEMENT_MODE;

  if (placement_mode == GP_DOTS_PLACEMENT_MODE_RADIUS) {
    vec4 P1 = from_cam(p1);
    vec4 P2 = from_cam(p2);
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

float t_to_i(float t, vec4 p1, vec4 p2)
{
  float i_start = gp_interp_flat.point_length.x;
  float i_end = gp_interp_flat.point_length.y;
  float point_density = gp_interp_flat.point_length.z;
  float i_delta = i_end - i_start;

  uint placement_mode = gp_interp_flat.mat_flag & GP_DOTS_PLACEMENT_MODE;

  if (placement_mode == GP_DOTS_PLACEMENT_MODE_RADIUS) {
    vec4 P1 = from_cam(p1);
    vec4 P2 = from_cam(p2);
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

vec2 uneven_capsule_intersection(vec2 p0, vec2 p1, vec2 p2, float r1, float r2)
{
  float l = distance(p1, p2);

  float local_dis_sq = dot(p2 - p1, p2 - p1);
  float X = (dot(p0 - p1, p2 - p1) / local_dis_sq) * l;
  vec2 p_t = p1 + (p2 - p1) * (X / l);
  float Y = distance(p_t, p0);

  float a = l * l - (r2 - r1) * (r2 - r1);
  float b = -2.0 * (r1 * (r2 - r1) + l * X);
  float c = Y * Y + X * X - r1 * r1;

  float discriminant = b * b - 4.0 * a * c;
  if (discriminant < 0.0) {
    return vec2(-1.0, -1.0);
  }

  /* The quadratic equation. */
  vec2 t = (vec2(-1.0, 1.0) * sqrt(discriminant) - b) / (2.0 * a);

  if (r1 < r2) {
    if (l - r2 < -r1) {
      return vec2(t.x, 1.0);
    }
  }
  else {
    if (l + r2 < r1) {
      return vec2(0.0, t.y);
    }
  }

  return t;
}

int min_bound(vec4 p1, vec4 p2)
{
  return round_q(t_to_i(0.0, p1, p2));
}

int max_bound(vec4 p1, vec4 p2)
{
  return round_q(t_to_i(1.0, p1, p2));
}

int2 get_bounds(vec2 p0, vec4 p1, vec4 p2)
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

  vec2 ts = uneven_capsule_intersection(p0, p1.xy, p2.xy, p1.w, p2.w);

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

vec3 ndc_to_view(vec4 ndc)
{
  if (ProjectionMatrix[3][3] == 0.0) {
    vec3 view = point_ndc_to_view(ndc);
    view.z *= -1.0;
    return view;
  }
  float aspect = viewportSize.x / viewportSize.y;
  return vec3(ndc.xy / vec2(1.0, aspect), 1.0);
}

void main()
{
  uint placement_mode = gp_interp_flat.mat_flag & GP_DOTS_PLACEMENT_MODE;
  bool is_multi_dot = placement_mode != GP_DOTS_PLACEMENT_MODE_SINGLE;

  if (flag_test(gp_interp_flat.mat_flag, GP_FILL))  // fill
  {
    fragColor = get_color(gp_interp.uv);
  }
  else {
    if (flag_test(gp_interp_flat.mat_flag, GP_STROKE_ALIGNMENT))  // dot and squares
    {
      if (is_multi_dot) {
        float radius1;
        float radius2;

        vec4 ndc1 = screen_space_to_ndc_and_radius(gp_interp_flat.sspos1, radius1, viewportSize);
        vec4 ndc2 = screen_space_to_ndc_and_radius(gp_interp_flat.sspos2, radius2, viewportSize);

        vec3 v1 = ndc_to_view(ndc1);
        vec3 v2 = ndc_to_view(ndc2);

        vec3 view_dir = ndc_to_view(vec4(gl_FragCoord.xy / viewportSize.xy, 0.0, 1.0) * 2.0 - 1.0);
        vec2 view_coord = view_dir.xy / view_dir.z;

        float dx = 15.0;

        vec3 dview_dir = ndc_to_view(
            vec4((gl_FragCoord.xy + vec2(dx, 0.0)) / viewportSize.xy, 0.0, 1.0) * 2.0 - 1.0);
        vec2 dview_coord = dview_dir.xy / dview_dir.z;

        vec2 dv_dx = (dview_coord - view_coord) / dx;
        float scale_fac = length(dv_dx);

        vec4 P1 = vec4(v1, radius1 * scale_fac);
        vec4 P2 = vec4(v2, radius2 * scale_fac);

        vec4 p1 = to_cam(P1);
        vec4 p2 = to_cam(P2);

        int2 bounds = get_bounds(view_coord, p1, p2);
        int lower = bounds.x;
        int upper = bounds.y;

        /* Loop thought backwards so we can break early. */
        for (int i = upper - 1; i >= lower; i--) {
          float t = i_to_t(i, p1, p2);

          vec4 pos = to_cam(P1 + (P2 - P1) * t);

          vec2 uv = (view_coord - pos.xy) / pos.w;

          fragColor = alpha_over(get_dot_color(uv * 0.5 + 0.5, i), fragColor);

          /* Break early if full opacity. */
          if (fragColor.w > 0.999) {
            break;
          }
        }
      }
      else {
        vec2 uv = (gl_FragCoord.xy - gp_interp_flat.sspos1.xy) / gp_interp_flat.sspos1.w;

        int i = int(gp_interp_flat.point_length.x);
        fragColor = get_dot_color(uv * 0.5 + 0.5, i);
      }
    }
    else {  // line
      fragColor = get_color(gp_interp.uv);
      fragColor *= gpencil_stroke_round_cap_mask(gp_interp_flat.sspos1.xy,
                                                 gp_interp_flat.sspos2.xy,
                                                 gp_interp_flat.aspect,
                                                 gp_interp_noperspective.thickness.x,
                                                 gp_interp_noperspective.hardness);
    }
  }

  /* To avoid aliasing artifacts, we reduce the opacity of small strokes. */
  fragColor *= smoothstep(0.0, 1.0, gp_interp_noperspective.thickness.y);

  /* Holdout materials. */
  if (flag_test(gp_interp_flat.mat_flag, GP_STROKE_HOLDOUT | GP_FILL_HOLDOUT)) {
    revealColor = fragColor.aaaa;
  }
  else {
    /* NOT holdout materials.
     * For compatibility with colored alpha buffer.
     * Note that we are limited to mono-chromatic alpha blending here
     * because of the blend equation and the limit of 1 color target
     * when using custom color blending. */
    revealColor = vec4(0.0, 0.0, 0.0, fragColor.a);

    if (fragColor.a < 0.001) {
      discard;
      return;
    }
  }

  vec2 fb_size = max(vec2(textureSize(gpSceneDepthTexture, 0).xy),
                     vec2(textureSize(gpMaskTexture, 0).xy));
  vec2 uvs = gl_FragCoord.xy / fb_size;
  /* Manual depth test */
  float scene_depth = texture(gpSceneDepthTexture, uvs).r;
  if (gl_FragCoord.z > scene_depth) {
    discard;
    return;
  }

  /* FIXME(fclem): Grrr. This is bad for performance but it's the easiest way to not get
   * depth written where the mask obliterate the layer. */
  float mask = texture(gpMaskTexture, uvs).r;
  if (mask < 0.001) {
    discard;
    return;
  }

  /* We override the fragment depth using the fragment shader to ensure a constant value.
   * This has a cost as the depth test cannot happen early.
   * We could do this in the vertex shader but then perspective interpolation of uvs and
   * fragment clipping gets really complicated. */
  if (gp_interp_flat.depth >= 0.0) {
    gl_FragDepth = gp_interp_flat.depth;
  }
  else {
    gl_FragDepth = gl_FragCoord.z;
  }
}
