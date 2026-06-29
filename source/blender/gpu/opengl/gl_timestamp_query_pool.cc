/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <BLI_assert.hh>

#include "gl_timestamp_query_pool.hh"

namespace blender::gpu {

GLTimestampQueryPool::GLTimestampQueryPool(unsigned int num_queries_max)
    : num_queries_max_(num_queries_max)
{
  query_ids_ = new GLuint[num_queries_max_];
  glGenQueries(num_queries_max, query_ids_);
}

GLTimestampQueryPool::~GLTimestampQueryPool()
{
  glDeleteQueries(num_queries_max_, query_ids_);
  delete[] query_ids_;
}

void GLTimestampQueryPool::reset() {}

void GLTimestampQueryPool::write_timestamp(unsigned int index)
{
  glQueryCounter(query_ids_[index], GL_TIMESTAMP);
}

void GLTimestampQueryPool::read_timestamps_sync(unsigned int index_first,
                                                unsigned int num_queries,
                                                uint64_t *timestamps_device)
{
  for (unsigned int offset = 0; offset < num_queries; offset++) {
    /* It shouldn't be necessary to do busy waiting for
     * glGetQueryObjectiv(query_ids_[index_first + offset], GL_QUERY_RESULT_AVAILABLE, &available).
     */
    glGetQueryObjectui64v(
        query_ids_[index_first + offset], GL_QUERY_RESULT, timestamps_device + offset);
  }
}

uint64_t GLTimestampQueryPool::convert_timestamp_device_to_host_domain(uint64_t timestamp_device)
{
  return timestamp_device;
}

uint64_t GLTimestampQueryPool::get_host_timestamp_now()
{
  GLint64 cpu_time;
  glGetInteger64v(GL_TIMESTAMP, &cpu_time);
  return cpu_time;
}

}  // namespace blender::gpu
