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

vec4 get_color(vec2 uv){
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

  if(flag_test(gp_interp_flat.mat_flag, GP_STROKE_ALIGNMENT)) // dot and squares
  {
    uv = uv*2.0 - 1.0;
    if(flag_test(gp_interp_flat.mat_flag, GP_STROKE_DOTS)){
      col *= gpencil_stroke_round_mask(length(uv), gp_interp_noperspective.hardness);
    }else{
      uv = abs(uv);
      col *= gpencil_stroke_round_mask(max(uv.x, uv.y), gp_interp_noperspective.hardness);
    }
  }

  return col;
}

vec4 alpha_over(vec4 base, vec4 over){
    return (1.0 - over.w) * base + over;
}

vec4 to_cam(vec4 a){
    return vec4(a.x/a.z, a.y/a.z, a.z, a.w/a.z);
}

vec4 from_cam(vec4 a){
    return vec4(a.x*a.z, a.y*a.z, a.z, a.w*a.z);
}

#define TYPE_DOT 0
#define TYPE_NUMBER 1
#define TYPE_LENGTH 2
#define TYPE_RADIUS 3

#define TYPE 2

#define point_density 10.0

float i_to_t(float i, vec4 p1, vec4 p2, float length_offset){
    vec4 P1 = from_cam(p1);
    vec4 P2 = from_cam(p2);
    float l = length(P1 - P2);

    if(TYPE == TYPE_RADIUS){
        if(p1.w == p2.w){
            return (i + mod(-length_offset*point_density,1.0) )*p1.w / (l * point_density);
        }
        
        float a = p1.w - p2.w;
        float E = -(a - l)/(a + l);
        
        float E_i = exp(((i + mod(-length_offset*point_density, 1.0))/(point_density*2)) * log(E));
        
        return (p1.w * (E_i - 1.0)) / (p2.w - p1.w);
    }else if(TYPE == TYPE_NUMBER){
        return i / point_density;
    }else if(TYPE == TYPE_LENGTH){
        return (i + mod(-length_offset*point_density,1.0) ) / (l * point_density);
    }else{// DOTs
        return 0.0;
    }
}

float t_to_i(float t, vec4 p1, vec4 p2, float length_offset){
    vec4 P1 = from_cam(p1);
    vec4 P2 = from_cam(p2);
    float l = length(P1 - P2);

    if(TYPE == TYPE_RADIUS){
        if(p1.w == p2.w){
            return t * (l * point_density)/p1.w - mod(-length_offset*point_density,1.0);
        }
 
        float a = p1.w - p2.w;
        float E = -(a - l)/(a + l);
        float E_i = t * (p2.w - p1.w) / p1.w + 1.0;
        
        return (log(E_i)/log(E) - mod(-length_offset*point_density, 1.0)/(point_density*2))*point_density*2.0;
    }else if(TYPE == TYPE_NUMBER){
        return t*point_density;
    }else if(TYPE == TYPE_LENGTH){
        return t * (l * point_density) - mod(-length_offset*point_density,1.0);
    }else{// DOTs
        return 0.0;
    }
}




int round_q(float fnum){
    return int(ceil(fnum));
    if(mod(fnum, 1.0) < 0.0001){
        return int(floor(fnum));
    }
}






vec2 circle_line_intersection(vec2 l1, vec2 l2, vec2 c, float r){
    float local_dis_sq = dot(l2-l1, l2-l1);
    float t = dot(c-l1, l2-l1)/local_dis_sq;
    float d = length(l1+(l2-l1)*t - c);
    float j = r*r - d*d;
    if(j < 0.0){
        return vec2(-1,-1);
    }
    
    float u = sqrt(j)/sqrt(local_dis_sq);
    
    return vec2(t-u, t+u);
}

float raytrace_plane(vec3 RD, vec3 RO, vec3 norm, vec3 p){
    return (dot(norm, p)-dot(RO, norm))/dot(RD, norm);
}

float screen_t_to_local_t(float screen_t, vec4 p1, vec4 p2){
    vec3 screen_point = normalize(vec3(p1.xy + (p2.xy-p1.xy) * saturate(screen_t), 1.0));

    vec3 P1 = from_cam(p1).xyz;
    vec3 P2 = from_cam(p2).xyz;
    
    vec3 norm = normalize(cross(P1-P2, cross(P1, vec3(0.0, 0.0, 1.0))));
    float ray_length = raytrace_plane(screen_point, vec3(0.0, 0.0, 0.0), norm, P1);
    vec3 local_point = screen_point*ray_length;
    
    
    float local_dis_sq = dot(P2-P1, P2-P1);
    float local_t = dot(local_point-P1, P2-P1)/local_dis_sq;

    return local_t;
}

int min_bound(vec4 p1, vec4 p2, float length_offset){
    return round_q(t_to_i(0.0, p1, p2, length_offset));
}

int max_bound(vec4 p1, vec4 p2, float length_offset){
    return round_q(t_to_i(1.0, p1, p2, length_offset));
}

