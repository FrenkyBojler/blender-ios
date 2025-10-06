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

// GLSL code for the main compute logic.
// Note the buffer names match the GpuMeshComputeBinding definitions below.
static const char *SCATTER_SHADER_MAIN_GLSL = R"GLSL(
// GLSL utility functions (packing, normals, etc.)
int pack_i16_trunc(float x) {
  return clamp(int(round(x * 32767.0)), -32768, 32767);
}
uint pack_i16_pair(float a, float b) {
  return (uint(pack_i16_trunc(a)) & 0xFFFFu) | ((uint(pack_i16_trunc(b)) & 0xFFFFu) << 16);
}
int pack_i10_trunc(float x) {
  return clamp(int(x * 511.0), -512, 511) & 0x3FF;
}
uint pack_norm(vec3 n) {
  return uint(pack_i10_trunc(n.x)) | (uint(pack_i10_trunc(n.y)) << 10) | (uint(pack_i10_trunc(n.z)) << 20);
}

vec3 newell_face_normal_object(int f) {
  int beg = face_offsets(f);
  int end = face_offsets(f + 1);
  vec3 n = vec3(0.0);
  vec3 v_prev = skinned_positions_in[corner_verts(end - 1)].xyz;
  for (int i = beg; i < end; ++i) {
    vec3 v_curr = skinned_positions_in[corner_verts(i)].xyz;
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
  if (c >= positions_out.length()) {
    return;
  }

  int v = corner_verts(int(c));

  // 1) Scatter position
  vec4 p_obj = skinned_positions_in[v];
  positions_out[c] = transform_mat[0] * p_obj;

  // 2) Calculate and scatter normal
  vec3 n_obj;
  if (normals_domain == 1) { // Face normals
    int f = corner_to_face(int(c));
    n_obj = newell_face_normal_object(f);
  }
  else { // Vertex normals
    int beg = vert_to_face_offsets(v);
    int end = vert_to_face_offsets(v + 1);
    vec3 n_accum = vec3(0.0);
    for (int i = beg; i < end; ++i) {
      n_accum += newell_face_normal_object(vert_to_face(i));
    }
    n_obj = n_accum;
  }

  vec3 n_world = transform_normal(n_obj, transform_mat[0]);
  if (normals_hq == 0) {
    normals_out[c] = pack_norm(n_world);
  }
  else {
    int base = int(c) * 2;
    normals_out[base + 0] = pack_i16_pair(n_world.x, n_world.y);
    normals_out[base + 1] = pack_i16_pair(n_world.z, 0.0);
  }
}
)GLSL";

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
    "   Optional argument `transform` must be a gpu.types.GPUStorageBuf containing\n"
    "   a mat4");

