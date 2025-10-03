/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Minimal wrapper to run the "scatter positions -> corners + normals" compute shader
 * from Python.
 */

#include <Python.h>
#include <mutex>
#include <unordered_map>

#include "gpu_py_mesh_scatter.hh"

#include "BKE_idtype.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"
#include "BKE_scene.hh"

#include "BLI_math_matrix.h"
#include "BLI_string_utf8.h"

#include "DNA_ID.h"
#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"

#include "../draw/intern/draw_cache_extract.hh"
#include "../gpu/intern/gpu_shader_create_info.hh"

#include "GPU_capabilities.hh"
#include "GPU_compute.hh"
#include "GPU_context.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_vertex_buffer.hh"

#include "../depsgraph/DEG_depsgraph_query.hh"

#include "../windowmanager/WM_api.hh"

#include "../mathutils/mathutils.hh"

#include "../generic/python_compat.hh" /* IWYU pragma: keep. */
#include "../intern/bpy_rna.hh"        /* pyrna_id_FromPyObject */
#include "gpu_py.hh"
#include "gpu_py_storagebuffer.hh"
#include "gpu_py_vertex_buffer.hh"

struct MeshScatterResources {
  blender::gpu::Shader *shader = nullptr;
  blender::bke::MeshGPUTopology topology;
  blender::gpu::StorageBuf *ssbo_transform_mat = nullptr;
  int normals_domain = 0;
  int normals_hq = 0;
};

/* Rollback guard for MeshScatter operations.
 *
 * Purpose:
 * - Centralizes cleanup of GPU resources and mesh state when a fatal error occurs
 *   after the mesh state has been mutated or GPU resources have been allocated.
 *
 * Usage:
 * - Construct the guard early: `MeshScatterRollback rb(mesh_orig, ob_orig);`
 * - Activate rollback only once irreversible work or allocations have been done:
 *     `rb.enable_rollback();`
 * - On a fatal error after activation, call:
 *     `return rb.fail_with_cleanup("message");`
 *   This sets the Python exception, arms the rollback and returns `nullptr`.
 * - For transient "retry" paths (e.g. `Py_RETURN_NONE` when cache is not ready),
 *   do not enable the rollback -- these paths should not free or reset state.
 * - On success call `rb.commit()` to disarm the rollback and avoid cleanup.
 *
 * Destructor (when and how it runs):
 * - The destructor is invoked automatically when the local `rb` object goes out of scope.
 *   This includes:
 *     - Normal function exit (end of `pygpu_mesh_scatter`),
 *     - Any early `return` from the function (including returns from `rb.fail_with_cleanup()`),
 *     - Stack unwinding during a C++ exception.
 * - The destructor performs cleanup only if the guard is armed (`enabled == true`).
 *   In that case it:
 *     - Resets `mesh_eval->is_running_gpu_deform` to 0 to avoid leaving the evaluated mesh marked,
 *     - Calls `bpygpu_mesh_scatter_free_for_mesh(mesh_orig)` to free or orphan GPU resources
 *       in a GPU-context-safe way,
 *     - Tags the object for geometry update and requests a redraw:
 *         `DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);`
 *         `WM_main_add_notifier(NC_WINDOW, nullptr);`
 * - If the guard is not armed (either `enable_rollback()` was never called, or `commit()` was
 *   called), the destructor is a no-op.
 *
 * Rationale:
 * - The RAII pattern avoids duplicated cleanup code and guarantees atomic rollback
 *   of mesh state on errors. Deferred freeing (orphaning) is handled by
 *   `bpygpu_mesh_scatter_free_for_mesh` to remain safe with respect to GPU context.
 */
struct MeshScatterRollback {
  Mesh *mesh_orig;
  Mesh *mesh_eval;
  Object *ob;
  bool enabled = false;

  MeshScatterRollback(Mesh *m_orig, Mesh *m_eval, Object *o)
      : mesh_orig(m_orig), mesh_eval(m_eval), ob(o)
  {
  }

  void enable_rollback()
  {
    enabled = true;
  }

  void commit()
  {
    /* Disarm rollback and drop references so destructor no-ops. */
    enabled = false;
    mesh_orig = nullptr;
    mesh_eval = nullptr;
    ob = nullptr;
  }

