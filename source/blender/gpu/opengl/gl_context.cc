/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BLI_assert.h"
#include "BLI_utildefines.h"

#include "BKE_global.hh"

#include "GPU_framebuffer.hh"

#include "GHOST_C-api.h"

#include "gpu_context_private.hh"
#include "gpu_immediate_private.hh"

#include "gl_debug.hh"
#include "gl_immediate.hh"
#include "gl_state.hh"
#include "gl_uniform_buffer.hh"

#include "gl_backend.hh" /* TODO: remove. */
#include "gl_context.hh"

namespace blender {

using namespace blender::gpu;

/* -------------------------------------------------------------------- */
/** \name Constructor / Destructor
 * \{ */

GLContext::GLContext(void *ghost_window, GLSharedOrphanLists &shared_orphan_list)
    : shared_orphan_list_(shared_orphan_list), context_lost_(false),
      state_manager(nullptr), imm(nullptr),
      front_left(nullptr), back_left(nullptr), front_right(nullptr), back_right(nullptr),
      active_fb(nullptr), ghost_window_(nullptr),
      default_attr_vbo_(0), is_active_(false),
      bound_ubo_slots(0), bound_ssbo_slots(0)
{
  ghost_window_ = ghost_window;

  if (G.debug & G_DEBUG_GPU) {
    debug::init_gl_callbacks();
  }

  float data[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  glGenBuffers(1, &default_attr_vbo_);
  glBindBuffer(GL_ARRAY_BUFFER, default_attr_vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(data), data, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  state_manager = new GLStateManager();
  imm = new GLImmediate();

  if (ghost_window_) {
    GLuint default_fbo = GHOST_GetDefaultGPUFramebuffer(
        static_cast<GHOST_WindowHandle>(ghost_window_));
    if (default_fbo == 0) {
      default_fbo = 0; // fallback to system default
    }

    GHOST_RectangleHandle bounds = GHOST_GetClientBounds(
        static_cast<GHOST_WindowHandle>(ghost_window_));
    int w = 0, h = 0;
    if (bounds) {
      w = GHOST_GetWidthRectangle(bounds);
      h = GHOST_GetHeightRectangle(bounds);
      GHOST_DisposeRectangle(bounds);
    }

    front_left = new GLFrameBuffer("front_left", this,
                                    default_fbo != 0 ? GL_COLOR_ATTACHMENT0 : GL_FRONT_LEFT,
                                    default_fbo, w, h);
    back_left = new GLFrameBuffer("back_left", this,
                                   default_fbo != 0 ? GL_COLOR_ATTACHMENT0 : GL_BACK_LEFT,
                                   default_fbo, w, h);

    GLboolean supports_stereo_quad_buffer = GL_FALSE;
    glGetBooleanv(GL_STEREO, &supports_stereo_quad_buffer);
    if (supports_stereo_quad_buffer) {
      front_right = new GLFrameBuffer("front_right", this, GL_FRONT_RIGHT, 0, w, h);
      back_right = new GLFrameBuffer("back_right", this, GL_BACK_RIGHT, 0, w, h);
    }
  }
  else {
    back_left = new GLFrameBuffer("back_left", this, GL_NONE, 0, 0, 0);
  }

  active_fb = back_left;
  if (state_manager && active_fb) {
    static_cast<GLStateManager *>(state_manager)->active_fb =
        static_cast<GLFrameBuffer *>(active_fb);
  }
}

GLContext::~GLContext()
{
  if (G.profile_gpu) {
    finish();
    process_frame_timings();
  }

  free_resources();

  BLI_assert(orphaned_framebuffers_.is_empty());
  BLI_assert(orphaned_vertarrays_.is_empty());
  BLI_assert(framebuffers_.is_empty());

  for (GLVaoCache *cache : vao_caches_) {
    if (cache) cache->clear();
  }

  if (default_attr_vbo_ != 0) {
    glDeleteBuffers(1, &default_attr_vbo_);
    default_attr_vbo_ = 0;
  }

  delete imm;
  delete state_manager;
  delete front_left;
  delete back_left;
  delete front_right;
  delete back_right;

  imm = nullptr;
  state_manager = nullptr;
  front_left = back_left = front_right = back_right = nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Activate / Deactivate context
 * \{ */

void GLContext::activate()
{
  if (is_active_) return;

  GLint major = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);

  if (major == 0) {
    context_lost_ = true;
  }

  if (context_lost_) {
    free_resources();

    {
      std::scoped_lock lock(lists_mutex_);
      orphaned_framebuffers_.clear();
      orphaned_vertarrays_.clear();
      vao_caches_.clear();
    }

    shared_orphan_list_.buffers.clear(glDeleteBuffers);
    shared_orphan_list_.textures.clear(glDeleteTextures);
    shared_orphan_list_.programs.clear([](GLuint n, GLuint *ids) {
      for (uint i = 0; i < n; i++) {
        glDeleteProgram(ids[i]);
      }
    });

    float data[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    glGenBuffers(1, &default_attr_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, default_attr_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(data), data, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    context_lost_ = false;
  }

  is_active_ = true;
  thread_ = pthread_self();

  orphans_clear();

  if (ghost_window_) {
    GHOST_RectangleHandle bounds =
        GHOST_GetClientBounds(static_cast<GHOST_WindowHandle>(ghost_window_));
    int w = 0, h = 0;
    if (bounds) {
      w = GHOST_GetWidthRectangle(bounds);
      h = GHOST_GetHeightRectangle(bounds);
      GHOST_DisposeRectangle(bounds);
    }

    if (front_left) front_left->size_set(w, h);
    if (back_left) back_left->size_set(w, h);
    if (front_right) front_right->size_set(w, h);
    if (back_right) back_right->size_set(w, h);
  }

  bound_ubo_slots = 0;
  bound_ssbo_slots = 0;

  immActivate();
}

void GLContext::deactivate()
{
  immDeactivate();
  is_active_ = false;
}

void GLContext::begin_frame() { }
void GLContext::end_frame() { process_frame_timings(); }

/** \} */

/* -------------------------------------------------------------------- */
/** \name Flush, Finish & sync
 * \{ */

void GLContext::flush() { glFlush(); }
void GLContext::finish() { glFinish(); }

/** \} */

/* -------------------------------------------------------------------- */
/** \name Safe object deletion
 * \{ */

void GLSharedOrphanLists::OrphanList::clear(FunctionRef<void(GLuint, GLuint *)> free_fn)
{
  std::scoped_lock lock(mutex_);
  if (!handles_.is_empty()) {
    free_fn(uint(handles_.size()), handles_.data());
    handles_.clear();
  }
};

void GLSharedOrphanLists::OrphanList::append(GLuint handle)
{
  std::scoped_lock lock(mutex_);
  handles_.append(handle);
};

void GLSharedOrphanLists::orphans_clear()
{
  GLContext *ctx = GLContext::get();
  if (!ctx) return;

  buffers.clear(glDeleteBuffers);
  textures.clear(glDeleteTextures);
  shaders.clear([](GLuint size, GLuint *handles) {
    for (uint i = 0; i < size; i++) glDeleteShader(handles[i]);
  });
  programs.clear([](GLuint size, GLuint *handles) {
    for (uint i = 0; i < size; i++) glDeleteProgram(handles[i]);
  });
};

void GLContext::orphans_clear()
{
  if (!this || !is_active_on_thread()) return;

  std::scoped_lock lock(lists_mutex_);

  if (!orphaned_vertarrays_.is_empty()) {
    glDeleteVertexArrays(uint(orphaned_vertarrays_.size()), orphaned_vertarrays_.data());
    orphaned_vertarrays_.clear();
  }
  if (!orphaned_framebuffers_.is_empty()) {
    glDeleteFramebuffers(uint(orphaned_framebuffers_.size()), orphaned_framebuffers_.data());
    orphaned_framebuffers_.clear();
  }

  shared_orphan_list_.orphans_clear();

  if (context_lost_) {
    orphaned_vertarrays_.clear();
    orphaned_framebuffers_.clear();
  }
}

void GLContext::orphans_add(Vector<GLuint> &orphan_list, std::mutex &list_mutex, GLuint id)
{
  std::scoped_lock lock(list_mutex);
  orphan_list.append(id);
}

void GLContext::vao_free(GLuint vao_id)
{
  GLContext *ctx = GLContext::get();
  if (ctx && this == ctx) glDeleteVertexArrays(1, &vao_id);
  else orphans_add(orphaned_vertarrays_, lists_mutex_, vao_id);
}

void GLContext::fbo_free(GLuint fbo_id)
{
  GLContext *ctx = GLContext::get();
  if (ctx && this == ctx) glDeleteFramebuffers(1, &fbo_id);
  else orphans_add(orphaned_framebuffers_, lists_mutex_, fbo_id);
}

void GLContext::buffer_free(GLuint buf_id)
{
  GLContext *ctx = GLContext::get();
  if (ctx) glDeleteBuffers(1, &buf_id);
  else GLBackend::get()->shared_orphan_list_get().buffers.append(buf_id);
}

void GLContext::texture_free(GLuint tex_id)
{
  GLContext *ctx = GLContext::get();
  if (ctx) glDeleteTextures(1, &tex_id);
  else GLBackend::get()->shared_orphan_list_get().textures.append(tex_id);
}

void GLContext::shader_free(GLuint shader_id)
{
  GLContext *ctx = GLContext::get();
  if (ctx) glDeleteShader(shader_id);
  else GLBackend::get()->shared_orphan_list_get().shaders.append(shader_id);
}

void GLContext::program_free(GLuint program_id)
{
  GLContext *ctx = GLContext::get();
  if (ctx) glDeleteProgram(program_id);
  else GLBackend::get()->shared_orphan_list_get().programs.append(program_id);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Linked object deletion
 * \{ */

void GLContext::vao_cache_register(GLVaoCache *cache)
{
  if (!cache) return;
  std::scoped_lock lock(lists_mutex_);
  vao_caches_.add(cache);
}

void GLContext::vao_cache_unregister(GLVaoCache *cache)
{
  if (!cache) return;
  std::scoped_lock lock(lists_mutex_);
  vao_caches_.remove(cache);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Memory statistics
 * \{ */

void GLContext::memory_statistics_get(int *r_total_mem, int *r_free_mem)
{
  if (!r_total_mem || !r_free_mem) return;

  if (epoxy_has_gl_extension("GL_NVX_gpu_memory_info")) {
    glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, r_total_mem);
    glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, r_free_mem);
  }
  else if (epoxy_has_gl_extension("GL_ATI_meminfo")) {
    int stats[4];
    glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, stats);
    *r_total_mem = 0;
    *r_free_mem = stats[0];
  }
  else {
    *r_total_mem = 0;
    *r_free_mem = 0;
  }
}

/** \} */

}  // namespace blender