int2 get_bounds(vec2 p0, vec4 p1, vec4 p2, float length_offset){
    if(TYPE == TYPE_DOT){
        return int2(0, 1);
    }
    
    int min_lower = 0;
    int max_upper = round_q(t_to_i(1.0, p1, p2, length_offset));

    return int2(min_lower, max_upper);

    int lower = 0;
    int upper = 1000000000;
    

    vec4 P1 = from_cam(p1);
    vec4 P2 = from_cam(p2);

    if(!(p1.z > 0 && p2.z > 0)) {
        return int2(min_lower, max_upper);
    } 
    
    float local_dis_sq = dot((p2-p1).xy, (p2-p1).xy);
    float t = saturate(dot(p0-p1.xy, (p2-p1).xy)/local_dis_sq);
    vec3 p_t = (p1 + (p2-p1)*t).xyz;
    
    float r = max(p1.w/p1.z, p2.w/p2.z);
    
//    r = r1/p1.z + (r2/p2.z - r1/p1.z) * t;
    
    if( length(p_t.xy - p0.xy) > r){
        return int2(0, 0);
    }

//        if(sdUnevenCapsule(p0*xy-p1*xy, r1/p1.z, r2/p2.z, distance(p1*xy,p2*xy))<0.0){
//            upper = 0;
//        }

    vec2 ts = circle_line_intersection(p1.xy, p2.xy, p0, r);
    
    if(ts.x != -1 && ts.y != -1){
        float t_min = screen_t_to_local_t(ts.x, p1, p2);
        float t_max = screen_t_to_local_t(ts.y, p1, p2);
        
        
        lower = int(floor(t_to_i(t_min, p1, p2, length_offset)));
        upper = int(ceil(t_to_i(t_max, p1, p2, length_offset))) + 1;
    }
    else{
        return int2(0, 0);
    }
    
    
    lower = max(min_lower, lower);
    upper = min(max_upper, upper);

    return int2(lower, upper);
}





























vec2 from_ss(vec2 a){
  return (a - viewportSize.xy / 2.0) / max(viewportSize.x, viewportSize.y);
}

vec4 from_ss(vec4 a){
  return vec4(from_ss(a.xy), a.z, a.w / max(viewportSize.x, viewportSize.y));
}

void main()
{
  bool is_multi_dot = true;

  if(flag_test(gp_interp_flat.mat_flag, GP_FILL)) // fill
  {
      fragColor = get_color(gp_interp.uv);
  }
  else {
    if(flag_test(gp_interp_flat.mat_flag, GP_STROKE_ALIGNMENT)) // dot and squares
    {
      if(is_multi_dot){
        vec2 coord = from_ss(gl_FragCoord.xy);

        float length_offset = 0.0;

        vec4 p1 = from_ss(gp_interp_flat.sspos1);
        vec4 p2 = from_ss(gp_interp_flat.sspos2);

        // p1 = gp_interp_flat.sspos1;
        // p2 = gp_interp_flat.sspos2;

        vec4 pos1 = from_cam(p1);
        vec4 pos2 = from_cam(p2);
        
        // vec4 pos1 = from_cam(gp_interp_flat.sspos1);
        // vec4 pos2 = from_cam(gp_interp_flat.sspos2);

        // float di = min(
        //   length(coord - gp_interp_flat.sspos1.xy) - gp_interp_flat.sspos1.w, 
        //   length(coord - gp_interp_flat.sspos2.xy) - gp_interp_flat.sspos2.w
        //   );

        // fragColor = vec4(vec3(di, cos(di*4.0), cos(di*20.0)), 1.0);

        // fragColor = vec4(vec3(gp_interp_flat.sspos1.z), 1.0);

        // fragColor = vec4(vec3(cos(length((pos1 - pos2).xyz)*40.0)), 1.0);

        vec2 uv = (coord - p1.xy) * p1.z;

        uv /= pos1.w;

        uv = uv*0.5 + 0.5;

        // fragColor = get_color(uv);

        int2 bounds = get_bounds(coord, to_cam(pos1), to_cam(pos2), length_offset);
        int lower = bounds.x;
        int upper = bounds.y;

        // for(int i = lower; i < upper; i++){
        for(int i=upper-1; i>=lower; i--){
          float t = i_to_t(i, to_cam(pos1), to_cam(pos2), length_offset);

          vec4 pos = to_cam(pos1 + (pos2 - pos1) * t);

          vec2 uv = coord - pos.xy;

          uv /= pos.w;

          uv = uv*0.5 + 0.5;

          // fragColor = alpha_over(fragColor, get_color(uv));
          fragColor = alpha_over(get_color(uv), fragColor);
        }
      } else {
        vec2 uv = gl_FragCoord.xy - gp_interp_flat.sspos1.xy;

        uv /= gp_interp_flat.sspos1.w / 2.0;

        uv = uv*0.5 + 0.5;

        fragColor = get_color(uv);
      }
    }
    else{ // line
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