  /* Fail helper: set Python error, arm rollback and return nullptr. */
  PyObject *fail_with_cleanup(const char *msg)
  {
    if (msg) {
      PyErr_SetString(PyExc_RuntimeError, msg);
    }
    /* Arm the rollback so the destructor will cleanup. */
    enabled = true;
    return nullptr;
  }

  ~MeshScatterRollback()
  {
    if (!enabled) {
      return;
    }

    /* Ensure evaluated mesh flag is reset so we don't leave it marked as running. */
    if (mesh_eval) {
      mesh_eval->is_running_gpu_deform = 0;
    }

    /* Free or orphan GPU resources associated with the original mesh. */
    if (mesh_orig) {
      bpygpu_mesh_scatter_free_for_mesh(mesh_orig);
    }

    /* Request geometry rebuild/redraw for the object so the system recovers. */
    if (ob) {
      DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
      WM_main_add_notifier(NC_WINDOW, nullptr);
    }
  }
};

/* Orphans to free later when GPU context is available. */
static std::vector<MeshScatterResources> g_mesh_scatter_orphans;
static std::unordered_map<const Mesh *, MeshScatterResources> g_mesh_scatter_resources;
static std::mutex g_mesh_scatter_resources_mutex;

/* Free resources for a single mesh (safe to call from other translation units).
 * If no GPU context is active the resource is moved to a pending orphan list
 * and freed later (module cleanup or when a context becomes available). */
void bpygpu_mesh_scatter_free_for_mesh(Mesh *mesh)
{
  if (mesh == nullptr) {
    return;
  }

  mesh->is_using_gpu_deform = 0;

  {
    std::lock_guard<std::mutex> lock(g_mesh_scatter_resources_mutex);
    auto it = g_mesh_scatter_resources.find(mesh);
    if (it == g_mesh_scatter_resources.end()) {
      return;
    }

    MeshScatterResources res = std::move(it->second);
    g_mesh_scatter_resources.erase(it);

    if (GPU_context_active_get()) {
      if (res.shader) {
        GPU_shader_free(res.shader);
      }
      if (res.ssbo_transform_mat) {
        GPU_storagebuf_free(res.ssbo_transform_mat);
      }
      BKE_mesh_gpu_topology_free(res.topology);
    }
    else {
      res.topology.data.clear();
      res.topology.total_size = 0;
      g_mesh_scatter_orphans.push_back(std::move(res));
    }
  }
}

/* Create (or reuse) scatter resources for a specific mesh:
 * - builds and uploads the packed topology SSBO
 * - creates the specialized compute shader with specialization constants set to those offsets
 *
 * Returns nullptr on failure (e.g. no GPU context). */
