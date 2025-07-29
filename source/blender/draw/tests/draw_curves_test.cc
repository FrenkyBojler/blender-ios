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

  const Vector<int> evaluated_offsets = {0, 5, 8};

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
  curves_evaluated_offsets_buf->data<int>().copy_from(evaluated_offsets);

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
    /* Dummy, not used for Catmull-Rom. */
    pass.bind_ssbo("handles_pos_left_buf", curves_evaluated_offsets_buf);
    pass.bind_ssbo("handles_pos_right_buf", curves_evaluated_offsets_buf);
    pass.bind_ssbo("bezier_offsets_buf", curves_evaluated_offsets_buf);
    pass.push_constant("curves_count", 2);
    pass.dispatch(1);
    pass.barrier(GPU_BARRIER_BUFFER_UPDATE);

    manager.submit(pass);

    points_pos_rad_buf.read();
    points_time_buf.read();
    curves_length_buf.read();

    Vector<float> interp_data;
    interp_data.resize(8);

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

    EXPECT_FLOAT_EQ(curves_length_buf[0], numbers::sqrt3);
    EXPECT_FLOAT_EQ(curves_length_buf[1], 2.0f * numbers::sqrt3);

    EXPECT_FLOAT_EQ(points_time_buf[0], 0.0f);
    EXPECT_FLOAT_EQ(points_time_buf[1], 0.218749985f);
    EXPECT_FLOAT_EQ(points_time_buf[2], 0.5f);
    EXPECT_FLOAT_EQ(points_time_buf[3], 0.78125f);
    EXPECT_FLOAT_EQ(points_time_buf[4], 1.0f);
    EXPECT_FLOAT_EQ(points_time_buf[5], 0.0f);
    EXPECT_FLOAT_EQ(points_time_buf[6], 0.5f);
    EXPECT_FLOAT_EQ(points_time_buf[7], 1.0f);
    /* Ensure the rest of the buffer is untouched. */
    EXPECT_EQ(points_time_buf[8], 0.0f);
  }

  const Vector<float3> handle_pos_left = {
      float3{0.0f}, float3{1.0f}, float3{-1.0f}, float3{1.0f}, float3{4.0f}};
  const Vector<float3> handle_pos_right = {
      float3{0.0f}, float3{-1.0f}, float3{1.0f}, float3{-1.0f}, float3{0.0f}};

  gpu::VertBuf *handles_pos_left_buf = GPU_vertbuf_create_with_format_ex(
      Position::format(), GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);
  handles_pos_left_buf->allocate(handle_pos_left.size());
  handles_pos_left_buf->data<float3>().copy_from(handle_pos_left);

  gpu::VertBuf *handles_pos_right_buf = GPU_vertbuf_create_with_format_ex(
      Position::format(), GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);
  handles_pos_right_buf->allocate(handle_pos_right.size());
  handles_pos_right_buf->data<float3>().copy_from(handle_pos_right);

  const Vector<int> bezier_offsets = {0, 2, 4, 5, 0, 2, 3};

  gpu::VertBuf *bezier_offsets_buf = GPU_vertbuf_create_with_format_ex(
      IntBuf::format(), GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);
  bezier_offsets_buf->allocate(bezier_offsets.size());
  bezier_offsets_buf->data<int>().copy_from(bezier_offsets);

  gpu::VertBuf *curves_type_bezier_buf = GPU_vertbuf_create_with_format(IntBuf::format());
  curves_type_bezier_buf->allocate(2);
  curves_type_bezier_buf->data<int>().copy_from({CURVE_TYPE_BEZIER, CURVE_TYPE_BEZIER});

  {
    StorageArrayBuffer<float4, 512> points_pos_rad_buf;
    StorageArrayBuffer<float, 512> points_time_buf;
    StorageArrayBuffer<float, 512> curves_length_buf;
    points_pos_rad_buf.clear_to_zero();
    points_time_buf.clear_to_zero();
    curves_length_buf.clear_to_zero();

    PassSimple pass("Curves Interpolation Bezier");
    pass.shader_set(sh);
    pass.bind_ssbo("curves_offsets_buf", curves_offsets_buf);
    pass.bind_ssbo("curves_type_buf", curves_type_bezier_buf);
    pass.bind_ssbo("curves_resolution_buf", curves_resolution_buf);
    pass.bind_ssbo("curves_evaluated_offsets_buf", curves_evaluated_offsets_buf);
    pass.bind_ssbo("points_pos_buf", points_pos_buf);
    pass.bind_ssbo("points_rad_buf", points_rad_buf);
    pass.bind_ssbo("points_pos_rad_buf", points_pos_rad_buf);
    pass.bind_ssbo("points_time_buf", points_time_buf);
    pass.bind_ssbo("curves_length_buf", curves_length_buf);
    pass.bind_ssbo("handles_pos_left_buf", handles_pos_left_buf);
    pass.bind_ssbo("handles_pos_right_buf", handles_pos_right_buf);
    pass.bind_ssbo("bezier_offsets_buf", bezier_offsets_buf);
    pass.push_constant("curves_count", 2);
    pass.dispatch(1);
    pass.barrier(GPU_BARRIER_BUFFER_UPDATE);

    manager.submit(pass);

    points_pos_rad_buf.read();
    points_time_buf.read();
    curves_length_buf.read();

    Vector<float3> interp_pos;
    interp_pos.resize(8);

    Vector<float> interp_rad;
    interp_rad.resize(8);

    {
      const int curve_index = 0;
      const IndexRange points(0, 3);
      const IndexRange evaluated_points(0, 5);
      const IndexRange offsets = bke::curves::per_curve_point_offsets_range(points, curve_index);

      bke::curves::bezier::calculate_evaluated_positions(
          points_pos.as_span().slice(points),
          handle_pos_left.as_span().slice(points),
          handle_pos_right.as_span().slice(points),
          bezier_offsets.as_span().slice(offsets),
          interp_pos.as_mutable_span().slice(evaluated_points));

      bke::curves::bezier::interpolate_to_evaluated(
          points_radius.as_span().slice(points),
          bezier_offsets.as_span().slice(offsets),
          interp_rad.as_mutable_span().slice(evaluated_points));
    }
    {
      const int curve_index = 1;
      const IndexRange points(3, 2);
      const IndexRange evaluated_points(5, 3);
      const IndexRange offsets = bke::curves::per_curve_point_offsets_range(points, curve_index);

      bke::curves::bezier::calculate_evaluated_positions(
          points_pos.as_span().slice(points),
          handle_pos_left.as_span().slice(points),
          handle_pos_right.as_span().slice(points),
          bezier_offsets.as_span().slice(offsets),
          interp_pos.as_mutable_span().slice(evaluated_points));

      bke::curves::bezier::interpolate_to_evaluated(
          points_radius.as_span().slice(points),
          bezier_offsets.as_span().slice(offsets),
          interp_rad.as_mutable_span().slice(evaluated_points));
    }

    EXPECT_EQ(points_pos_rad_buf[0], float4(interp_pos[0], interp_rad[0]));
    EXPECT_EQ(points_pos_rad_buf[1], float4(interp_pos[1], interp_rad[1]));
    EXPECT_EQ(points_pos_rad_buf[2], float4(interp_pos[2], interp_rad[2]));
    EXPECT_EQ(points_pos_rad_buf[3], float4(interp_pos[3], interp_rad[3]));
    EXPECT_EQ(points_pos_rad_buf[4], float4(interp_pos[4], interp_rad[4]));
    EXPECT_EQ(points_pos_rad_buf[5], float4(interp_pos[5], interp_rad[5]));
    EXPECT_EQ(points_pos_rad_buf[6], float4(interp_pos[6], interp_rad[6]));
    EXPECT_EQ(points_pos_rad_buf[7], float4(interp_pos[7], interp_rad[7]));
    /* Ensure the rest of the buffer is untouched. */
    EXPECT_EQ(points_pos_rad_buf[8], float4(0.0));

    float curve_len[2] = {0.0f, 0.0f};
    Vector<float> interp_time{0.0f};
    interp_time.resize(8);
    interp_time[0] = 0.0f;
    for (int i : IndexRange(1, 4)) {
      curve_len[0] += math::distance(interp_pos[i], interp_pos[i - 1]);
      interp_time[i] = curve_len[0];
    }
    for (int i : IndexRange(1, 4)) {
      interp_time[i] /= curve_len[0];
    }
    interp_time[5] = 0.0f;
    for (int i : IndexRange(6, 2)) {
      curve_len[1] += math::distance(interp_pos[i], interp_pos[i - 1]);
      interp_time[i] = curve_len[1];
    }
    for (int i : IndexRange(6, 2)) {
      interp_time[i] /= curve_len[1];
    }

    EXPECT_FLOAT_EQ(curves_length_buf[0], curve_len[0]);
    EXPECT_FLOAT_EQ(curves_length_buf[1], curve_len[1]);

    EXPECT_FLOAT_EQ(points_time_buf[0], interp_time[0]);
    EXPECT_FLOAT_EQ(points_time_buf[1], interp_time[1]);
    EXPECT_FLOAT_EQ(points_time_buf[2], interp_time[2]);
    EXPECT_FLOAT_EQ(points_time_buf[3], interp_time[3]);
    EXPECT_FLOAT_EQ(points_time_buf[4], interp_time[4]);
    EXPECT_FLOAT_EQ(points_time_buf[5], interp_time[5]);
    EXPECT_FLOAT_EQ(points_time_buf[6], interp_time[6]);
    EXPECT_FLOAT_EQ(points_time_buf[7], interp_time[7]);
    /* Ensure the rest of the buffer is untouched. */
    EXPECT_EQ(points_time_buf[8], 0.0f);
  }

  const bke::curves::nurbs::BasisCache basis_cache_c0 = {
      {0.1f, 0.2f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f},
      {0, 0, 0, 0, 0},
      false,
  };
  const bke::curves::nurbs::BasisCache basis_cache_c1 = {
      {0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 2.0f},
      {0, 0, 0},
      false,
  };

  Vector<int> basis_cache_offset;
  Vector<uint32_t> basis_cache_packed;
  {
    basis_cache_offset.append(basis_cache_packed.size());
    basis_cache_packed.append(basis_cache_c0.invalid);
    basis_cache_packed.extend(
        Span{reinterpret_cast<const uint32_t *>(basis_cache_c0.start_indices.data()),
             basis_cache_c0.start_indices.size()});
    basis_cache_packed.extend(
        Span{reinterpret_cast<const uint32_t *>(basis_cache_c0.weights.data()),
             basis_cache_c0.weights.size()});

    basis_cache_offset.append(basis_cache_packed.size());
    basis_cache_packed.append(basis_cache_c1.invalid);
    basis_cache_packed.extend(
        Span{reinterpret_cast<const uint32_t *>(basis_cache_c1.start_indices.data()),
             basis_cache_c1.start_indices.size()});
    basis_cache_packed.extend(
        Span{reinterpret_cast<const uint32_t *>(basis_cache_c1.weights.data()),
             basis_cache_c1.weights.size()});
  }

  /* Raw data. Shader reinterpret as float or int. */
  gpu::VertBuf *basis_cache_buf = GPU_vertbuf_create_with_format_ex(
      IntBuf::format(), GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);
  basis_cache_buf->allocate(basis_cache_packed.size());
  basis_cache_buf->data<uint>().copy_from(basis_cache_packed);

  gpu::VertBuf *basis_cache_offset_buf = GPU_vertbuf_create_with_format_ex(
      IntBuf::format(), GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);
  basis_cache_offset_buf->allocate(basis_cache_offset.size());
  basis_cache_offset_buf->data<int>().copy_from(basis_cache_offset);

  const Vector<int8_t> curves_order = {3, 2, /* Padding. */ 0, 0};

  gpu::VertBuf *curves_order_buf = GPU_vertbuf_create_with_format_ex(
      IntBuf::format(), GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);
  curves_order_buf->allocate(curves_order.size() / 4);
  curves_order_buf->data<int>().copy_from(
      Span<int>(reinterpret_cast<const int *>(curves_order.data()), curves_order.size() / 4));

  const Vector<float> control_weights = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};

  gpu::VertBuf *control_weights_buf = GPU_vertbuf_create_with_format(Radius::format());
  control_weights_buf->allocate(control_weights.size());
  control_weights_buf->data<float>().copy_from(control_weights);

  gpu::VertBuf *curves_type_nurbs_buf = GPU_vertbuf_create_with_format(IntBuf::format());
  curves_type_nurbs_buf->allocate(2);
  curves_type_nurbs_buf->data<int>().copy_from({CURVE_TYPE_NURBS, CURVE_TYPE_NURBS});

  {
    StorageArrayBuffer<float4, 512> points_pos_rad_buf;
    StorageArrayBuffer<float, 512> points_time_buf;
    StorageArrayBuffer<float, 512> curves_length_buf;
    points_pos_rad_buf.clear_to_zero();
    points_time_buf.clear_to_zero();
    curves_length_buf.clear_to_zero();

    PassSimple pass("Curves Interpolation Nurbs");
    pass.shader_set(sh);
    pass.bind_ssbo("curves_offsets_buf", curves_offsets_buf);
    pass.bind_ssbo("curves_type_buf", curves_type_nurbs_buf);
    pass.bind_ssbo("curves_resolution_buf", curves_order_buf);
    pass.bind_ssbo("curves_evaluated_offsets_buf", curves_evaluated_offsets_buf);
    pass.bind_ssbo("points_pos_buf", points_pos_buf);
    pass.bind_ssbo("points_rad_buf", points_rad_buf);
    pass.bind_ssbo("points_pos_rad_buf", points_pos_rad_buf);
    pass.bind_ssbo("points_time_buf", points_time_buf);
    pass.bind_ssbo("curves_length_buf", curves_length_buf);
    pass.bind_ssbo("handles_pos_left_buf", basis_cache_buf);
    pass.bind_ssbo("handles_pos_right_buf", control_weights_buf);
    pass.bind_ssbo("bezier_offsets_buf", basis_cache_offset_buf);
    pass.push_constant("curves_count", 2);
    pass.dispatch(1);
    pass.barrier(GPU_BARRIER_BUFFER_UPDATE);

    manager.submit(pass);

    points_pos_rad_buf.read();
    points_time_buf.read();
    curves_length_buf.read();

    Vector<float3> interp_pos;
    interp_pos.resize(8);

    Vector<float> interp_rad;
    interp_rad.resize(8);

    {
      const int curve_index = 0;
      const IndexRange points(0, 3);
      const IndexRange evaluated_points(0, 5);

      bke::curves::nurbs::interpolate_to_evaluated(
          basis_cache_c0,
          curves_order[curve_index],
          control_weights.as_span().slice(points),
          points_pos.as_span().slice(points),
          interp_pos.as_mutable_span().slice(evaluated_points));

      bke::curves::nurbs::interpolate_to_evaluated(
          basis_cache_c0,
          curves_order[curve_index],
          control_weights.as_span().slice(points),
          points_radius.as_span().slice(points),
          interp_rad.as_mutable_span().slice(evaluated_points));
    }
    {
      const int curve_index = 1;
      const IndexRange points(3, 2);
      const IndexRange evaluated_points(5, 3);

      bke::curves::nurbs::interpolate_to_evaluated(
          basis_cache_c1,
          curves_order[curve_index],
          control_weights.as_span().slice(points),
          points_pos.as_span().slice(points),
          interp_pos.as_mutable_span().slice(evaluated_points));

      bke::curves::nurbs::interpolate_to_evaluated(
          basis_cache_c1,
          curves_order[curve_index],
          control_weights.as_span().slice(points),
          points_radius.as_span().slice(points),
          interp_rad.as_mutable_span().slice(evaluated_points));
    }

    EXPECT_NEAR(points_pos_rad_buf[0].x, interp_pos[0].x, 0.000001f);
    EXPECT_NEAR(points_pos_rad_buf[1].x, interp_pos[1].x, 0.000001f);
    EXPECT_NEAR(points_pos_rad_buf[2].x, interp_pos[2].x, 0.000001f);
    EXPECT_NEAR(points_pos_rad_buf[3].x, interp_pos[3].x, 0.000001f);
    EXPECT_NEAR(points_pos_rad_buf[4].x, interp_pos[4].x, 0.000001f);
    EXPECT_NEAR(points_pos_rad_buf[5].x, interp_pos[5].x, 0.000001f);
    EXPECT_NEAR(points_pos_rad_buf[6].x, interp_pos[6].x, 0.000001f);
    EXPECT_NEAR(points_pos_rad_buf[7].x, interp_pos[7].x, 0.000001f);

    EXPECT_NEAR(points_pos_rad_buf[0].w, interp_rad[0], 0.000001f);
    EXPECT_NEAR(points_pos_rad_buf[1].w, interp_rad[1], 0.000001f);
    EXPECT_NEAR(points_pos_rad_buf[2].w, interp_rad[2], 0.000001f);
    EXPECT_NEAR(points_pos_rad_buf[3].w, interp_rad[3], 0.000001f);
    EXPECT_NEAR(points_pos_rad_buf[4].w, interp_rad[4], 0.000001f);
    EXPECT_NEAR(points_pos_rad_buf[5].w, interp_rad[5], 0.000001f);
    EXPECT_NEAR(points_pos_rad_buf[6].w, interp_rad[6], 0.000001f);
    EXPECT_NEAR(points_pos_rad_buf[7].w, interp_rad[7], 0.000001f);
    /* Ensure the rest of the buffer is untouched. */
    EXPECT_EQ(points_pos_rad_buf[8], float4(0.0));

    float curve_len[2] = {0.0f, 0.0f};
    Vector<float> interp_time{0.0f};
    interp_time.resize(8);
    interp_time[0] = 0.0f;
    for (int i : IndexRange(1, 4)) {
      curve_len[0] += math::distance(interp_pos[i], interp_pos[i - 1]);
      interp_time[i] = curve_len[0];
    }
    for (int i : IndexRange(1, 4)) {
      interp_time[i] /= curve_len[0];
    }
    interp_time[5] = 0.0f;
    for (int i : IndexRange(6, 2)) {
      curve_len[1] += math::distance(interp_pos[i], interp_pos[i - 1]);
      interp_time[i] = curve_len[1];
    }
    for (int i : IndexRange(6, 2)) {
      interp_time[i] /= curve_len[1];
    }

    EXPECT_NEAR(curves_length_buf[0], curve_len[0], 0.000001f);
    EXPECT_NEAR(curves_length_buf[1], curve_len[1], 0.000001f);

    EXPECT_EQ(points_time_buf[0], interp_time[0]);
    EXPECT_NEAR(points_time_buf[1], interp_time[1], 0.000001f);
    EXPECT_NEAR(points_time_buf[2], interp_time[2], 0.000001f);
    EXPECT_NEAR(points_time_buf[3], interp_time[3], 0.000001f);
    EXPECT_NEAR(points_time_buf[4], interp_time[4], 0.000001f);
    EXPECT_EQ(points_time_buf[5], interp_time[5]);
    EXPECT_NEAR(points_time_buf[6], interp_time[6], 0.000001f);
    EXPECT_NEAR(points_time_buf[7], interp_time[7], 0.000001f);
    /* Ensure the rest of the buffer is untouched. */
    EXPECT_EQ(points_time_buf[8], 0.0f);
  }

  GPU_shader_unbind();

  GPU_SHADER_FREE_SAFE(sh);
  GPU_VERTBUF_DISCARD_SAFE(curves_offsets_buf);
  GPU_VERTBUF_DISCARD_SAFE(curves_type_buf);
  GPU_VERTBUF_DISCARD_SAFE(curves_type_bezier_buf);
  GPU_VERTBUF_DISCARD_SAFE(curves_type_nurbs_buf);
  GPU_VERTBUF_DISCARD_SAFE(curves_resolution_buf);
  GPU_VERTBUF_DISCARD_SAFE(curves_evaluated_offsets_buf);
  GPU_VERTBUF_DISCARD_SAFE(points_pos_buf);
  GPU_VERTBUF_DISCARD_SAFE(points_rad_buf);
  GPU_VERTBUF_DISCARD_SAFE(handles_pos_left_buf);
  GPU_VERTBUF_DISCARD_SAFE(handles_pos_right_buf);
  GPU_VERTBUF_DISCARD_SAFE(bezier_offsets_buf);
  GPU_VERTBUF_DISCARD_SAFE(basis_cache_buf);
  GPU_VERTBUF_DISCARD_SAFE(basis_cache_offset_buf);
  GPU_VERTBUF_DISCARD_SAFE(curves_order_buf);
  GPU_VERTBUF_DISCARD_SAFE(control_weights_buf);
}
DRAW_TEST(draw_curves_interpolation)

}  // namespace blender::draw