static PyObject *pygpu_mesh_scatter(PyObject * /*self*/, PyObject *args, PyObject *kwds)
{
  using namespace blender::draw;
  using namespace blender::bke;

  PyObject *py_obj = nullptr;
  BPyGPUStorageBuf *py_ssbo_skinned_pos = nullptr;
  BPyGPUStorageBuf *py_ssbo_transform = nullptr;

  static const char *keywords[] = {"obj", "ssbo", "transform", nullptr};
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "OO|O:scatter_positions_to_corners",
                                   (char **)keywords,
                                   &py_obj,
                                   &py_ssbo_skinned_pos,
                                   &py_ssbo_transform))
  {
    return nullptr;
  }

  /* --- 1. Validate Inputs --- */
  if (!GPU_context_active_get()) {
    PyErr_SetString(PyExc_RuntimeError, "No active GPU context");
    return nullptr;
  }
  if (!py_ssbo_skinned_pos || !py_ssbo_skinned_pos->ssbo) {
    PyErr_SetString(PyExc_TypeError, "Expected a GPUStorageBuf as second argument (positions SSBO)");
    return nullptr;
  }

  ID *id_obj = nullptr;
  if (!pyrna_id_FromPyObject(py_obj, &id_obj) || GS(id_obj->name) != ID_OB) {
    PyErr_Format(PyExc_TypeError, "Expected an Object, not %.200s", Py_TYPE(py_obj)->tp_name);
    return nullptr;
  }

  Object *ob_eval = reinterpret_cast<Object *>(id_obj);
  if (!DEG_is_evaluated(ob_eval) || ob_eval->type != OB_MESH) {
    PyErr_SetString(PyExc_TypeError, "Expected an evaluated mesh object");
    return nullptr;
  }

  Depsgraph *depsgraph = DEG_get_depsgraph_by_id(ob_eval->id);
  if (!depsgraph) {
    PyErr_SetString(PyExc_RuntimeError, "Object is not owned by a depsgraph");
    return nullptr;
  }

  Mesh *mesh_eval = static_cast<Mesh *>(ob_eval->data);
  if (!mesh_eval || !mesh_eval->runtime || !mesh_eval->runtime->batch_cache) {
    /* Not an error, just not ready. Request a redraw and tell Python to try again later. */
    Object *ob_orig = DEG_get_original(ob_eval);
    if (ob_orig) {
      DEG_id_tag_update(&ob_orig->id, ID_RECALC_GEOMETRY);
      WM_main_add_notifier(NC_WINDOW, nullptr);
    }
    Py_RETURN_NONE;
  }

  /* --- 2. Prepare GPU resources and bindings --- */
  auto *cache = static_cast<MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  auto *vbo_pos = cache->final.buff.vbos.lookup_ptr(VBOType::Position)->get();
  auto *vbo_nor = cache->final.buff.vbos.lookup_ptr(VBOType::CornerNormal)->get();

  /* Transform SSBO: optional. If not provided, create an identity SSBO and mark it
   * as owned by this function (we will free it unless compute is deferred). */
  blender::gpu::StorageBuf *transform_ssbo = nullptr;
  bool transform_owned = false;

  if (py_ssbo_transform == nullptr) {
    transform_ssbo = GPU_storagebuf_create(sizeof(float) * 16);
    float m[4][4];
    unit_m4(m);
    GPU_storagebuf_update(transform_ssbo, &m[0][0]);
    transform_owned = true;
  }
  else {
    if (!py_ssbo_transform->ssbo) {
      PyErr_SetString(PyExc_TypeError, "transform SSBO is invalid");
      return nullptr;
    }
  }

  using namespace blender::gpu::shader;

  std::vector<GpuMeshComputeBinding> bindings = {
      {0, vbo_pos, Qualifier::write, "vec4", "positions_out[]"},
      {1, vbo_nor, Qualifier::write, "uint", "normals_out[]"},
      {2, py_ssbo_skinned_pos->ssbo, Qualifier::read, "vec4", "skinned_positions_in[]"},
      {3,
       transform_owned ? transform_ssbo : py_ssbo_transform->ssbo,
       Qualifier::read,
       "mat4",
       "transform_mat[]"},
  };

  Scene *scene = DEG_get_input_scene(depsgraph);
  const int normals_domain_val = (mesh_eval->normals_domain() == MeshNormalDomain::Face) ? 1 : 0;
  const int normals_hq_val = int(bool(scene->r.perf_flag & SCE_PERF_HQ_NORMALS) ||
                                 GPU_use_hq_normals_workaround());

  auto config_shader = [&](blender::gpu::shader::ShaderCreateInfo &info) {
    info.specialization_constant(
        blender::gpu::shader::Type::int_t, "normals_domain", normals_domain_val);
    info.specialization_constant(blender::gpu::shader::Type::int_t, "normals_hq", normals_hq_val);
  };

  /* --- 3. Run Compute Shader via High-Level API --- */
  GpuComputeStatus status = BKE_mesh_gpu_run_compute(depsgraph,
                                                     ob_eval,
                                                     SCATTER_SHADER_MAIN_GLSL,
                                                     bindings,
                                                     config_shader,
                                                     mesh_eval->corner_verts().size());

  if (status == GpuComputeStatus::Error) {
    PyErr_SetString(PyExc_RuntimeError, "Failed to run mesh compute shader");
    return nullptr;
  }

  /* Ready: free locally-created SSBO. */
  if (transform_owned && transform_ssbo) {
    GPU_storagebuf_free(transform_ssbo);
    transform_ssbo = nullptr;
  }
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
  BKE_mesh_gpu_free_for_mesh(mesh_orig);
  if (mesh_orig) {
    mesh_orig->is_using_gpu_deform = 0;
  }
  if (ob->data == mesh_orig) {
    static_cast<Mesh *>(ob->data)->is_running_gpu_deform = 0;
  }

  Py_RETURN_NONE;
}

void bpygpu_mesh_scatter_free_for_mesh(Mesh *me)
{
  /* Only frees shader + topology ssbo,
   * User has to free himself his ssbos (positions + transform) */
  BKE_mesh_gpu_free_for_mesh(me);
}

void bpygpu_mesh_scatter_shaders_free_all()
{
  BKE_mesh_gpu_free_all_caches();
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