static MeshScatterResources *mesh_scatter_resources_get_or_create(Mesh *mesh, Scene *scene)
{
  if (!mesh) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(g_mesh_scatter_resources_mutex);

  auto it = g_mesh_scatter_resources.find(mesh);
  if (it != g_mesh_scatter_resources.end()) {
    /* Existing resources found -> return pointer to stored value. */
    return &it->second;
  }

  /* Must have a GPU context to create SSBOs / shaders. */
  if (!GPU_context_active_get()) {
    return nullptr;
  }

  MeshScatterResources res;

  if (!BKE_mesh_gpu_topology_create(mesh, res.topology)) {
    return nullptr;
  }

  if (!BKE_mesh_gpu_topology_upload(res.topology)) {
    return nullptr;
  }

  /* Create and upload transform matrix SSBO (identity matrix). */
  if (!res.ssbo_transform_mat) {
    float transform_mat[4][4];
    unit_m4(transform_mat);
    res.ssbo_transform_mat = GPU_storagebuf_create(sizeof(float) * 16);
    GPU_storagebuf_update(res.ssbo_transform_mat, transform_mat);
  }

  res.normals_domain = mesh->normals_domain() == blender::bke::MeshNormalDomain::Face ? 1 : 0;
  res.normals_hq = int(bool(scene->r.perf_flag & SCE_PERF_HQ_NORMALS) || GPU_use_hq_normals_workaround());

  /* Create shader with specialization constants baked to mesh offsets. */
  using namespace blender::gpu::shader;
  const int group_size = 256;

  ShaderCreateInfo info("BGE_Armature_Scatter_Pass_Mesh");
  info.local_group_size(group_size, 1, 1);
  info.compute_source("draw_colormanagement_lib.glsl");
  info.storage_buf(0, Qualifier::write, "vec4", "positions[]");
  info.storage_buf(1, Qualifier::write, "uint", "normals[]");
  info.storage_buf(2, Qualifier::read, "vec4", "skinned_vert_positions[]");
  info.storage_buf(3, Qualifier::read, "mat4", "transform_mat[]");
  info.storage_buf(4, Qualifier::read, "int", "topo[]");

  BKE_mesh_gpu_topology_add_specialization_constants(info, res.topology);

  info.specialization_constant(Type::int_t, "normals_domain", res.normals_domain);
  info.specialization_constant(Type::int_t, "normals_hq", res.normals_hq);

  std::string glsl_accessors = BKE_mesh_gpu_topology_glsl_accessors_string(res.topology);

  info.compute_source_generated = glsl_accessors + R"GLSL(
// helpers SNORM16 packing
int pack_i16_trunc(float x) {
  const int max_i16 = 32767;
  const int min_i16 = -32768;
  float s = x * float(max_i16);
  int q = int(round(s));
  return clamp(q, min_i16, max_i16);
}
uint pack_i16_pair(float a, float b) {
  uint lo = uint(pack_i16_trunc(a)) & 0xFFFFu;
  uint hi = (uint(pack_i16_trunc(b)) & 0xFFFFu) << 16;
  return lo | hi;
}

// 10_10_10_2 packing utility
int pack_i10_trunc(float x) {
  const int signed_int_10_max = 511;
  const int signed_int_10_min = -512;
  float s = x * float(signed_int_10_max);
  int q = int(s);
  q = clamp(q, signed_int_10_min, signed_int_10_max);
  return q & 0x3FF;
}

uint pack_norm(vec3 n) {
  int nx = pack_i10_trunc(n.x);
  int ny = pack_i10_trunc(n.y);
  int nz = pack_i10_trunc(n.z);
  return uint(nx) | (uint(ny) << 10) | (uint(nz) << 20);
}

vec3 newell_face_normal_object(int f) {
  int beg = face_offsets(f);
  int end = face_offsets(f + 1);
  vec3 n = vec3(0.0);
  int v_prev_idx = corner_verts(end - 1);
  vec3 v_prev = skinned_vert_positions[v_prev_idx].xyz;
  for (int i = beg; i < end; ++i) {
    int v_curr_idx = corner_verts(i);
    vec3 v_curr = skinned_vert_positions[v_curr_idx].xyz;
    n += cross(v_prev, v_curr);
    v_prev = v_curr;
  }
  return normalize(n);
}

vec3 transform_normal(vec3 n, mat4 m) {
  return transpose(inverse(mat3(m))) * n;
}

void main() {
  uint c = gl_GlobalInvocationID.x;
  if (c >= positions.length()) {
    return;
  }

  int v = corner_verts(int(c));

  // 1) Scatter position
  vec4 p_obj = skinned_vert_positions[v];
  positions[c] = transform_mat[0] * p_obj;

  // 2) Calculate and scatter normal
  vec3 n_obj;
  if (normals_domain == 1) { // Face
    int f = corner_to_face(int(c));
    n_obj = newell_face_normal_object(f);
  }
  else { // Point
    int beg = vert_to_face_offsets(v);
    int end = vert_to_face_offsets(v + 1);
    vec3 n_accum = vec3(0.0);
    for (int i = beg; i < end; ++i) {
      int f = vert_to_face(i);
      n_accum += newell_face_normal_object(f);
    }
    n_obj = n_accum;
  }

  vec3 n_world = transform_normal(n_obj, transform_mat[0]);
  if (normals_hq == 0) {
    /* existing 10_10_10_2 packing, 1 uint per corner */
    normals[c] = pack_norm(n_world);
  }
  else {
    /* SNORM16x4 packing: write 2 uints per corner (x,y) then (z,0) */
    int base = int(c) * 2;
    normals[base + 0] = pack_i16_pair(n_world.x, n_world.y);
    normals[base + 1] = pack_i16_pair(n_world.z, 0.0);
  }
}
)GLSL";

  blender::gpu::Shader *shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
  if (!shader) {
    /* Creation failed: cleanup created ssbo if any. */
    if (res.ssbo_transform_mat) {
      GPU_storagebuf_free(res.ssbo_transform_mat);
      res.ssbo_transform_mat = nullptr;
    }
    return nullptr;
  }

  res.shader = shader;

  /* Insert into global map and return pointer to stored value. */
  auto inserted = g_mesh_scatter_resources.emplace(mesh, std::move(res));
  return &inserted.first->second;
}

