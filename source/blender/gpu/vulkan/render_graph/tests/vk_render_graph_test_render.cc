/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "vk_render_graph_test_types.hh"

namespace blender::gpu::render_graph {

class VKRenderGraphTestRender : public VKRenderGraphTest_P {};

TEST_P(VKRenderGraphTestRender, begin_clear_attachments_end_read_back)
{
  VkHandle<VkImage> image(1u);
  VkHandle<VkImageView> image_view(2u);
  VkHandle<VkBuffer> buffer(3u);

  resources.add_image(image, false);
  resources.add_buffer(buffer);

  {
    VKResourceAccessInfo access_info = {};
    access_info.images.append(
        {image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, {}});
    VKBeginRenderingNode::CreateInfo begin_rendering(access_info);
    begin_rendering.node_data.color_attachments[0].sType =
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    begin_rendering.node_data.color_attachments[0].imageLayout = color_attachment_layout();
    begin_rendering.node_data.color_attachments[0].imageView = image_view;
    begin_rendering.node_data.color_attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    begin_rendering.node_data.color_attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    begin_rendering.node_data.vk_rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    begin_rendering.node_data.vk_rendering_info.colorAttachmentCount = 1;
    begin_rendering.node_data.vk_rendering_info.layerCount = 1;
    begin_rendering.node_data.vk_rendering_info.pColorAttachments =
        begin_rendering.node_data.color_attachments;

    render_graph->add_node(begin_rendering);
  }

  {
    VKClearAttachmentsNode::CreateInfo clear_attachments = {};
    clear_attachments.attachment_count = 1;
    clear_attachments.attachments[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear_attachments.attachments[0].clearValue.color.float32[0] = 0.2;
    clear_attachments.attachments[0].clearValue.color.float32[1] = 0.4;
    clear_attachments.attachments[0].clearValue.color.float32[2] = 0.6;
    clear_attachments.attachments[0].clearValue.color.float32[3] = 1.0;
    clear_attachments.attachments[0].colorAttachment = 0;
    clear_attachments.vk_clear_rect.baseArrayLayer = 0;
    clear_attachments.vk_clear_rect.layerCount = 1;
    clear_attachments.vk_clear_rect.rect.extent.width = 1920;
    clear_attachments.vk_clear_rect.rect.extent.height = 1080;
    render_graph->add_node(clear_attachments);
  }

  {
    VKEndRenderingNode::CreateInfo end_rendering = {};
    render_graph->add_node(end_rendering);
  }

  {
    VKCopyImageToBufferNode::CreateInfo copy_image_to_buffer = {};
    copy_image_to_buffer.node_data.src_image = image;
    copy_image_to_buffer.node_data.dst_buffer = buffer;
    copy_image_to_buffer.node_data.region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_image_to_buffer.vk_image_aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    render_graph->add_node(copy_image_to_buffer);
  }

  submit(render_graph, command_buffer);

  EXPECT_EQ(6, log.size());
  EXPECT_EQ(
      "pipeline_barrier(src_stage_mask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, "
      "dst_stage_mask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT" +
          endl() +
          " - image_barrier(src_access_mask=, "
          "dst_access_mask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, "
          "old_layout=VK_IMAGE_LAYOUT_UNDEFINED, "
          "new_layout=" +
          color_attachment_layout_str() +
          ", image=0x1, "
          "subresource_range=" +
          endl() +
          "    aspect_mask=VK_IMAGE_ASPECT_COLOR_BIT, base_mip_level=0, level_count=4294967295, "
          "base_array_layer=0, layer_count=4294967295  )" +
          endl() + ")",
      log[0]);
  EXPECT_EQ("begin_rendering(p_rendering_info=flags=, render_area=" + endl() +
                "  offset=" + endl() + "    x=0, y=0  , extent=" + endl() +
                "    width=0, height=0  , layer_count=1, view_mask=0, color_attachment_count=1, "
                "p_color_attachments=" +
                endl() + "  image_view=0x2, image_layout=" + color_attachment_layout_str() +
                ", "
                "resolve_mode=VK_RESOLVE_MODE_NONE, resolve_image_view=0, "
                "resolve_image_layout=VK_IMAGE_LAYOUT_UNDEFINED, "
                "load_op=VK_ATTACHMENT_LOAD_OP_DONT_CARE, store_op=VK_ATTACHMENT_STORE_OP_STORE" +
                endl() + ")",
            log[1]);
  EXPECT_EQ(
      "clear_attachments( - attachment(aspect_mask=VK_IMAGE_ASPECT_COLOR_BIT, "
      "color_attachment=0)" +
          endl() + " - rect(rect=" + endl() + "    offset=" + endl() +
          "      x=0, y=0    , extent=" + endl() +
          "      width=1920, height=1080      , base_array_layer=0, layer_count=1)" + endl() + ")",
      log[2]);
  EXPECT_EQ("end_rendering()", log[3]);
  EXPECT_EQ(
      "pipeline_barrier(src_stage_mask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, "
      "dst_stage_mask=VK_PIPELINE_STAGE_TRANSFER_BIT" +
          endl() +
          " - image_barrier(src_access_mask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, "
          "dst_access_mask=VK_ACCESS_TRANSFER_READ_BIT, "
          "old_layout=" +
          color_attachment_layout_str() +
          ", "
          "new_layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image=0x1, subresource_range=" +
          endl() +
          "    aspect_mask=VK_IMAGE_ASPECT_COLOR_BIT, base_mip_level=0, level_count=4294967295, "
          "base_array_layer=0, layer_count=4294967295  )" +
          endl() + ")",
      log[4]);
  EXPECT_EQ(
      "copy_image_to_buffer(src_image=0x1, src_image_layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, "
      "dst_buffer=0x3" +
          endl() +
          " - region(buffer_offset=0, buffer_row_length=0, buffer_image_height=0, "
          "image_subresource=" +
          endl() +
          "    aspect_mask=VK_IMAGE_ASPECT_COLOR_BIT, mip_level=0, base_array_layer=0, "
          "layer_count=0  , image_offset=" +
          endl() + "    x=0, y=0, z=0  , image_extent=" + endl() +
          "    width=0, height=0, depth=0  )" + endl() + ")",
      log[5]);
}

TEST_P(VKRenderGraphTestRender, begin_draw_end)
{
  VkHandle<VkImage> image(1u);
  VkHandle<VkImageView> image_view(2u);
  VkHandle<VkPipelineLayout> pipeline_layout(4u);
  VkHandle<VkPipeline> pipeline(3u);

  resources.add_image(image, false);

  {
    VKResourceAccessInfo access_info = {};
    access_info.images.append(
        {image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, {}});
    VKBeginRenderingNode::CreateInfo begin_rendering(access_info);
    begin_rendering.node_data.color_attachments[0].sType =
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    begin_rendering.node_data.color_attachments[0].imageLayout = color_attachment_layout();
    begin_rendering.node_data.color_attachments[0].imageView = image_view;
    begin_rendering.node_data.color_attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    begin_rendering.node_data.color_attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    begin_rendering.node_data.vk_rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    begin_rendering.node_data.vk_rendering_info.colorAttachmentCount = 1;
    begin_rendering.node_data.vk_rendering_info.layerCount = 1;
    begin_rendering.node_data.vk_rendering_info.pColorAttachments =
        begin_rendering.node_data.color_attachments;

    render_graph->add_node(begin_rendering);
  }

  {
    VKResourceAccessInfo access_info = {};
    VKDrawNode::CreateInfo draw(access_info);
    draw.node_data.first_instance = 0;
    draw.node_data.first_vertex = 0;
    draw.node_data.instance_count = 1;
    draw.node_data.vertex_count = 4;
    draw.node_data.graphics.pipeline_data.push_constants_range = IndexRange(0);
    draw.node_data.graphics.pipeline_data.vk_descriptor_set = VK_NULL_HANDLE;
    draw.node_data.graphics.pipeline_data.vk_pipeline = pipeline;
    draw.node_data.graphics.pipeline_data.vk_pipeline_layout = pipeline_layout;
    draw.node_data.graphics.viewport.viewports.append(VkViewport{});
    draw.node_data.graphics.viewport.scissors.append(VkRect2D{});
    render_graph->add_node(draw);
  }

  {
    VKEndRenderingNode::CreateInfo end_rendering = {};
    render_graph->add_node(end_rendering);
  }

  submit(render_graph, command_buffer);
  EXPECT_EQ(7, log.size());
  EXPECT_EQ(
      "pipeline_barrier(src_stage_mask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, "
      "dst_stage_mask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT" +
          endl() +
          " - image_barrier(src_access_mask=, "
          "dst_access_mask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, "
          "old_layout=VK_IMAGE_LAYOUT_UNDEFINED, "
          "new_layout=" +
          color_attachment_layout_str() + ", image=0x1, subresource_range=" + endl() +
          "    aspect_mask=VK_IMAGE_ASPECT_COLOR_BIT, base_mip_level=0, "
          "level_count=4294967295, base_array_layer=0, layer_count=4294967295  )" +
          endl() + ")",
      log[0]);
  EXPECT_EQ("begin_rendering(p_rendering_info=flags=, render_area=" + endl() +
                "  offset=" + endl() + "    x=0, y=0  , extent=" + endl() +
                "    width=0, height=0  , layer_count=1, view_mask=0, "
                "color_attachment_count=1, p_color_attachments=" +
                endl() + "  image_view=0x2, image_layout=" + color_attachment_layout_str() +
                ", "
                "resolve_mode=VK_RESOLVE_MODE_NONE, resolve_image_view=0, "
                "resolve_image_layout=VK_IMAGE_LAYOUT_UNDEFINED, "
                "load_op=VK_ATTACHMENT_LOAD_OP_DONT_CARE, store_op=VK_ATTACHMENT_STORE_OP_STORE" +
                endl() + ")",
            log[1]);
  EXPECT_EQ("set_viewport(num_viewports=1)", log[2]);
  EXPECT_EQ("set_scissor(num_scissors=1)", log[3]);
  EXPECT_EQ("bind_pipeline(pipeline_bind_point=VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline=0x3)",
            log[4]);
  EXPECT_EQ("draw(vertex_count=4, instance_count=1, first_vertex=0, first_instance=0)", log[5]);
  EXPECT_EQ("end_rendering()", log[6]);
}

TEST_P(VKRenderGraphTestRender, begin_draw_end__layered)
{
  VkHandle<VkImage> image(1u);
  VkHandle<VkImageView> image_view(2u);
  VkHandle<VkPipelineLayout> pipeline_layout(4u);
  VkHandle<VkPipeline> pipeline(3u);

  resources.add_image(image, true);

  {
    VKResourceAccessInfo access_info = {};
    access_info.images.append({image,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT,
                               {0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}});
    VKBeginRenderingNode::CreateInfo begin_rendering(access_info);
    begin_rendering.node_data.color_attachments[0].sType =
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    begin_rendering.node_data.color_attachments[0].imageLayout = color_attachment_layout();
    begin_rendering.node_data.color_attachments[0].imageView = image_view;
    begin_rendering.node_data.color_attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    begin_rendering.node_data.color_attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    begin_rendering.node_data.vk_rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    begin_rendering.node_data.vk_rendering_info.colorAttachmentCount = 1;
    begin_rendering.node_data.vk_rendering_info.layerCount = 1;
    begin_rendering.node_data.vk_rendering_info.pColorAttachments =
        begin_rendering.node_data.color_attachments;

    render_graph->add_node(begin_rendering);
  }

  {
    VKResourceAccessInfo access_info = {};
    access_info.images.append({image,
                               VK_ACCESS_SHADER_READ_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT,
                               {0, VK_REMAINING_MIP_LEVELS, 1, VK_REMAINING_ARRAY_LAYERS}});
    VKDrawNode::CreateInfo draw(access_info);
    draw.node_data.first_instance = 0;
    draw.node_data.first_vertex = 0;
    draw.node_data.instance_count = 1;
    draw.node_data.vertex_count = 4;
    draw.node_data.graphics.pipeline_data.push_constants_range = IndexRange(0);
    draw.node_data.graphics.pipeline_data.vk_descriptor_set = VK_NULL_HANDLE;
    draw.node_data.graphics.pipeline_data.vk_pipeline = pipeline;
    draw.node_data.graphics.pipeline_data.vk_pipeline_layout = pipeline_layout;
    draw.node_data.graphics.viewport.viewports.append(VkViewport{});
    draw.node_data.graphics.viewport.scissors.append(VkRect2D{});
    render_graph->add_node(draw);
  }

  {
    VKEndRenderingNode::CreateInfo end_rendering = {};
    render_graph->add_node(end_rendering);
  }

  submit(render_graph, command_buffer);
  EXPECT_EQ(9, log.size());
  EXPECT_EQ(
      "pipeline_barrier(src_stage_mask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, "
      "dst_stage_mask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT" +
          endl() +
          " - image_barrier(src_access_mask=, "
          "dst_access_mask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, "
          "old_layout=VK_IMAGE_LAYOUT_UNDEFINED, "
          "new_layout=" +
          color_attachment_layout_str() + ", image=0x1, subresource_range=" + endl() +
          "    aspect_mask=VK_IMAGE_ASPECT_COLOR_BIT, base_mip_level=0, "
          "level_count=4294967295, base_array_layer=0, layer_count=4294967295  )" +
          endl() + ")",
      log[0]);
  EXPECT_EQ(
      "pipeline_barrier(src_stage_mask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, "
      "dst_stage_mask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT" +
          endl() +
          " - image_barrier(src_access_mask=VK_ACCESS_TRANSFER_WRITE_BIT, "
          "dst_access_mask=VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT, "
          "VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, "
          "VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, "
          "old_layout=" +
          color_attachment_layout_str() +
          ", "
          "new_layout=VK_IMAGE_LAYOUT_GENERAL, "
          "image=0x1, subresource_range=" +
          endl() +
          "    aspect_mask=VK_IMAGE_ASPECT_COLOR_BIT, base_mip_level=0, level_count=4294967295, "
          "base_array_layer=1, layer_count=4294967295  )" +
          endl() + ")",
      log[1]);
  EXPECT_EQ("begin_rendering(p_rendering_info=flags=, render_area=" + endl() +
                "  offset=" + endl() + "    x=0, y=0  , extent=" + endl() +
                "    width=0, height=0  , layer_count=1, view_mask=0, "
                "color_attachment_count=1, p_color_attachments=" +
                endl() + "  image_view=0x2, image_layout=" + color_attachment_layout_str() +
                ", "
                "resolve_mode=VK_RESOLVE_MODE_NONE, resolve_image_view=0, "
                "resolve_image_layout=VK_IMAGE_LAYOUT_UNDEFINED, "
                "load_op=VK_ATTACHMENT_LOAD_OP_DONT_CARE, store_op=VK_ATTACHMENT_STORE_OP_STORE" +
                endl() + ")",
            log[2]);
  EXPECT_EQ("set_viewport(num_viewports=1)", log[3]);
  EXPECT_EQ("set_scissor(num_scissors=1)", log[4]);
  EXPECT_EQ("bind_pipeline(pipeline_bind_point=VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline=0x3)",
            log[5]);
  EXPECT_EQ("draw(vertex_count=4, instance_count=1, first_vertex=0, first_instance=0)", log[6]);
  EXPECT_EQ("end_rendering()", log[7]);
  EXPECT_EQ(
      "pipeline_barrier(src_stage_mask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, "
      "dst_stage_mask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT" +
          endl() +
          " - image_barrier(src_access_mask=VK_ACCESS_SHADER_READ_BIT, "
          "VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, "
          "VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, "
          "VK_ACCESS_TRANSFER_WRITE_BIT, dst_access_mask=VK_ACCESS_SHADER_READ_BIT, "
          "VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, "
          "VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, "
          "VK_ACCESS_TRANSFER_WRITE_BIT, old_layout=VK_IMAGE_LAYOUT_GENERAL, "
          "new_layout=" +
          color_attachment_layout_str() + ", image=0x1, subresource_range=" + endl() +
          "    aspect_mask=VK_IMAGE_ASPECT_COLOR_BIT, base_mip_level=0, level_count=4294967295, "
          "base_array_layer=1, layer_count=4294967295  )" +
          endl() + ")",
      log[8]);
}

INSTANTIATE_TEST_SUITE_P(, VKRenderGraphTestRender, ::testing::Values(true, false));

class VKRenderGraphTestBarrierMerge : public VKRenderGraphTest {};

/**
 * Test that pre-barriers are merged when they have the same src/dst stage masks
 * within the same rendering group.
 */
TEST_P(VKRenderGraphTestRender, merge_within_rendering_group)
{
  VkHandle<VkBuffer> buffer_a(1u);
  VkHandle<VkBuffer> buffer_b(2u);
  VkHandle<VkImage> image(3u);
  VkHandle<VkImageView> image_view(4u);

  resources.add_buffer(buffer_a);
  resources.add_buffer(buffer_b);
  resources.add_image(image, false);

  render_graph->add_node(VKFillBufferNode::CreateInfo{buffer_a, 1024, 42});
  render_graph->add_node(VKFillBufferNode::CreateInfo{buffer_b, 1024, 43});

  {
    VKResourceAccessInfo access_info = {};
    access_info.images.append(
        {image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, {}});
    VKBeginRenderingNode::CreateInfo begin_rendering(access_info);
    begin_rendering.node_data.color_attachments[0].sType =
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    begin_rendering.node_data.color_attachments[0].imageLayout = color_attachment_layout();
    begin_rendering.node_data.color_attachments[0].imageView = image_view;
    begin_rendering.node_data.color_attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    begin_rendering.node_data.color_attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    begin_rendering.node_data.vk_rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    begin_rendering.node_data.vk_rendering_info.colorAttachmentCount = 1;
    begin_rendering.node_data.vk_rendering_info.layerCount = 1;
    begin_rendering.node_data.vk_rendering_info.pColorAttachments =
        begin_rendering.node_data.color_attachments;
    render_graph->add_node(begin_rendering);
  }

  {
    VKResourceAccessInfo access_info = {};
    access_info.buffers.append({buffer_a, VK_ACCESS_SHADER_READ_BIT});
    VKDrawNode::CreateInfo draw(access_info);
    draw.node_data.vertex_count = 3;
    draw.node_data.instance_count = 1;
    render_graph->add_node(draw);
  }

  {
    VKResourceAccessInfo access_info = {};
    access_info.buffers.append({buffer_b, VK_ACCESS_SHADER_READ_BIT});
    VKDrawNode::CreateInfo draw(access_info);
    draw.node_data.vertex_count = 3;
    draw.node_data.instance_count = 1;
    render_graph->add_node(draw);
  }

  render_graph->add_node(VKEndRenderingNode::CreateInfo{});
  submit(render_graph, command_buffer);

  ASSERT_EQ(8, log.size());

  EXPECT_EQ("fill_buffer(dst_buffer=0x1, dst_offset=0, size=1024, data=42)", log[0]);
  EXPECT_EQ("fill_buffer(dst_buffer=0x2, dst_offset=0, size=1024, data=43)", log[1]);

  /* pipeline_barrier(src=TOP_OF_PIPE, dst=ALL_GRAPHICS, image_barrier for attachment image=0x3) */
  EXPECT_TRUE(log[2].find("pipeline_barrier") != std::string::npos);
  EXPECT_TRUE(log[2].find("image=0x3") != std::string::npos);

  /* MERGED pipeline_barrier(src=TRANSFER, dst=ALL_GRAPHICS,
   *   buffer_barrier(buffer=0x1), buffer_barrier(buffer=0x2)) */
  EXPECT_TRUE(log[3].find("pipeline_barrier") != std::string::npos);
  EXPECT_TRUE(log[3].find("buffer=0x1") != std::string::npos);
  EXPECT_TRUE(log[3].find("buffer=0x2") != std::string::npos);
  int buffer_barrier_count = 0;
  size_t search_pos = log[3].find("buffer_barrier(");
  while (search_pos != std::string::npos) {
    buffer_barrier_count++;
    search_pos = log[3].find("buffer_barrier(", search_pos + 1);
  }
  EXPECT_EQ(2, buffer_barrier_count);

  /* begin_rendering(...) */
  EXPECT_TRUE(log[4].find("begin_rendering") != std::string::npos);
  /* draw(...) */
  EXPECT_TRUE(log[5].find("draw(") != std::string::npos);
  /* draw(...) */
  EXPECT_TRUE(log[6].find("draw(") != std::string::npos);
  EXPECT_EQ("end_rendering()", log[7]);
}

/**
 * Test that pre-barriers in different groups are NOT merged even when they have
 * identical stage masks.
 */
TEST_F(VKRenderGraphTestBarrierMerge, no_merge_across_groups)
{
  VkHandle<VkBuffer> buffer_a(1u);
  VkHandle<VkBuffer> buffer_b(2u);
  VkHandle<VkBuffer> staging_a(3u);
  VkHandle<VkBuffer> staging_b(4u);

  resources.add_buffer(buffer_a);
  resources.add_buffer(buffer_b);
  resources.add_buffer(staging_a);
  resources.add_buffer(staging_b);

  render_graph->add_node(VKFillBufferNode::CreateInfo{buffer_a, 1024, 42});
  render_graph->add_node(VKFillBufferNode::CreateInfo{buffer_b, 1024, 43});

  {
    VKCopyBufferNode::CreateInfo copy_buffer = {};
    copy_buffer.src_buffer = buffer_a;
    copy_buffer.dst_buffer = staging_a;
    copy_buffer.region.srcOffset = 0;
    copy_buffer.region.dstOffset = 0;
    copy_buffer.region.size = 1024;
    render_graph->add_node(copy_buffer);
  }

  {
    VKCopyBufferNode::CreateInfo copy_buffer = {};
    copy_buffer.src_buffer = buffer_b;
    copy_buffer.dst_buffer = staging_b;
    copy_buffer.region.srcOffset = 0;
    copy_buffer.region.dstOffset = 0;
    copy_buffer.region.size = 1024;
    render_graph->add_node(copy_buffer);
  }

  submit(render_graph, command_buffer);

  ASSERT_EQ(6, log.size());
  EXPECT_EQ("fill_buffer(dst_buffer=0x1, dst_offset=0, size=1024, data=42)", log[0]);
  EXPECT_EQ("fill_buffer(dst_buffer=0x2, dst_offset=0, size=1024, data=43)", log[1]);

  /* pipeline_barrier(src=TRANSFER, dst=TRANSFER, buffer_barrier(buffer=0x1)) — NOT merged across groups */
  EXPECT_TRUE(log[2].find("pipeline_barrier") != std::string::npos);
  EXPECT_TRUE(log[2].find("buffer=0x1") != std::string::npos);
  EXPECT_TRUE(log[2].find("buffer_barrier(") != std::string::npos);
  EXPECT_EQ(std::string::npos, log[2].find("buffer_barrier(", log[2].find("buffer_barrier(") + 1));

  /* copy_buffer(src=0x1, dst=0x3) */
  EXPECT_TRUE(log[3].find("copy_buffer") != std::string::npos);
  EXPECT_TRUE(log[3].find("src_buffer=0x1") != std::string::npos);
  EXPECT_TRUE(log[3].find("dst_buffer=0x3") != std::string::npos);

  /* pipeline_barrier(src=TRANSFER, dst=TRANSFER, buffer_barrier(buffer=0x2)) — separate group, NOT merged */
  EXPECT_TRUE(log[4].find("pipeline_barrier") != std::string::npos);
  EXPECT_TRUE(log[4].find("buffer=0x2") != std::string::npos);
  EXPECT_TRUE(log[4].find("buffer_barrier(") != std::string::npos);
  EXPECT_EQ(std::string::npos, log[4].find("buffer_barrier(", log[4].find("buffer_barrier(") + 1));

  /* copy_buffer(src=0x2, dst=0x4) */
  EXPECT_TRUE(log[5].find("copy_buffer") != std::string::npos);
  EXPECT_TRUE(log[5].find("src_buffer=0x2") != std::string::npos);
  EXPECT_TRUE(log[5].find("dst_buffer=0x4") != std::string::npos);
}

/**
 * Test that the merge_pre_barrier does not merge when stage masks differ.
 */
TEST_P(VKRenderGraphTestRender, no_merge_different_stage_masks)
{
  VkHandle<VkBuffer> buffer(1u);
  VkHandle<VkImage> image(2u);
  VkHandle<VkImageView> image_view(3u);

  resources.add_buffer(buffer);
  resources.add_image(image, false);

  render_graph->add_node(VKFillBufferNode::CreateInfo{buffer, 1024, 42});

  {
    VKResourceAccessInfo access_info = {};
    access_info.images.append(
        {image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, {}});
    VKBeginRenderingNode::CreateInfo begin_rendering(access_info);
    begin_rendering.node_data.color_attachments[0].sType =
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    begin_rendering.node_data.color_attachments[0].imageLayout = color_attachment_layout();
    begin_rendering.node_data.color_attachments[0].imageView = image_view;
    begin_rendering.node_data.color_attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    begin_rendering.node_data.color_attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    begin_rendering.node_data.vk_rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    begin_rendering.node_data.vk_rendering_info.colorAttachmentCount = 1;
    begin_rendering.node_data.vk_rendering_info.layerCount = 1;
    begin_rendering.node_data.vk_rendering_info.pColorAttachments =
        begin_rendering.node_data.color_attachments;
    render_graph->add_node(begin_rendering);
  }

  {
    VKResourceAccessInfo access_info = {};
    access_info.buffers.append({buffer, VK_ACCESS_SHADER_READ_BIT});
    VKDrawNode::CreateInfo draw(access_info);
    draw.node_data.vertex_count = 3;
    draw.node_data.instance_count = 1;
    render_graph->add_node(draw);
  }

  render_graph->add_node(VKEndRenderingNode::CreateInfo{});
  submit(render_graph, command_buffer);

  ASSERT_EQ(6, log.size());
  EXPECT_EQ("fill_buffer(dst_buffer=0x1, dst_offset=0, size=1024, data=42)", log[0]);

  /* pipeline_barrier(src=TOP_OF_PIPE, dst=ALL_GRAPHICS, image_barrier for image=0x2) */
  EXPECT_TRUE(log[1].find("pipeline_barrier") != std::string::npos);
  EXPECT_TRUE(log[1].find("image=0x2") != std::string::npos);

  /* pipeline_barrier(src=TRANSFER, dst=ALL_GRAPHICS, buffer_barrier for buffer=0x1) — different src_stage_mask, NOT merged */
  EXPECT_TRUE(log[2].find("pipeline_barrier") != std::string::npos);
  EXPECT_TRUE(log[2].find("buffer=0x1") != std::string::npos);

  /* begin_rendering(...) */
  EXPECT_TRUE(log[3].find("begin_rendering") != std::string::npos);
  /* draw(...) */
  EXPECT_TRUE(log[4].find("draw(") != std::string::npos);
  EXPECT_EQ("end_rendering()", log[5]);
}

/**
 * Multiple DRAW nodes reading distinct buffers all merge into one pre-barrier.
 */
TEST_P(VKRenderGraphTestRender, merge_multiple_buffers)
{
  VkHandle<VkBuffer> buffers[5] = {
      VkHandle<VkBuffer>(1u),
      VkHandle<VkBuffer>(2u),
      VkHandle<VkBuffer>(3u),
      VkHandle<VkBuffer>(4u),
      VkHandle<VkBuffer>(5u),
  };
  VkHandle<VkImage> image(6u);
  VkHandle<VkImageView> image_view(7u);

  for (int i = 0; i < 5; i++) {
    resources.add_buffer(buffers[i]);
  }
  resources.add_image(image, false);

  for (int i = 0; i < 5; i++) {
    render_graph->add_node(VKFillBufferNode::CreateInfo{buffers[i], 256, uint32_t(i)});
  }

  {
    VKResourceAccessInfo access_info = {};
    access_info.images.append(
        {image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, {}});
    VKBeginRenderingNode::CreateInfo begin_rendering(access_info);
    begin_rendering.node_data.color_attachments[0].sType =
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    begin_rendering.node_data.color_attachments[0].imageLayout = color_attachment_layout();
    begin_rendering.node_data.color_attachments[0].imageView = image_view;
    begin_rendering.node_data.color_attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    begin_rendering.node_data.color_attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    begin_rendering.node_data.vk_rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    begin_rendering.node_data.vk_rendering_info.colorAttachmentCount = 1;
    begin_rendering.node_data.vk_rendering_info.layerCount = 1;
    begin_rendering.node_data.vk_rendering_info.pColorAttachments =
        begin_rendering.node_data.color_attachments;
    render_graph->add_node(begin_rendering);
  }

  for (int i = 0; i < 5; i++) {
    VKResourceAccessInfo access_info = {};
    access_info.buffers.append({buffers[i], VK_ACCESS_SHADER_READ_BIT});
    VKDrawNode::CreateInfo draw(access_info);
    draw.node_data.vertex_count = 3;
    draw.node_data.instance_count = 1;
    render_graph->add_node(draw);
  }

  render_graph->add_node(VKEndRenderingNode::CreateInfo{});
  submit(render_graph, command_buffer);

  ASSERT_EQ(14, log.size());

  for (int i = 0; i < 5; i++) {
    /* fill_buffer(...) */
    EXPECT_TRUE(log[i].find("fill_buffer") != std::string::npos);
  }

  /* pipeline_barrier(src=TOP_OF_PIPE, dst=ALL_GRAPHICS, image_barrier for image=0x6) */
  EXPECT_TRUE(log[5].find("pipeline_barrier") != std::string::npos);
  EXPECT_TRUE(log[5].find("image=0x6") != std::string::npos);

  /* MERGED pipeline_barrier(src=TRANSFER, dst=ALL_GRAPHICS,
   *   buffer_barrier(buffer=0x1..0x5)) */
  int buffer_barrier_count = 0;
  size_t search_pos = log[6].find("buffer_barrier(");
  while (search_pos != std::string::npos) {
    buffer_barrier_count++;
    search_pos = log[6].find("buffer_barrier(", search_pos + 1);
  }
  EXPECT_EQ(5, buffer_barrier_count);

  for (int i = 0; i < 5; i++) {
    std::string expected = "buffer=0x" + std::to_string(i + 1);
    EXPECT_TRUE(log[6].find(expected) != std::string::npos);
  }

  /* begin_rendering(...) */
  EXPECT_TRUE(log[7].find("begin_rendering") != std::string::npos);
  for (int i = 0; i < 5; i++) {
    /* draw(...) */
    EXPECT_TRUE(log[8 + i].find("draw(") != std::string::npos);
  }
  EXPECT_EQ("end_rendering()", log[13]);
}

}  // namespace blender::gpu::render_graph
