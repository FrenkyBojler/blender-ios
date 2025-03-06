/* SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "GPU_vertex_buffer.hh"

namespace blender::opensubdiv {

/**
 * GLVertexBuffer compatible API wrapped around a blender::gpu::VertBuf
 *
 * The blender::gpu::VertBuf is owned by the wrapper.
 * Vertex buffer is used as its API is able to wrap around SSBOs as well.
 */
class GPUVertexBuffer {
  gpu::VertBuf &gpu_vertex_buffer_;

 public:
  GPUVertexBuffer(gpu::VertBuf &gpu_vertex_buffer) : gpu_vertex_buffer_(gpu_vertex_buffer) {}

  /**
   * Create a new gpu::VertBuf wrapped in a GPUVertexBuffer.
   *
   * @param element_count: Number of elements per vertex
   * @param vertex_len: Number of vertices
   * @param device_context: Unused.
   */

  static GPUVertexBuffer *Create(int element_count, int vertex_len, void *device_context = nullptr)
  {
    (void)device_context;
    GPUVertFormat format;
    GPU_vertformat_clear(&format);
    GPU_vertformat_attr_add(&format, "elements", GPU_COMP_F32, element_count, GPU_FETCH_FLOAT);
    gpu::VertBuf *vertex_buffer = GPU_vertbuf_create_with_format_ex(format, GPU_USAGE_STATIC);
    GPU_vertbuf_init_build_on_device(*vertex_buffer, format, vertex_len);
    return new GPUVertexBuffer(*vertex_buffer);
  }

  /// Destructor.
  ~GPUVertexBuffer()
  {
    GPU_vertbuf_discard(&gpu_vertex_buffer_);
  }

  /// This method is meant to be used in client code in order to provide coarse
  /// vertices data to Osd.
  void UpdateData(const float *src,
                  int start_vertex,
                  int num_vertices,
                  void *device_context = NULL)
  {
    (void)device_context;
    GPU_vertbuf_use(&gpu_vertex_buffer_);
    GPU_vertbuf_update_sub(&gpu_vertex_buffer_, start_vertex, num_vertices, src);
  }
  /*

  /// Returns how many elements defined in this vertex buffer.
  int GetNumElements() const {
      GPU_
  }

  */
  /// Returns how many vertices allocated in this vertex buffer.
  int GetNumVertices() const
  {
    return GPU_vertbuf_get_vertex_len(&gpu_vertex_buffer_);
  }

  /// Returns the GL buffer object.
  GLuint BindVBO(void *device_context = NULL)
  {
    (void)device_context;
    return 0;
  }

  gpu::VertBuf *get_vertex_buffer()
  {
    return &gpu_vertex_buffer_;
  }
};

}  // namespace blender::opensubdiv