/* Free all cached mesh scatter resources (shader + ssbo) */
static void mesh_scatter_resources_free_all()
{
  std::lock_guard<std::mutex> lock(g_mesh_scatter_resources_mutex);

  /* Free map entries */
  for (auto &kv : g_mesh_scatter_resources) {
    const Mesh *mesh_key = kv.first;
    if (mesh_key) {
      Mesh *mesh_mut = const_cast<Mesh *>(mesh_key);
      mesh_mut->is_using_gpu_deform = 0;
    }
    MeshScatterResources &r = kv.second;
    if (r.shader && GPU_context_active_get()) {
      GPU_shader_free(r.shader);
      r.shader = nullptr;
    }
    if (r.ssbo_transform_mat && GPU_context_active_get()) {
      GPU_storagebuf_free(r.ssbo_transform_mat);
      r.ssbo_transform_mat = nullptr;
    }
    BKE_mesh_gpu_topology_free(r.topology);
  }
  g_mesh_scatter_resources.clear();

  /* Free orphans */
  for (MeshScatterResources &r : g_mesh_scatter_orphans) {
    if (GPU_context_active_get()) {
      if (r.shader) {
        GPU_shader_free(r.shader);
      }
      if (r.ssbo_transform_mat) {
        GPU_storagebuf_free(r.ssbo_transform_mat);
      }
      if (r.topology.ssbo) {
        GPU_storagebuf_free(r.topology.ssbo);
        r.topology.ssbo = nullptr;
      }
    }
  }
  g_mesh_scatter_orphans.clear();
}

/* Expose C symbol for module cleanup (used from gpu module free). */
void bpygpu_mesh_scatter_shaders_free_all()
{
  mesh_scatter_resources_free_all();
}

static bool vbos_ready_for_scatter(Mesh *mesh_eval)
{
  if (!mesh_eval || !mesh_eval->runtime || !mesh_eval->runtime->batch_cache) {
    return false;
  }
  auto cache = static_cast<blender::draw::MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  if (!cache || cache->final.buff.vbos.size() == 0) {
    return false;
  }
  return true;
}

/* Returns stored MeshScatterResources pointer or nullptr on failure.
 * If expected_corners is provided it is filled with current corner count.
 */
static MeshScatterResources *ensure_resources(Mesh *mesh_eval, Scene *scene, int *expected_corners)
{
  MeshScatterResources *res = mesh_scatter_resources_get_or_create(mesh_eval, scene);
  if (!res) {
    return nullptr;
  }
  if (expected_corners) {
    *expected_corners = int(mesh_eval->corner_verts().size());
  }
  return res;
}

/* Result type for post MeshScatterResources creation checks.
 * If `error` != nullptr the check failed and callers should treat it as an error message.
 * On success `error` is nullptr and cache/vbo pointers are valid.
 */
struct ResourceCheckResult {
  const char *error;
  blender::draw::MeshBatchCache *cache;
  blender::gpu::VertBuf *vbo_pos;
  blender::gpu::VertBuf *vbo_nor;
};

