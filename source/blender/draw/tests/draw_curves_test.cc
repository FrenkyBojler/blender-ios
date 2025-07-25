/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "DNA_curves_types.h"

#include "BKE_curves.hh"

#include "GPU_batch.hh"
#include "GPU_shader.hh"

#include "draw_manager.hh"
#include "draw_pass.hh"
#include "draw_testing.hh"

#include "draw_shader_shared.hh"

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
    /* x: point_id, y: curve_id, z: curve_segment, w: azimuthal_offset */
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

  /* Cylinder. */
  curves_info_buf.vertex_per_segment = 7;
  curves_info_buf.half_cylinder_face_count = 2;
  curves_info_buf.push_update();

  {
    StorageArrayBuffer<float, 512> result_pos;
    StorageArrayBuffer<int4, 512> result_idx;
    result_pos.clear_to_zero();
    result_idx.clear_to_zero();

    PassSimple pass("Cylinder Curves");
    pass.framebuffer_set(&fb);
    pass.shader_set(sh);
    pass.bind_ubo("drw_curves", curves_info_buf);
    pass.bind_texture("curves_pos_buf", pos_buf);
    pass.bind_texture("curves_rad_buf", rad_buf);
    pass.bind_texture("curves_indirection_buf", indirection_cylinder_buf);
    pass.bind_ssbo("result_pos_buf", result_pos);
    pass.bind_ssbo("result_indices_buf", result_idx);
    pass.draw(batch_cylinder);
    pass.barrier(GPU_BARRIER_BUFFER_UPDATE);

    manager.submit(pass);

    /* Note: Expected values follows diagram shown in #142969. */

    result_pos.read();
    EXPECT_EQ(result_pos[0], 1.0f);
    EXPECT_EQ(result_pos[1], 0.75f);
    EXPECT_EQ(result_pos[2], 1.0f);
    EXPECT_EQ(result_pos[3], 0.75f);
    EXPECT_EQ(result_pos[4], 1.0f);
    EXPECT_EQ(result_pos[5], 0.75f);
    EXPECT_TRUE(isnan(result_pos[6]));
    EXPECT_EQ(result_pos[7], 0.75f);
    EXPECT_EQ(result_pos[8], 0.5f);
    EXPECT_EQ(result_pos[9], 0.75f);
    EXPECT_EQ(result_pos[10], 0.5f);
    EXPECT_EQ(result_pos[11], 0.75f);
    EXPECT_EQ(result_pos[12], 0.5f);
    EXPECT_TRUE(isnan(result_pos[13]));
    EXPECT_EQ(result_pos[14], 0.5f);
    EXPECT_EQ(result_pos[15], 0.25f);
    EXPECT_EQ(result_pos[16], 0.5f);
    EXPECT_EQ(result_pos[17], 0.25f);
    EXPECT_EQ(result_pos[18], 0.5f);
    EXPECT_EQ(result_pos[19], 0.25f);
    EXPECT_TRUE(isnan(result_pos[20]));
    EXPECT_EQ(result_pos[21], 0.25f);
    EXPECT_EQ(result_pos[22], 0.0f);
    EXPECT_EQ(result_pos[23], 0.25f);
    EXPECT_EQ(result_pos[24], 0.0f);
    EXPECT_EQ(result_pos[25], 0.25f);
    EXPECT_EQ(result_pos[26], 0.0f);
    EXPECT_TRUE(isnan(result_pos[27]));
    EXPECT_EQ(result_pos[28], 0.0f);
    EXPECT_EQ(result_pos[29], 1.0f);
    EXPECT_EQ(result_pos[30], 0.0f);
    EXPECT_EQ(result_pos[31], 1.0f);
    EXPECT_EQ(result_pos[32], 0.0f);
    EXPECT_EQ(result_pos[33], 1.0f);
    EXPECT_TRUE(isnan(result_pos[34]));
    EXPECT_EQ(result_pos[35], 1.0f);
    EXPECT_EQ(result_pos[36], 2.0f);
    EXPECT_EQ(result_pos[37], 1.0f);
    EXPECT_EQ(result_pos[38], 2.0f);
    EXPECT_EQ(result_pos[39], 1.0f);
    EXPECT_EQ(result_pos[40], 2.0f);
    EXPECT_TRUE(isnan(result_pos[41]));

    result_idx.read();
    /* x: point_id, y: curve_id, z: curve_segment, w: azimuthal_offset */
    EXPECT_EQ(result_idx[0], int4(0, 0, 0, -1));
    EXPECT_EQ(result_idx[1], int4(1, 0, 1, -1));
    EXPECT_EQ(result_idx[2], int4(0, 0, 0, 0));
    EXPECT_EQ(result_idx[3], int4(1, 0, 1, 0));
    EXPECT_EQ(result_idx[4], int4(0, 0, 0, 1));
    EXPECT_EQ(result_idx[5], int4(1, 0, 1, 1));
    EXPECT_EQ(result_idx[6], int4(0, 0, 0, 2));

    EXPECT_EQ(result_idx[7], int4(1, 0, 1, -1));
    EXPECT_EQ(result_idx[8], int4(2, 0, 2, -1));
    EXPECT_EQ(result_idx[9], int4(1, 0, 1, 0));
    EXPECT_EQ(result_idx[10], int4(2, 0, 2, 0));
    EXPECT_EQ(result_idx[11], int4(1, 0, 1, 1));
    EXPECT_EQ(result_idx[12], int4(2, 0, 2, 1));
    EXPECT_EQ(result_idx[13], int4(1, 0, 1, 2));

    EXPECT_EQ(result_idx[14], int4(2, 0, 2, -1));
    EXPECT_EQ(result_idx[15], int4(3, 0, 3, -1));
    EXPECT_EQ(result_idx[16], int4(2, 0, 2, 0));
    EXPECT_EQ(result_idx[17], int4(3, 0, 3, 0));
    EXPECT_EQ(result_idx[18], int4(2, 0, 2, 1));
    EXPECT_EQ(result_idx[19], int4(3, 0, 3, 1));
    EXPECT_EQ(result_idx[20], int4(2, 0, 2, 2));

    EXPECT_EQ(result_idx[21], int4(3, 0, 3, -1));
    EXPECT_EQ(result_idx[22], int4(4, 0, 4, -1));
    EXPECT_EQ(result_idx[23], int4(3, 0, 3, 0));
    EXPECT_EQ(result_idx[24], int4(4, 0, 4, 0));
    EXPECT_EQ(result_idx[25], int4(3, 0, 3, 1));
    EXPECT_EQ(result_idx[26], int4(4, 0, 4, 1));
    EXPECT_EQ(result_idx[27], int4(3, 0, 3, 2));

    EXPECT_EQ(result_idx[28], int4(5, 1, 0, -1));
    EXPECT_EQ(result_idx[29], int4(6, 1, 1, -1));
    EXPECT_EQ(result_idx[30], int4(5, 1, 0, 0));
    EXPECT_EQ(result_idx[31], int4(6, 1, 1, 0));
    EXPECT_EQ(result_idx[32], int4(5, 1, 0, 1));
    EXPECT_EQ(result_idx[33], int4(6, 1, 1, 1));
    EXPECT_EQ(result_idx[34], int4(5, 1, 0, 2));

    EXPECT_EQ(result_idx[35], int4(6, 1, 1, -1));
    EXPECT_EQ(result_idx[36], int4(7, 1, 2, -1));
    EXPECT_EQ(result_idx[37], int4(6, 1, 1, 0));
    EXPECT_EQ(result_idx[38], int4(7, 1, 2, 0));
    EXPECT_EQ(result_idx[39], int4(6, 1, 1, 1));
    EXPECT_EQ(result_idx[40], int4(7, 1, 2, 1));
    EXPECT_EQ(result_idx[41], int4(6, 1, 1, 2));
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

static void test_draw_curves_topology()
{
  Manager manager;

  GPUShader *sh = GPU_shader_create_from_info_name("draw_curves_topology");

  struct IntBuf {
    int data;
    GPU_VERTEX_FORMAT_FUNC(IntBuf, data);
  };
  gpu::VertBuf *curve_offsets_buf = GPU_vertbuf_create_with_format(IntBuf::format());
  curve_offsets_buf->allocate(4);
  curve_offsets_buf->data<int>().copy_from({0, 5, 8, 10});

  {
    StorageArrayBuffer<int, 512> indirection_buf;
    indirection_buf.clear_to_zero();

    PassSimple pass("Ribbon Curves");
    pass.shader_set(sh);
    pass.bind_ssbo("evaluated_offsets_buf", curve_offsets_buf);
    pass.bind_ssbo("indirection_buf", indirection_buf);
    pass.push_constant("curves_count", 3);
    pass.push_constant("is_ribbon_topology", true);
    pass.dispatch(1);
    pass.barrier(GPU_BARRIER_BUFFER_UPDATE);

    manager.submit(pass);

    /* Note: Expected values follows diagram shown in #142969. */
    indirection_buf.read();

    EXPECT_EQ(indirection_buf[0], 0);
    EXPECT_EQ(indirection_buf[1], -1);
    EXPECT_EQ(indirection_buf[2], -2);
    EXPECT_EQ(indirection_buf[3], -3);
    EXPECT_EQ(indirection_buf[4], -4);
    EXPECT_EQ(indirection_buf[5], 0x7FFFFFFF);
    EXPECT_EQ(indirection_buf[6], 1);
    EXPECT_EQ(indirection_buf[7], -1);
    EXPECT_EQ(indirection_buf[8], -2);
    EXPECT_EQ(indirection_buf[9], 0x7FFFFFFF);
    EXPECT_EQ(indirection_buf[10], 2);
    EXPECT_EQ(indirection_buf[11], -1);
    EXPECT_EQ(indirection_buf[12], 0x7FFFFFFF);
    /* Ensure the rest of the buffer is untouched. */
    EXPECT_EQ(indirection_buf[13], 0);
    EXPECT_EQ(indirection_buf[14], 0);
  }

  {
    StorageArrayBuffer<int, 512> indirection_buf;
    indirection_buf.clear_to_zero();

    PassSimple pass("Cylinder Curves");
    pass.shader_set(sh);
    pass.bind_ssbo("evaluated_offsets_buf", curve_offsets_buf);
    pass.bind_ssbo("indirection_buf", indirection_buf);
    pass.push_constant("curves_count", 3);
    pass.push_constant("is_ribbon_topology", false);
    pass.dispatch(1);
    pass.barrier(GPU_BARRIER_BUFFER_UPDATE);

    manager.submit(pass);

    /* Note: Expected values follows diagram shown in #142969. */
    indirection_buf.read();

    EXPECT_EQ(indirection_buf[0], 0);
    EXPECT_EQ(indirection_buf[1], -1);
    EXPECT_EQ(indirection_buf[2], -2);
    EXPECT_EQ(indirection_buf[3], -3);
    EXPECT_EQ(indirection_buf[4], 1);
    EXPECT_EQ(indirection_buf[5], -1);
    EXPECT_EQ(indirection_buf[6], 2);
    /* Ensure the rest of the buffer is untouched. */
    EXPECT_EQ(indirection_buf[7], 0);
    EXPECT_EQ(indirection_buf[8], 0);
  }

  GPU_shader_unbind();

  GPU_SHADER_FREE_SAFE(sh);
  GPU_VERTBUF_DISCARD_SAFE(curve_offsets_buf);
}
DRAW_TEST(draw_curves_topology)

static void test_draw_curves_interpolation()
{
  Manager manager;

  GPUShader *sh = GPU_shader_create_from_info_name("draw_curves_interpolation");

  const int curve_resolution = 2;

  const Vector<int> evaluated_offsets_data = {0, 5, 8};
  const OffsetIndices<int> evaluated_offsets = evaluated_offsets_data.as_span();

  struct IntBuf {
    int data;
    GPU_VERTEX_FORMAT_FUNC(IntBuf, data);
  };
  gpu::VertBuf *curves_offsets_buf = GPU_vertbuf_create_with_format(IntBuf::format());
  curves_offsets_buf->allocate(3);
  curves_offsets_buf->data<int>().copy_from({0, 3, 5});

  gpu::VertBuf *curves_type_buf = GPU_vertbuf_create_with_format(IntBuf::format());
  curves_type_buf->allocate(2);
  curves_type_buf->data<int>().copy_from({CURVE_TYPE_CATMULL_ROM, CURVE_TYPE_CATMULL_ROM});

  gpu::VertBuf *curves_resolution_buf = GPU_vertbuf_create_with_format(IntBuf::format());
  curves_resolution_buf->allocate(2);
  curves_resolution_buf->data<int>().copy_from({curve_resolution, curve_resolution});

  gpu::VertBuf *curves_evaluated_offsets_buf = GPU_vertbuf_create_with_format(IntBuf::format());
  curves_evaluated_offsets_buf->allocate(3);
  curves_evaluated_offsets_buf->data<int>().copy_from(evaluated_offsets.data());

  const Vector<float> points_radius = {1.0f, 0.5f, 0.0f, 0.0f, 2.0f};
  const Vector<float3> points_pos = {
      float3{1.0f}, float3{0.5f}, float3{0.0f}, float3{0.0f}, float3{2.0f}};

  struct Position {
    float3 pos;
    GPU_VERTEX_FORMAT_FUNC(Position, pos);
  };
  gpu::VertBuf *points_pos_buf = GPU_vertbuf_create_with_format_ex(
      Position::format(), GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);
  points_pos_buf->allocate(points_pos.size());
  points_pos_buf->data<float3>().copy_from(points_pos);

  struct Radius {
    float rad;
    GPU_VERTEX_FORMAT_FUNC(Radius, rad);
  };
  gpu::VertBuf *points_rad_buf = GPU_vertbuf_create_with_format_ex(
      Radius::format(), GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);
  points_rad_buf->allocate(points_radius.size());
  points_rad_buf->data<float>().copy_from(points_radius);

  Vector<float> interp_data;
  interp_data.resize(8);
  {
    StorageArrayBuffer<float4, 512> points_pos_rad_buf;
    StorageArrayBuffer<float, 512> points_time_buf;
    StorageArrayBuffer<float, 512> curves_length_buf;
    points_pos_rad_buf.clear_to_zero();
    points_time_buf.clear_to_zero();
    curves_length_buf.clear_to_zero();

    PassSimple pass("Curves Interpolation Catmull Rom");
    pass.shader_set(sh);
    pass.bind_ssbo("curves_offsets_buf", curves_offsets_buf);
    pass.bind_ssbo("curves_type_buf", curves_type_buf);
    pass.bind_ssbo("curves_resolution_buf", curves_resolution_buf);
    pass.bind_ssbo("curves_evaluated_offsets_buf", curves_evaluated_offsets_buf);
    pass.bind_ssbo("points_pos_buf", points_pos_buf);
    pass.bind_ssbo("points_rad_buf", points_rad_buf);
    pass.bind_ssbo("points_pos_rad_buf", points_pos_rad_buf);
    pass.bind_ssbo("points_time_buf", points_time_buf);
    pass.bind_ssbo("curves_length_buf", curves_length_buf);
    pass.push_constant("curves_count", 2);
    pass.dispatch(1);
    pass.barrier(GPU_BARRIER_BUFFER_UPDATE);

    manager.submit(pass);

    points_pos_rad_buf.read();
    points_time_buf.read();
    curves_length_buf.read();

    bke::curves::catmull_rom::interpolate_to_evaluated(
        GSpan(points_radius.as_span().slice(0, 3)),
        false,
        curve_resolution,
        GMutableSpan(interp_data.as_mutable_span().slice(0, 5)));

    bke::curves::catmull_rom::interpolate_to_evaluated(
        GSpan(points_radius.as_span().slice(3, 2)),
        false,
        curve_resolution,
        GMutableSpan(interp_data.as_mutable_span().slice(5, 3)));

    EXPECT_EQ(points_pos_rad_buf[0], float4(interp_data[0]));
    EXPECT_EQ(points_pos_rad_buf[1], float4(interp_data[1]));
    EXPECT_EQ(points_pos_rad_buf[2], float4(interp_data[2]));
    EXPECT_EQ(points_pos_rad_buf[3], float4(interp_data[3]));
    EXPECT_EQ(points_pos_rad_buf[4], float4(interp_data[4]));
    EXPECT_EQ(points_pos_rad_buf[5], float4(interp_data[5]));
    EXPECT_EQ(points_pos_rad_buf[6], float4(interp_data[6]));
    EXPECT_EQ(points_pos_rad_buf[7], float4(interp_data[7]));
    /* Ensure the rest of the buffer is untouched. */
    EXPECT_EQ(points_pos_rad_buf[8], float4(0.0));
  }

  GPU_shader_unbind();

  GPU_SHADER_FREE_SAFE(sh);
  GPU_VERTBUF_DISCARD_SAFE(curves_offsets_buf);
  GPU_VERTBUF_DISCARD_SAFE(curves_type_buf);
  GPU_VERTBUF_DISCARD_SAFE(curves_resolution_buf);
  GPU_VERTBUF_DISCARD_SAFE(curves_evaluated_offsets_buf);
  GPU_VERTBUF_DISCARD_SAFE(points_pos_buf);
  GPU_VERTBUF_DISCARD_SAFE(points_rad_buf);
}
DRAW_TEST(draw_curves_interpolation)

}  // namespace blender::draw
