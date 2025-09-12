#include "draw_view_lib.glsl"

void main()
{
  float depth = texture(depth_tx, screen_uv).r;
  if (depth == 1.0f) {
    gpu_discard_fragment();
    return;
  }

  const float3 P = drw_point_screen_to_world(float3(screen_uv, depth));
  rayQueryEXT query;
  rayQueryInitializeEXT(query,
                        shadow_as,
                        gl_RayFlagsTerminateOnFirstHitEXT,
                        0xFF,
                        P,
                        0.01f,
                        -pass_data.light_direction_ws,
                        1000.0f);
  rayQueryProceedEXT(query);

  bool is_light_occluded = rayQueryGetIntersectionTypeEXT(query, true) !=
                           gl_RayQueryCommittedIntersectionNoneEXT;

  gl_FragStencilRefARB = is_light_occluded ? 0x01 : 0x00;
}