static ResourceCheckResult check_post_resource_invariants(Object *ob_orig,
                                                          Mesh *mesh_orig,
                                                          Mesh *mesh_eval)
{
  using namespace blender::draw;

  ResourceCheckResult result = {nullptr, nullptr, nullptr, nullptr};

  if (!mesh_eval || !mesh_eval->runtime || !mesh_eval->runtime->batch_cache) {
    result.error = "Mesh batch cache not available after resource setup";
    return result;
  }
  if (ob_orig->type != OB_MESH) {
    result.error = "Object no longer owns a mesh";
    return result;
  }
  if (ob_orig->modifiers.first) {
    result.error = "Object gained modifiers while running";
    return result;
  }
  if (mesh_orig != static_cast<Mesh *>(ob_orig->data)) {
    result.error = "Mesh data was replaced while running";
    return result;
  }

  auto cache = static_cast<blender::draw::MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  if (!cache || cache->final.buff.vbos.size() == 0) {
    result.error = "Mesh batch cache VBOs not available after resource setup";
    return result;
  }

  /* Lookup VBOs */
  auto pos_it = cache->final.buff.vbos.lookup_ptr(VBOType::Position);
  auto nor_it = cache->final.buff.vbos.lookup_ptr(VBOType::CornerNormal);
  if (!pos_it || !nor_it) {
    result.error = "Required VBOs missing after resource setup";
    return result;
  }

  blender::gpu::VertBuf *pos_vbo = pos_it->get();
  blender::gpu::VertBuf *nor_vbo = nor_it->get();

  const GPUVertFormat *format2 = GPU_vertbuf_get_format(pos_vbo);
  int pos_id2 = GPU_vertformat_attr_id_get(format2, "pos");
  if (pos_id2 < 0) {
    result.error = "Position attribute missing in VBO";
    return result;
  }

  /* Success: fill result */
  result.cache = cache;
  result.vbo_pos = pos_vbo;
  result.vbo_nor = nor_vbo;
  return result;
}

/* Parse and upload the transform SSBO. On error this returns rb.fail_with_cleanup(...).
 * On success returns nullptr (no Python exception set).
 *
 * rb must be valid; if rollback semantics are desired rb.enable_rollback() should be called
 * before invoking this helper.
 */
static PyObject *parse_and_upload_transform(PyObject *py_transform,
                                            MeshScatterResources *res,
                                            MeshScatterRollback &rb)
{
  if (py_transform == nullptr || py_transform == Py_None) {
    return nullptr; /* nothing to do */
  }

  if (MatrixObject_Check(py_transform)) {
    MatrixObject *mat = (MatrixObject *)py_transform;
    if (BaseMath_ReadCallback((BaseMathObject *)mat) == -1) {
      return rb.fail_with_cleanup("Invalid mathutils.Matrix");
    }
    if ((mat->row_num != mat->col_num) || !ELEM(mat->row_num, 4)) {
      return rb.fail_with_cleanup("Expected 4x4 matrix");
    }
    if (!res->ssbo_transform_mat) {
      res->ssbo_transform_mat = GPU_storagebuf_create(sizeof(float) * 16);
      if (!res->ssbo_transform_mat) {
        return rb.fail_with_cleanup("Failed to create transform SSBO");
      }
    }
    GPU_storagebuf_update(res->ssbo_transform_mat, mat->matrix);
    return nullptr;
  }

  /* Fallback: flat sequence of 16 numbers (column-major) */
  if (!PySequence_Check(py_transform) || PySequence_Size(py_transform) != 16) {
    return rb.fail_with_cleanup(
        "transform must be a mathutils.Matrix or a sequence of 16 numbers");
  }

  float transform_mat[16];
  for (int i = 0; i < 16; i++) {
    PyObject *item = PySequence_GetItem(py_transform, i); /* new ref */
    if (!item) {
      return rb.fail_with_cleanup("Failed to read transform sequence");
    }
    double val = PyFloat_AsDouble(item);
    Py_DECREF(item);
    if (PyErr_Occurred()) {
      PyErr_Clear(); /* convert generic Python error to clearer message via fail_with_cleanup */
      return rb.fail_with_cleanup("transform elements must be numbers");
    }
    transform_mat[i] = (float)val;
  }

  if (!res->ssbo_transform_mat) {
    res->ssbo_transform_mat = GPU_storagebuf_create(sizeof(float) * 16);
    if (!res->ssbo_transform_mat) {
      return rb.fail_with_cleanup("Failed to create transform SSBO");
    }
  }
  GPU_storagebuf_update(res->ssbo_transform_mat, transform_mat);
  return nullptr;
}

