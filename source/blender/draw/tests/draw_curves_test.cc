/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "draw_manager.hh"
#include "draw_pass.hh"
#include "draw_testing.hh"

#include "draw_shader_shared.hh"

#include "GPU_batch.hh"
#include "GPU_shader.hh"

namespace blender::draw {

static void test_draw_curves_lib()
{
  Manager manager;

  GPUShader *sh = GPU_shader_create_from_info_name("draw_curves_test");

  struct Indirection {
    int index;
    GPU_VERTEX_FORMAT_FUNC(Indirection, index);
  };
  gpu::VertBuf *indirection_ribbon_buf = GPU_vertbuf_create_with_format_ex(
      Indirection::format(), GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);
  indirection_ribbon_buf->allocate(9);
  indirection_ribbon_buf->data<int>().copy_from({0, -1, -2, -3, -4, 0x7FFFFFFF, 1, -1, -2});
  gpu::Batch *batch_ribbon = GPU_batch_create_procedural(GPU_PRIM_TRI_STRIP, 2 * 9);

  gpu::VertBuf *indirection_cylinder_buf = GPU_vertbuf_create_with_format_ex(
      Indirection::format(), GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);
  indirection_cylinder_buf->allocate(6);
  indirection_cylinder_buf->data<int>().copy_from({0, -1, -2, -3, 1, -1});
  gpu::Batch *batch_cylinder = GPU_batch_create_procedural(GPU_PRIM_TRI_STRIP, (3 * 2 + 1) * 6);

  struct Position {
    float3 pos;
    GPU_VERTEX_FORMAT_FUNC(Position, pos);
  };
  gpu::VertBuf *pos_buf = GPU_vertbuf_create_with_format_ex(Position::format(),
                                                            GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);
  pos_buf->allocate(8);
  pos_buf->data<float3>().copy_from({float3{1.0f},
                                     float3{0.75f},
                                     float3{0.5f},
                                     float3{0.25f},
                                     float3{0.0f},
                                     float3{0.0f},
                                     float3{1.0f},
                                     float3{2.0f}});
  struct Radius {
    float rad;
    GPU_VERTEX_FORMAT_FUNC(Radius, rad);
  };
  gpu::VertBuf *rad_buf = GPU_vertbuf_create_with_format_ex(Radius::format(),
                                                            GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);
  rad_buf->allocate(8);
  rad_buf->data<float>().copy_from({1.0f, 0.75f, 0.5f, 0.25f, 0.0f, 0.0f, 1.0f, 2.0f});

  UniformBuffer<CurvesInfos> curves_info_buf;
  curves_info_buf.is_point_attribute[0].x = 0;
  curves_info_buf.is_point_attribute[1].x = 1;
  /* Ribbon. */
  curves_info_buf.vertex_per_segment = 2;
  curves_info_buf.half_cylinder_face_count = 1;
  curves_info_buf.push_update();

  Framebuffer fb;
  fb.ensure(int2(1, 1));

  {
    StorageArrayBuffer<float, 512> result_pos;
    StorageArrayBuffer<int4, 512> result_idx;
    result_pos.clear_to_zero();
    result_idx.clear_to_zero();

    PassSimple pass("Ribbon Curves");
    pass.framebuffer_set(&fb);
    pass.shader_set(sh);
    pass.bind_ubo("drw_curves", curves_info_buf);
    pass.bind_texture("curves_pos_buf", pos_buf);
    pass.bind_texture("curves_rad_buf", rad_buf);
    pass.bind_texture("curves_indirection_buf", indirection_ribbon_buf);
    pass.bind_ssbo("result_pos_buf", result_pos);
    pass.bind_ssbo("result_indices_buf", result_idx);
    pass.draw(batch_ribbon);
    pass.barrier(GPU_BARRIER_BUFFER_UPDATE);

    manager.submit(pass);

    /* Note: Expected values follows diagram shown in #142969. */

    result_pos.read();
    EXPECT_EQ(result_pos[0], 1.0f);
    EXPECT_EQ(result_pos[1], 1.0f);
    EXPECT_EQ(result_pos[2], 0.75f);
    EXPECT_EQ(result_pos[3], 0.75f);
    EXPECT_EQ(result_pos[4], 0.5f);
    EXPECT_EQ(result_pos[5], 0.5f);
    EXPECT_EQ(result_pos[6], 0.25f);
    EXPECT_EQ(result_pos[7], 0.25f);
    EXPECT_EQ(result_pos[8], 0.0f);
    EXPECT_EQ(result_pos[9], 0.0f);
    EXPECT_TRUE(isnan(result_pos[10]));
    EXPECT_TRUE(isnan(result_pos[11]));
    EXPECT_EQ(result_pos[12], 0.0f);
    EXPECT_EQ(result_pos[13], 0.0f);
    EXPECT_EQ(result_pos[14], 1.0f);
    EXPECT_EQ(result_pos[15], 1.0f);
    EXPECT_EQ(result_pos[16], 2.0f);
    EXPECT_EQ(result_pos[17], 2.0f);

    result_idx.read();
    /* x: point_id, y: curve_id, z: curve_segment, w: unused */
    EXPECT_EQ(result_idx[0], int4(0, 0, 0, -1));
    EXPECT_EQ(result_idx[1], int4(0, 0, 0, 1));
    EXPECT_EQ(result_idx[2], int4(1, 0, 1, -1));
    EXPECT_EQ(result_idx[3], int4(1, 0, 1, 1));
    EXPECT_EQ(result_idx[4], int4(2, 0, 2, -1));
    EXPECT_EQ(result_idx[5], int4(2, 0, 2, 1));
    EXPECT_EQ(result_idx[6], int4(3, 0, 3, -1));
    EXPECT_EQ(result_idx[7], int4(3, 0, 3, 1));
    EXPECT_EQ(result_idx[8], int4(4, 0, 4, -1));
    EXPECT_EQ(result_idx[9], int4(4, 0, 4, 1));
    EXPECT_EQ(result_idx[10], int4(5 - 0x7FFFFFFF, 0x7FFFFFFF, 0, -1));
    EXPECT_EQ(result_idx[11], int4(5 - 0x7FFFFFFF, 0x7FFFFFFF, 0, 1));
    EXPECT_EQ(result_idx[12], int4(5, 1, 0, -1));
    EXPECT_EQ(result_idx[13], int4(5, 1, 0, 1));
    EXPECT_EQ(result_idx[14], int4(6, 1, 1, -1));
    EXPECT_EQ(result_idx[15], int4(6, 1, 1, 1));
    EXPECT_EQ(result_idx[16], int4(7, 1, 2, -1));
    EXPECT_EQ(result_idx[17], int4(7, 1, 2, 1));
  }

  GPU_shader_unbind();

  GPU_SHADER_FREE_SAFE(sh);
  GPU_BATCH_DISCARD_SAFE(batch_ribbon);
  GPU_BATCH_DISCARD_SAFE(batch_cylinder);
  GPU_VERTBUF_DISCARD_SAFE(indirection_ribbon_buf);
  GPU_VERTBUF_DISCARD_SAFE(indirection_cylinder_buf);
  GPU_VERTBUF_DISCARD_SAFE(pos_buf);
  GPU_VERTBUF_DISCARD_SAFE(rad_buf);
}
DRAW_TEST(draw_curves_lib)

}  // namespace blender::draw