PyDoc_STRVAR(
    pygpu_mesh_scatter_doc,
    ".. function:: scatter_positions_to_corners(obj, ssbo_positions, transform=None)\n"
    "\n"
    "   Scatter per-vertex positions (from user SSBO) to per-corner VBOs and recompute\n"
    "   packed normals using the internal compute shader. The mesh VBOs (positions and\n"
    "   normals) will be updated and ready for rendering.\n\n"
    "   NOTE: this function is non-blocking and may request the draw/cache system to\n"
    "   rebuild mesh VBOs asynchronously. If the evaluated mesh currently uses a\n"
    "   3-component vertex format but the draw/cache needs a 4-component (float4)\n"
    "   format, the function will tag the object for a geometry rebuild and return\n"
    "   immediately. The actual VBO population and scatter will then occur on the\n"
    "   next frame.\n\n"
    "   Because the operation can be deferred, callers that require the scatter to be\n"
    "   completed synchronously should re-invoke this function on a later frame (for\n"
    "   example using `bpy.app.timers.register` or from a modal operator) until the\n"
    "   VBOs are populated. This C API does not block or force the draw/cache to\n"
    "   populate VBOs synchronously.\n\n"
    "   `obj` must be an evaluated bpy.types.Object owning a mesh. `ssbo_positions`\n"
    "   must be a gpu.types.GPUStorageBuf containing vec4 per vertex.\n\n"
    "   Optional argument `transform` may be provided to apply a 4x4 transform when\n"
    "   scattering positions. Accepted values:\n"
    "     - a `mathutils.Matrix` (4x4) — the matrix is copied as-is from the mathutils\n"
    "       object (internal mathutils layout).\n"
    "     - a flat sequence of 16 floats — the sequence must be provided in column-major\n"
    "       order (suitable for GLSL `mat4`).\n\n"
    "   If you need a different memory layout, transpose the matrix in Python before\n"
    "   calling (for example: `[m[row][col] for col in range(4) for row in range(4)]`).\n");

static PyObject *pygpu_mesh_scatter(PyObject * /*self*/, PyObject *args, PyObject *kwds)
{
  using namespace blender::draw;

  PyObject *py_obj = nullptr;
  BPyGPUStorageBuf *py_ssbo = nullptr;
  PyObject *py_transform = nullptr;

  static const char *_keywords[] = {"obj", "ssbo", "transform", nullptr};
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "OO|O:scatter_positions_to_corners",
                                   (char **)_keywords,
                                   &py_obj,
                                   &py_ssbo,
                                   &py_transform))
  {
    return nullptr;
  }

  /* Validate GPU context */
  if (!GPU_context_active_get()) {
    PyErr_SetString(PyExc_RuntimeError, "No active GPU context");
    return nullptr;
  }

  /* Validate ssbo */
  if (!py_ssbo || py_ssbo->ssbo == nullptr) {
    PyErr_SetString(PyExc_TypeError, "Expected a GPUStorageBuf as second argument");
    return nullptr;
  }

  /* Convert Python object to Blender Object* */
  ID *id_obj = nullptr;
  if (!pyrna_id_FromPyObject(py_obj, &id_obj)) {
    PyErr_Format(PyExc_TypeError, "Expected an Object, not %.200s", Py_TYPE(py_obj)->tp_name);
    return nullptr;
  }
  if (GS(id_obj->name) != ID_OB) {
    PyErr_Format(PyExc_TypeError,
                 "Expected an Object, not %.200s",
                 BKE_idtype_idcode_to_name(GS(id_obj->name)));
    return nullptr;
  }

  Object *ob_eval = reinterpret_cast<Object *>(id_obj);
  if (!DEG_is_evaluated(ob_eval)) {
    PyErr_SetString(PyExc_TypeError, "Expected an evaluated object");
    return nullptr;
  }

  if (ob_eval->modifiers.first) {
    PyErr_SetString(PyExc_ValueError, "Objects with modifiers are not supported");
    return nullptr;
  }
  if (ob_eval->type != OB_MESH) {
    PyErr_SetString(PyExc_TypeError, "Object does not own a mesh");
    return nullptr;
  }

  Depsgraph *depsgraph = DEG_get_depsgraph_by_id(ob_eval->id);
  if (!depsgraph) {
    PyErr_SetString(PyExc_TypeError, "Object is not owned by a depsgraph");
    return nullptr;
  }

  Object *ob_orig = DEG_get_original(ob_eval);
  Mesh *mesh_orig = static_cast<Mesh *>(ob_orig->data);
  Mesh *mesh_eval = static_cast<Mesh *>(ob_eval->data);

  /* Reject running in other Mode than OB_MODE_OBJECT (retry) */
  if (ob_orig->mode != OB_MODE_OBJECT) {
    Py_RETURN_NONE;
  }

  /* Quick check for batch cache presence */
  if (!vbos_ready_for_scatter(mesh_eval)) {
    Py_RETURN_NONE; /* retry later */
  }

  blender::draw::MeshBatchCache *cache = static_cast<blender::draw::MeshBatchCache *>(
      mesh_eval->runtime->batch_cache);

  /* Lookup VBOs */
  blender::gpu::VertBuf *vbo_pos = nullptr;
  blender::gpu::VertBuf *vbo_nor = nullptr;
  {
    auto pos_it = cache->final.buff.vbos.lookup_ptr(VBOType::Position);
    auto nor_it = cache->final.buff.vbos.lookup_ptr(VBOType::CornerNormal);
    if (pos_it) {
      vbo_pos = pos_it->get();
    }
    if (nor_it) {
      vbo_nor = nor_it->get();
    }
    if (!vbo_pos || !vbo_nor) {
      PyErr_SetString(PyExc_RuntimeError, "Required VBOs not present in cache");
      return nullptr;
    }
  }

  /* Inspect vertex format and decide if we need rebuild (retry) */
  const GPUVertFormat *format = GPU_vertbuf_get_format(vbo_pos);
  int pos_id = GPU_vertformat_attr_id_get(format, "pos");
  const GPUVertAttr *attr = &format->attrs[pos_id];
  blender::gpu::VertAttrType type = attr->type.format;

  using blender::gpu::VertAttrType;
  if (type == VertAttrType::SFLOAT_32_32_32_32) {
    mesh_orig->is_using_gpu_deform = 0;
  }
  else if (type == VertAttrType::SFLOAT_32_32_32) {
    /* Request geometry rebuild (retry next frame) */
    mesh_orig->is_using_gpu_deform = 1;
  }

  /* If the object is already tagged for geometry rebuild and the VBO format
   * indicates the draw/cache is already to float4, free scatter resources and
   * retry later. Do this before arming the rollback or marking mesh_eval. */
  if ((ob_orig->id.recalc & ID_RECALC_GEOMETRY) != 0 &&
      type == blender::gpu::VertAttrType::SFLOAT_32_32_32_32)
  {
    bpygpu_mesh_scatter_free_for_mesh(mesh_orig);
    Py_RETURN_NONE;
  }

  if (mesh_orig->is_using_gpu_deform == 1) {
    DEG_id_tag_update(&ob_orig->id, ID_RECALC_GEOMETRY);
    BKE_scene_graph_update_tagged(depsgraph, DEG_get_bmain(depsgraph));
    WM_main_add_notifier(NC_WINDOW, nullptr);
    Py_RETURN_NONE;
  }

  /* Mark evaluated mesh as running GPU deform */
  mesh_eval->is_running_gpu_deform = 1;

  /* Create rollback guard now (it will reset is_running_gpu_deform and free resources on error).
   */
  MeshScatterRollback rb(mesh_orig, mesh_eval, ob_orig);
  /* Arm rollback immediately so any subsequent error triggers cleanup. */
  rb.enable_rollback();

  /* Create / get resources (may create SSBOs and shader) */
  Scene *scene = DEG_get_input_scene(depsgraph);

  MeshScatterResources *res = ensure_resources(mesh_eval, scene, nullptr);

  if (!res || !res->shader) {
    return rb.fail_with_cleanup("Scatter compute shader not available for mesh");
  }

  /* Check again if we need to free MeshScatterResources */
  auto inv = check_post_resource_invariants(ob_orig, mesh_orig, mesh_eval);
  if (inv.error) {
    return rb.fail_with_cleanup(inv.error);
  }
  cache = inv.cache;
  vbo_pos = inv.vbo_pos;
  vbo_nor = inv.vbo_nor;

  /* Parse & upload optional transform. On error this returns rb.fail_with_cleanup(...) */
  PyObject *parse_err = parse_and_upload_transform(py_transform, res, rb);
  if (parse_err) {
    return parse_err;
  }

  /* Bind shader with specialization constants state */
  const blender::gpu::shader::SpecializationConstants *constants_state =
      &GPU_shader_get_default_constant_state(res->shader);
  GPU_shader_bind(res->shader, constants_state);

  /* Bind destination VBOs as SSBO */
  vbo_pos->bind_as_ssbo(0);
  vbo_nor->bind_as_ssbo(1);

  /* Bind user SSBO (positions per vertex) */
  GPU_storagebuf_bind(py_ssbo->ssbo, 2);

  /* Bind transform_mat/topo */
  GPU_storagebuf_bind(res->ssbo_transform_mat, 3);
  GPU_storagebuf_bind(res->topology.ssbo, 4);

  /* Dispatch compute */
  const int num_corners = int(mesh_eval->corner_verts().size());
  const int group_size = 256;
  const int num_groups_corners = (num_corners + group_size - 1) / group_size;
  GPU_compute_dispatch(res->shader, num_groups_corners, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_VERTEX_ATTRIB_ARRAY);

  GPU_shader_unbind();

  /* Tag the object for transform to reset TAA samples... */
  DEG_id_tag_update(&ob_orig->id, ID_RECALC_TRANSFORM);

  rb.commit();
  Py_RETURN_NONE;
}

PyDoc_STRVAR(pygpu_mesh_scatter_free_doc,
             ".. function:: scatter_free_for_mesh(obj)\n"
             "\n"
             "   Free GPU resources (shader + SSBOs) associated with the mesh owned by `obj`.\n"
             "   Also resets `mesh.is_using_gpu_deform` and `mesh.is_running_gpu_deform` to 0.\n"
             "   `obj` may be an evaluated object or an original object (bpy.types.Object).\n");

static PyObject *pygpu_mesh_scatter_free(PyObject * /*self*/, PyObject *args, PyObject *kwds)
{
  PyObject *py_obj = nullptr;
  static const char *_keywords[] = {"obj", nullptr};
  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "O:scatter_free_for_mesh", (char **)_keywords, &py_obj))
  {
    return nullptr;
  }

  ID *id_obj = nullptr;
  if (!pyrna_id_FromPyObject(py_obj, &id_obj)) {
    PyErr_Format(PyExc_TypeError, "Expected an Object, not %.200s", Py_TYPE(py_obj)->tp_name);
    return nullptr;
  }

  if (GS(id_obj->name) != ID_OB) {
    PyErr_Format(PyExc_TypeError,
                 "Expected an Object, not %.200s",
                 BKE_idtype_idcode_to_name(GS(id_obj->name)));
    return nullptr;
  }

  Object *ob = reinterpret_cast<Object *>(id_obj);

  /* Accept evaluated or original object. If evaluated, find original. */
  Object *ob_orig = ob;
  if (DEG_is_evaluated(ob)) {
    /* DEG_get_original returns the original object for an evaluated one. */
    ob_orig = DEG_get_original(ob);
    if (ob_orig == nullptr) {
      PyErr_SetString(PyExc_RuntimeError, "Failed to find original object for evaluated object");
      return nullptr;
    }
  }

  if (ob_orig->type != OB_MESH) {
    PyErr_SetString(PyExc_TypeError, "Object does not own a mesh");
    return nullptr;
  }

  Mesh *mesh_orig = static_cast<Mesh *>(ob_orig->data);
  if (!mesh_orig) {
    PyErr_SetString(PyExc_RuntimeError, "Object mesh data not available");
    return nullptr;
  }

  /* Free GPU resources associated with this mesh (thread-safe internally). */
  bpygpu_mesh_scatter_free_for_mesh(mesh_orig);

  Py_RETURN_NONE;
}

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wcast-function-type"
#  else
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wcast-function-type"
#  endif
#endif

static PyMethodDef pygpu_mesh__tp_methods[] = {
    {"scatter_positions_to_corners",
     (PyCFunction)pygpu_mesh_scatter,
     METH_VARARGS | METH_KEYWORDS,
     pygpu_mesh_scatter_doc},
    {"scatter_free_for_mesh",
     (PyCFunction)pygpu_mesh_scatter_free,
     METH_VARARGS | METH_KEYWORDS,
     pygpu_mesh_scatter_free_doc},
    {nullptr, nullptr, 0, nullptr},
};

static PyModuleDef pygpu_mesh_module_def = {
    PyModuleDef_HEAD_INIT,
    "gpu.mesh",
    "Mesh related GPU helpers.",
    0,
    pygpu_mesh__tp_methods,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

#ifdef __GNUC__
#  ifdef __clang__
#    pragma clang diagnostic pop
#  else
#    pragma GCC diagnostic pop
#  endif
#endif

PyObject *bpygpu_mesh_init(void)
{
  PyObject *submodule = PyModule_Create(&pygpu_mesh_module_def);
  return submodule;
}
