/* SPDX-FileCopyrightText: 2021-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include <Eigen/Core>
#include <Eigen/Geometry>

#include "GHOST_Types.hh"
#include "GHOST_XrException.hh"
#include "GHOST_Xr_intern.hh"

#include "GHOST_XrControllerModel.hh"

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "tiny_gltf.h"

struct GHOST_XrControllerModelNode {
  int32_t parent_idx = -1;
  int32_t component_idx = -1;
  float local_transform[4][4];
};

/* -------------------------------------------------------------------- */
/** \name glTF Utilities
 *
 * Adapted from Microsoft OpenXR-Mixed Reality Samples (MIT License):
 * https://github.com/microsoft/OpenXR-MixedReality
 * \{ */

struct GHOST_XrPrimitive {
  std::vector<GHOST_XrControllerModelVertex> vertices;
  std::vector<uint32_t> indices;
};

/**
 * Validate that an accessor does not go out of bounds of the buffer view that it references and
 * that the buffer view does not exceed the bounds of the buffer that it references
 */
static void validate_accessor(const tinygltf::Accessor &accessor,
                              const tinygltf::BufferView &buffer_view,
                              const tinygltf::Buffer &buffer,
                              size_t byte_stride,
                              size_t element_size)
{
  /* Make sure the accessor does not go out of range of the buffer view. */
  if (accessor.byteOffset + (accessor.count - 1) * byte_stride + element_size >
      buffer_view.byteLength)
  {
    throw GHOST_XrException("glTF: Accessor goes out of range of bufferview.");
  }

  /* Make sure the buffer view does not go out of range of the buffer. */
  if (buffer_view.byteOffset + buffer_view.byteLength > buffer.data.size()) {
    throw GHOST_XrException("glTF: BufferView goes out of range of buffer.");
  }
}

template<float (GHOST_XrControllerModelVertex::*field)[3]>
static void read_vertices_vec3(const tinygltf::Accessor &accessor,
                               const tinygltf::BufferView &buffer_view,
                               const tinygltf::Buffer &buffer,
                               GHOST_XrPrimitive &primitive)
{
  if (accessor.type != TINYGLTF_TYPE_VEC3) {
    throw GHOST_XrException(
        "glTF: Accessor for primitive attribute has incorrect type (VEC3 expected).");
  }

  if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
    throw GHOST_XrException(
        "glTF: Accessor for primitive attribute has incorrect component type (FLOAT expected).");
  }

  /* If stride is not specified, it is tightly packed. */
  constexpr size_t packed_size = sizeof(float) * 3;
  const size_t stride = buffer_view.byteStride == 0 ? packed_size : buffer_view.byteStride;
  validate_accessor(accessor, buffer_view, buffer, stride, packed_size);

  /* Resize the vertices vector, if necessary, to include room for the attribute data.
   * If there are multiple attributes for a primitive, the first one will resize, and the
   * subsequent will not need to. */
  primitive.vertices.resize(accessor.count);

  /* Copy the attribute value over from the glTF buffer into the appropriate vertex field. */
  const uint8_t *buffer_ptr = buffer.data.data() + buffer_view.byteOffset + accessor.byteOffset;
  for (size_t i = 0; i < accessor.count; i++, buffer_ptr += stride) {
    memcpy(primitive.vertices[i].*field, buffer_ptr, packed_size);  // TODO: was stride
  }
}

template<float (GHOST_XrControllerModelVertex::*field)[2]>
static void read_vertices_vec2(const tinygltf::Accessor &accessor,
                               const tinygltf::BufferView &buffer_view,
                               const tinygltf::Buffer &buffer,
                               GHOST_XrPrimitive &primitive)
{
  if (accessor.type != TINYGLTF_TYPE_VEC2) {
    throw GHOST_XrException(
        "glTF: Accessor for primitive attribute has incorrect type (VEC2 expected).");
  }

  if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
    throw GHOST_XrException(
        "glTF: Accessor for primitive attribute has incorrect component type (FLOAT expected).");
  }

  constexpr size_t packed_size = sizeof(float) * 2;
  const size_t stride = buffer_view.byteStride == 0 ? packed_size : buffer_view.byteStride;
  validate_accessor(accessor, buffer_view, buffer, stride, packed_size);

  primitive.vertices.resize(accessor.count);

  const uint8_t *buffer_ptr = buffer.data.data() + buffer_view.byteOffset + accessor.byteOffset;
  for (size_t i = 0; i < accessor.count; i++, buffer_ptr += stride) {
    memcpy(primitive.vertices[i].*field, buffer_ptr, packed_size);
    /* glTF uses top-left UV origin (V=0 at top), Blender uses bottom-left (V=0 at bottom). */
    (primitive.vertices[i].*field)[1] = 1.0f - (primitive.vertices[i].*field)[1];
  }
}

static void load_attribute_accessor(const tinygltf::Model &gltf_model,
                                    const std::string &attribute_name,
                                    int accessor_id,
                                    GHOST_XrPrimitive &primitive)
{
  const auto &accessor = gltf_model.accessors.at(accessor_id);

  if (accessor.bufferView == -1) {
    throw GHOST_XrException("glTF: Accessor for primitive attribute specifies no bufferview.");
  }

  const tinygltf::BufferView &buffer_view = gltf_model.bufferViews.at(accessor.bufferView);
  if (buffer_view.target != TINYGLTF_TARGET_ARRAY_BUFFER && buffer_view.target != 0) {
    throw GHOST_XrException(
        "glTF: Accessor for primitive attribute uses bufferview with invalid 'target' type.");
  }

  const tinygltf::Buffer &buffer = gltf_model.buffers.at(buffer_view.buffer);

  if (attribute_name == "POSITION") {
    read_vertices_vec3<&GHOST_XrControllerModelVertex::position>(
        accessor, buffer_view, buffer, primitive);
  }
  else if (attribute_name == "NORMAL") {
    read_vertices_vec3<&GHOST_XrControllerModelVertex::normal>(
        accessor, buffer_view, buffer, primitive);
  }
  else if (attribute_name == "TEXCOORD_0") {
    read_vertices_vec2<&GHOST_XrControllerModelVertex::uv>(
        accessor, buffer_view, buffer, primitive);
  }
}

/**
 * Reads index data from a glTF primitive into a GHOST_XrPrimitive. glTF indices may be 8bit, 16bit
 * or 32bit integers. This will coalesce indices from the source type(s) into a 32bit integer.
 */
template<typename TSrcIndex>
static void read_indices(const tinygltf::Accessor &accessor,
                         const tinygltf::BufferView &buffer_view,
                         const tinygltf::Buffer &buffer,
                         GHOST_XrPrimitive &primitive)
{

  /* Allow 0 (not specified) even though spec doesn't seem to allow this (BoomBox GLB fails). */
  if (buffer_view.target != TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER && buffer_view.target != 0) {
    throw GHOST_XrException(
        "glTF: Accessor for indices uses bufferview with invalid 'target' type.");
  }

  constexpr size_t component_size_bytes = sizeof(TSrcIndex);
  /* Index buffer must be packed per glTF spec. */
  if (buffer_view.byteStride != 0 && buffer_view.byteStride != component_size_bytes) {
    throw GHOST_XrException(
        "glTF: Accessor for indices uses bufferview with invalid 'byteStride'.");
  }

  validate_accessor(accessor, buffer_view, buffer, component_size_bytes, component_size_bytes);

  /* Since only triangles are supported, enforce that the number of indices is divisible by 3. */
  if ((accessor.count % 3) != 0) {
    throw GHOST_XrException("glTF: Unexpected number of indices for triangle primitive");
  }

  const TSrcIndex *index_buffer = reinterpret_cast<const TSrcIndex *>(
      buffer.data.data() + buffer_view.byteOffset + accessor.byteOffset);
  for (uint32_t i = 0; i < accessor.count; i++) {
    primitive.indices.push_back(*(index_buffer + i));
  }
}

/**
 * Reads index data from a glTF primitive into a GHOST_XrPrimitive.
 */
static void load_index_accessor(const tinygltf::Model &gltf_model,
                                const tinygltf::Accessor &accessor,
                                GHOST_XrPrimitive &primitive)
{
  if (accessor.type != TINYGLTF_TYPE_SCALAR) {
    throw GHOST_XrException("glTF: Accessor for indices specifies invalid 'type'.");
  }

  if (accessor.bufferView == -1) {
    throw GHOST_XrException("glTF: Index accessor without bufferView is currently not supported.");
  }

  const tinygltf::BufferView &buffer_view = gltf_model.bufferViews.at(accessor.bufferView);
  const tinygltf::Buffer &buffer = gltf_model.buffers.at(buffer_view.buffer);

  if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
    read_indices<uint8_t>(accessor, buffer_view, buffer, primitive);
  }
  else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
    read_indices<uint16_t>(accessor, buffer_view, buffer, primitive);
  }
  else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
    read_indices<uint32_t>(accessor, buffer_view, buffer, primitive);
  }
  else {
    throw GHOST_XrException("glTF: Accessor for indices specifies invalid 'componentType'.");
  }
}

static GHOST_XrPrimitive read_primitive(const tinygltf::Model &gltf_model,
                                        const tinygltf::Primitive &gltf_primitive)
{
  if (gltf_primitive.mode != TINYGLTF_MODE_TRIANGLES) {
    throw GHOST_XrException(
        "glTF: Unsupported primitive mode. Only TINYGLTF_MODE_TRIANGLES is supported.");
  }

  GHOST_XrPrimitive primitive;

  /* glTF vertex data is stored in an attribute dictionary.Loop through each attribute and insert
   * it into the GHOST_XrPrimitive. */
  for (const auto &[attr_name, accessor_idx] : gltf_primitive.attributes) {
    load_attribute_accessor(gltf_model, attr_name, accessor_idx, primitive);
  }

  if (gltf_primitive.indices != -1) {
    /* If indices are specified for the glTF primitive, read them into the GHOST_XrPrimitive. */
    load_index_accessor(gltf_model, gltf_model.accessors.at(gltf_primitive.indices), primitive);
  }

  return primitive;
}

/**
 * Calculate node local and world transforms.
 */
static void calc_node_transforms(const tinygltf::Node &gltf_node,
                                 const float parent_transform[4][4],
                                 float r_local_transform[4][4],
                                 float r_world_transform[4][4])
{
  /* A node may specify either a 4x4 matrix or TRS (Translation - Rotation - Scale) values, but not
   * both. */
  if (gltf_node.matrix.size() == 16) {
    const std::vector<double> &dm = gltf_node.matrix;
    float m[4][4] = {{float(dm[0]), float(dm[1]), float(dm[2]), float(dm[3])},
                     {float(dm[4]), float(dm[5]), float(dm[6]), float(dm[7])},
                     {float(dm[8]), float(dm[9]), float(dm[10]), float(dm[11])},
                     {float(dm[12]), float(dm[13]), float(dm[14]), float(dm[15])}};
    memcpy(r_local_transform, m, sizeof(float[4][4]));
  }
  else {
    /* No matrix is present, so construct a matrix from the TRS values (each one is optional). */
    std::vector<double> translation = gltf_node.translation;
    std::vector<double> rotation = gltf_node.rotation;
    std::vector<double> scale = gltf_node.scale;
    Eigen::Matrix4f &m = *(Eigen::Matrix4f *)r_local_transform;
    Eigen::Quaternionf q;
    Eigen::Matrix3f scalemat;

    if (translation.size() != 3) {
      translation.resize(3);
      translation[0] = translation[1] = translation[2] = 0.0;
    }
    if (rotation.size() != 4) {
      rotation.resize(4);
      rotation[0] = rotation[1] = rotation[2] = 0.0;
      rotation[3] = 1.0;
    }
    if (scale.size() != 3) {
      scale.resize(3);
      scale[0] = scale[1] = scale[2] = 1.0;
    }

    q.w() = float(rotation[3]);
    q.x() = float(rotation[0]);
    q.y() = float(rotation[1]);
    q.z() = float(rotation[2]);
    q.normalize();

    scalemat.setIdentity();
    scalemat(0, 0) = float(scale[0]);
    scalemat(1, 1) = float(scale[1]);
    scalemat(2, 2) = float(scale[2]);

    m.setIdentity();
    m.block<3, 3>(0, 0) = q.toRotationMatrix() * scalemat;
    m.block<3, 1>(0, 3) = Eigen::Vector3f(
        float(translation[0]), float(translation[1]), float(translation[2]));
  }

  *(Eigen::Matrix4f *)r_world_transform = *(Eigen::Matrix4f *)parent_transform *
                                          *(Eigen::Matrix4f *)r_local_transform;
}

static void load_node(const tinygltf::Model &gltf_model,
                      int gltf_node_id,
                      int32_t parent_idx,
                      const float parent_transform[4][4],
                      const std::string &parent_name,
                      const std::vector<XrRenderModelAssetNodePropertiesEXT> &node_properties,
                      const std::vector<int32_t> &material_to_texture,
                      std::vector<GHOST_XrControllerModelVertex> &vertices,
                      std::vector<uint32_t> &indices,
                      std::vector<GHOST_XrControllerModelComponent> &components,
                      std::vector<GHOST_XrControllerModelNode> &nodes,
                      std::vector<int32_t> &node_state_indices,
                      int32_t component_offset)
{
  const tinygltf::Node &gltf_node = gltf_model.nodes.at(gltf_node_id);
  float world_transform[4][4];

  GHOST_XrControllerModelNode &node = nodes.emplace_back();
  const int32_t node_idx = int32_t(nodes.size() - 1);
  node.parent_idx = parent_idx;
  calc_node_transforms(gltf_node, parent_transform, node.local_transform, world_transform);

  /* Match by uniqueName only (EXT difference from MSFT). */
  for (size_t i = 0; i < node_properties.size(); ++i) {
    if ((node_state_indices[i] < 0) && (gltf_node.name == node_properties[i].uniqueName)) {
      node_state_indices[i] = node_idx;
      break;
    }
  }

  /* Load mesh data if present. */
  if (gltf_node.mesh != -1) {
    const tinygltf::Mesh &gltf_mesh = gltf_model.meshes.at(gltf_node.mesh);

    GHOST_XrControllerModelComponent &component = components.emplace_back();
    /* Store local index relative to this model's component offset. */
    node.component_idx = int32_t(components.size() - 1 - component_offset);
    memcpy(component.transform, world_transform, sizeof(component.transform));
    component.vertex_offset = vertices.size();
    component.index_offset = indices.size();
    component.texture_index = -1;

    for (const tinygltf::Primitive &gltf_primitive : gltf_mesh.primitives) {
      /* Get texture index from material (use first primitive's material). */
      const bool valid_material_idx = (gltf_primitive.material >= 0 &&
                                       gltf_primitive.material < material_to_texture.size());
      if (component.texture_index == -1 && valid_material_idx) {
        component.texture_index = material_to_texture[gltf_primitive.material];
      }

      const GHOST_XrPrimitive primitive = read_primitive(gltf_model, gltf_primitive);

      const size_t start_vertex = vertices.size();
      size_t offset = start_vertex;
      size_t count = primitive.vertices.size();
      vertices.resize(offset + count);
      memcpy(vertices.data() + offset,
             primitive.vertices.data(),
             count * sizeof(decltype(primitive.vertices)::value_type));

      offset = indices.size();
      count = primitive.indices.size();
      indices.resize(offset + count);
      for (size_t i = 0; i < count; i += 3) {
        indices[offset + i + 0] = start_vertex + primitive.indices[i + 0];
        indices[offset + i + 1] = start_vertex + primitive.indices[i + 2];
        indices[offset + i + 2] = start_vertex + primitive.indices[i + 1];
      }
    }

    component.vertex_count = vertices.size() - component.vertex_offset;
    component.index_count = indices.size() - component.index_offset;
  }

  /* Recursively load children. */
  for (const int child_node_id : gltf_node.children) {
    load_node(gltf_model,
              child_node_id,
              node_idx,
              world_transform,
              gltf_node.name,
              node_properties,
              material_to_texture,
              vertices,
              indices,
              components,
              nodes,
              node_state_indices,
              component_offset);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name OpenXR Extension Functions
 *
 * \{ */

static XrInstance g_instance = XR_NULL_HANDLE;

/* Multi-vendor interaction model extension function pointers. */
static PFN_xrEnumerateInteractionRenderModelIdsEXT g_xrEnumerateInteractionRenderModelIdsEXT =
    nullptr;

/* Multi-vendor render model extension function pointers. */
static PFN_xrCreateRenderModelEXT g_xrCreateRenderModelEXT = nullptr;
static PFN_xrDestroyRenderModelEXT g_xrDestroyRenderModelEXT = nullptr;
static PFN_xrGetRenderModelPropertiesEXT g_xrGetRenderModelPropertiesEXT = nullptr;
static PFN_xrCreateRenderModelSpaceEXT g_xrCreateRenderModelSpaceEXT = nullptr;
static PFN_xrCreateRenderModelAssetEXT g_xrCreateRenderModelAssetEXT = nullptr;
static PFN_xrDestroyRenderModelAssetEXT g_xrDestroyRenderModelAssetEXT = nullptr;
static PFN_xrGetRenderModelAssetDataEXT g_xrGetRenderModelAssetDataEXT = nullptr;
static PFN_xrGetRenderModelAssetPropertiesEXT g_xrGetRenderModelAssetPropertiesEXT = nullptr;
static PFN_xrGetRenderModelStateEXT g_xrGetRenderModelStateEXT = nullptr;
static PFN_xrGetRenderModelPoseTopLevelUserPathEXT g_xrGetRenderModelPoseTopLevelUserPathEXT =
    nullptr;

static void init_controller_model_extension_functions_multi_vendor(XrInstance instance)
{
  if (instance != g_instance) {
    g_instance = instance;

    g_xrEnumerateInteractionRenderModelIdsEXT = nullptr;

    g_xrCreateRenderModelEXT = nullptr;
    g_xrDestroyRenderModelEXT = nullptr;
    g_xrGetRenderModelPropertiesEXT = nullptr;
    g_xrCreateRenderModelSpaceEXT = nullptr;
    g_xrCreateRenderModelAssetEXT = nullptr;
    g_xrDestroyRenderModelAssetEXT = nullptr;
    g_xrGetRenderModelAssetDataEXT = nullptr;
    g_xrGetRenderModelAssetPropertiesEXT = nullptr;
    g_xrGetRenderModelStateEXT = nullptr;
    g_xrGetRenderModelPoseTopLevelUserPathEXT = nullptr;
  }

  if (g_xrEnumerateInteractionRenderModelIdsEXT == nullptr) {
    INIT_EXTENSION_FUNCTION(xrEnumerateInteractionRenderModelIdsEXT);
  }

  if (g_xrCreateRenderModelEXT == nullptr) {
    INIT_EXTENSION_FUNCTION(xrCreateRenderModelEXT);
  }
  if (g_xrDestroyRenderModelEXT == nullptr) {
    INIT_EXTENSION_FUNCTION(xrDestroyRenderModelEXT);
  }
  if (g_xrGetRenderModelPropertiesEXT == nullptr) {
    INIT_EXTENSION_FUNCTION(xrGetRenderModelPropertiesEXT);
  }
  if (g_xrCreateRenderModelSpaceEXT == nullptr) {
    INIT_EXTENSION_FUNCTION(xrCreateRenderModelSpaceEXT);
  }
  if (g_xrCreateRenderModelAssetEXT == nullptr) {
    INIT_EXTENSION_FUNCTION(xrCreateRenderModelAssetEXT);
  }
  if (g_xrDestroyRenderModelAssetEXT == nullptr) {
    INIT_EXTENSION_FUNCTION(xrDestroyRenderModelAssetEXT);
  }
  if (g_xrGetRenderModelAssetDataEXT == nullptr) {
    INIT_EXTENSION_FUNCTION(xrGetRenderModelAssetDataEXT);
  }
  if (g_xrGetRenderModelAssetPropertiesEXT == nullptr) {
    INIT_EXTENSION_FUNCTION(xrGetRenderModelAssetPropertiesEXT);
  }
  if (g_xrGetRenderModelStateEXT == nullptr) {
    INIT_EXTENSION_FUNCTION(xrGetRenderModelStateEXT);
  }
  if (g_xrGetRenderModelPoseTopLevelUserPathEXT == nullptr) {
    INIT_EXTENSION_FUNCTION(xrGetRenderModelPoseTopLevelUserPathEXT);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GHOST_XrControllerModel
 *
 * \{ */

GHOST_XrControllerModel::GHOST_XrControllerModel(XrInstance instance,
                                                 XrSpace reference_space,
                                                 const char *subaction_path_str)
{
  init_controller_model_extension_functions_multi_vendor(instance);
  reference_space_ = reference_space;

  CHECK_XR(xrStringToPath(instance, subaction_path_str, &subaction_path_),
           (std::string("Failed to get user path \"") + subaction_path_str + "\".").data());

  /* Build top level paths used for filtering. */
  XrPath left_hand_path;
  XrPath right_hand_path;

  CHECK_XR(xrStringToPath(instance, "/user/hand/left", &left_hand_path),
           "Failed to get left hand user path");
  CHECK_XR(xrStringToPath(instance, "/user/hand/right", &right_hand_path),
           "Failed to get right hand user path");

  toplevel_paths_ = {left_hand_path, right_hand_path};
}

GHOST_XrControllerModel::~GHOST_XrControllerModel()
{
  /* Wait for async loading if in progress. */
  if (load_task_.valid()) {
    load_task_.wait();
  }

  /* Destroy render model handles. */
  for (XrRenderModelEXT render_model : interaction_models_) {
    if (render_model != XR_NULL_HANDLE) {
      g_xrDestroyRenderModelEXT(render_model);
    }
  }

  /* Destroy model spaces. */
  for (XrSpace model_space : model_spaces_) {
    if (model_space != XR_NULL_HANDLE) {
      xrDestroySpace(model_space);
    }
  }
}

void GHOST_XrControllerModel::load(XrSession session)
{
  if (data_loaded_ || load_task_.valid()) {
    return;
  }

  /* Enumerate the render model IDs. */
  uint32_t num_models = 0;
  CHECK_XR(g_xrEnumerateInteractionRenderModelIdsEXT(session, nullptr, 0, &num_models, nullptr),
           "Failed to obtain interaction render model count.");

  if (num_models == 0) {
    /* No render model to load. */
    return;
  }

  std::vector<XrRenderModelIdEXT> interaction_model_ids{XR_NULL_PATH, num_models};
  CHECK_XR(g_xrEnumerateInteractionRenderModelIdsEXT(
               session, nullptr, num_models, &num_models, interaction_model_ids.data()),
           "Failed to iterate interaction render models.");

  // Create render model handles
  // TODO: The names of glTF extensions that the application is capable of supporting.
  // The returned glTF model may have any or all of these extensions listed in
  // the "extensionsRequired" array.
  // Pass only the extensions that your app/engine are capable of supporting.
  std::vector<const char *> appSupportedGltfExtensions{"KHR_texture_basisu",
                                                       "KHR_materials_specular"};

  /* Create render model handles, filtering by subaction path. */
  for (XrRenderModelIdEXT id : interaction_model_ids) {
    XrRenderModelEXT render_model;
    XrRenderModelCreateInfoEXT render_model_create_info = {XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT};

    render_model_create_info.renderModelId = id;

    render_model_create_info.gltfExtensionCount = (uint32_t)appSupportedGltfExtensions.size();
    render_model_create_info.gltfExtensions = appSupportedGltfExtensions.data();

    CHECK_XR(g_xrCreateRenderModelEXT(session, &render_model_create_info, &render_model),
             "Failed to create interaction render model handle.");

    // TODO: This is kind of abusing the API as we should instead load all model and choose which one to return on getData, but oh well.
    /* Check if this model matches our target subaction path. */
    XrInteractionRenderModelTopLevelUserPathGetInfoEXT path_info = {
        XR_TYPE_INTERACTION_RENDER_MODEL_TOP_LEVEL_USER_PATH_GET_INFO_EXT};
    path_info.next = nullptr;
    path_info.topLevelUserPathCount = toplevel_paths_.size();
    path_info.topLevelUserPaths = toplevel_paths_.data();

    XrPath top_level_path = XR_NULL_PATH;
    CHECK_XR(g_xrGetRenderModelPoseTopLevelUserPathEXT(render_model, &path_info, &top_level_path),
             "Failed to get render model top level user path.");

    if (top_level_path != subaction_path_) {
      /* This model is for a different hand/user path, destroy it and skip. */
      g_xrDestroyRenderModelEXT(render_model);
      continue;
    }

    interaction_models_.push_back(render_model);
  }

  for (XrRenderModelEXT render_model : interaction_models_) {
    /* Create a space for locating the render model. */
    XrRenderModelSpaceCreateInfoEXT space_create_info = {
        XR_TYPE_RENDER_MODEL_SPACE_CREATE_INFO_EXT};
    space_create_info.renderModel = render_model;
    XrSpace model_space;
    CHECK_XR(g_xrCreateRenderModelSpaceEXT(session, &space_create_info, &model_space),
             "Failed to create interaction render model space.");
    model_spaces_.push_back(model_space);

    /* Get the model properties: UUID and number of animatable nodes */
    XrRenderModelPropertiesGetInfoEXT properties_get_info = {
        XR_TYPE_RENDER_MODEL_PROPERTIES_GET_INFO_EXT};
    XrRenderModelPropertiesEXT properties = {XR_TYPE_RENDER_MODEL_PROPERTIES_EXT};
    CHECK_XR(g_xrGetRenderModelPropertiesEXT(render_model, &properties_get_info, &properties),
             "Failed to obtain interaction render model properties.");

    model_properties_.push_back(properties);

    // TODO: Sub-scopre / function from there on? Async?
    /* Create the asset handle to request the data. */
    XrRenderModelAssetCreateInfoEXT asset_create_info = {
        XR_TYPE_RENDER_MODEL_ASSET_CREATE_INFO_EXT};
    asset_create_info.cacheId = properties.cacheId;
    XrRenderModelAssetEXT asset;
    CHECK_XR(g_xrCreateRenderModelAssetEXT(session, &asset_create_info, &asset),
             "Failed to create interaction render model asset handle.");

    /* Copy the binary glTF (GLB) asset data using two-call idiom. */
    XrRenderModelAssetDataGetInfoEXT asset_get_info = {
        XR_TYPE_RENDER_MODEL_ASSET_DATA_GET_INFO_EXT};
    XrRenderModelAssetDataEXT asset_data = {XR_TYPE_RENDER_MODEL_ASSET_DATA_EXT};
    CHECK_XR(g_xrGetRenderModelAssetDataEXT(asset, &asset_get_info, &asset_data),
             "Failed to obtain interaction render model glTF data size.");
    std::vector<uint8_t> model_data(asset_data.bufferCountOutput);
    asset_data.bufferCapacityInput = (uint32_t)model_data.size();
    asset_data.buffer = model_data.data();
    CHECK_XR(g_xrGetRenderModelAssetDataEXT(asset, &asset_get_info, &asset_data),
             "Failed to obtain interaction render model glTF data buffer.");

    /* Parse glTF using tinygltf. */
    tinygltf::TinyGLTF gltf_loader;
    tinygltf::Model gltf_model;
    std::string err_msg;

    /* Use custom image loader to store raw encoded data instead of decoding.
     * The actual decoding happens later using ImBuf in #wm_xr_controller_model_textures_create. */
    auto load_img_func = [](tinygltf::Image *image,
                            const int,
                            std::string *,
                            std::string *,
                            int,
                            int,
                            const uchar *bytes,
                            int size,
                            void *) -> bool {
      /* Directly store bytes into the tinyGltf image. */
      image->image.assign(bytes, bytes + size);
      image->as_is = true;

      return true;
    };
    gltf_loader.SetImageLoader(load_img_func, nullptr);

    if (!gltf_loader.LoadBinaryFromMemory(
            &gltf_model, &err_msg, nullptr, model_data.data(), model_data.size()))
    {
      throw GHOST_XrException(("Failed to load glTF controller model: " + err_msg).c_str());
    }

    /* Get the unique names of the animatable nodes. */
    XrRenderModelAssetPropertiesGetInfoEXT asset_properties_get_info = {
        XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_GET_INFO_EXT};
    XrRenderModelAssetPropertiesEXT asset_properties = {XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_EXT};
    std::vector<XrRenderModelAssetNodePropertiesEXT> node_properties(
        properties.animatableNodeCount);
    asset_properties.nodePropertyCount = (uint32_t)node_properties.size();
    asset_properties.nodeProperties = node_properties.data();
    CHECK_XR(
        g_xrGetRenderModelAssetPropertiesEXT(asset, &asset_properties_get_info, &asset_properties),
        "Failed to obtain interaction render model asset properties.");

    /* Once the glTF data has been handled we no longer need the XrRenderModelAssetEXT handle. */
    CHECK_XR(g_xrDestroyRenderModelAssetEXT(asset),
             "Failed to destroy interaction render model asset hanndle.");

    /* Extract raw texture data from glTF images. */
    const int32_t existing_textures_offset = int32_t(textures_.size());
    for (const tinygltf::Image &image : gltf_model.images) {
      textures_.push_back(image.image);
    }

    /* Build glTF material to texture index mapping. */
    std::vector<int32_t> material_to_texture(gltf_model.materials.size(), -1);
    for (size_t i = 0; i < gltf_model.materials.size(); ++i) {
      const tinygltf::Material &mat = gltf_model.materials[i];

      int tex_index = mat.pbrMetallicRoughness.baseColorTexture.index;
      int image_index = gltf_model.textures[tex_index].source;

      material_to_texture[i] = existing_textures_offset + image_index;
    }

    /* Setup per-model tracking. */
    PerModelData per_model;
    per_model.node_properties = std::move(node_properties);
    per_model.node_offset = nodes_.size();
    per_model.component_offset = components_.size();
    per_model.node_state_indices.resize(per_model.node_properties.size(), -1);

    if (!gltf_model.scenes.empty()) {
      const tinygltf::Scene &default_scene = gltf_model.scenes.at(
          (gltf_model.defaultScene == -1) ? 0 : gltf_model.defaultScene);

      float root_transform[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

      for (const int node_id : default_scene.nodes) {
        load_node(gltf_model,
                  node_id,
                  -1, /* Root has no parent. */
                  root_transform,
                  "", /* Root has no parent name. */
                  per_model.node_properties,
                  material_to_texture,
                  vertices_,
                  indices_,
                  components_,
                  nodes_,
                  per_model.node_state_indices,
                  per_model.component_offset);
      }
    }

    per_model_data_.push_back(std::move(per_model));
  }

  data_loaded_ = true;
}

void GHOST_XrControllerModel::updateComponents(XrSession /*session*/, XrTime display_time)
{
  if (!data_loaded_) {
    return;
  }

  for (size_t model_idx = 0; model_idx < interaction_models_.size(); ++model_idx) {
    XrRenderModelEXT render_model = interaction_models_[model_idx];
    const XrRenderModelPropertiesEXT &model_properties = model_properties_[model_idx];
    XrSpace model_space = model_spaces_[model_idx];
    const PerModelData &per_model = per_model_data_[model_idx];

    /* Locate the model's space. */
    XrSpaceLocation model_location = {XR_TYPE_SPACE_LOCATION};
    CHECK_XR(xrLocateSpace(model_space, reference_space_, display_time, &model_location),
             "Failed to locate interaction render model space.");

    const bool tracked_orientation = (model_location.locationFlags &
                                      XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
    const bool tracked_position = (model_location.locationFlags &
                                   XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;

    if (!tracked_orientation || !tracked_position) {
      /* Only render if the model space is tracked, and if the session state is appropriate,
       * if applicable. (e.g. interaction models are only to be rendered when FOCUSED) */
      continue;
    }

    /* Store the model's base pose (use first tracked model). */
    if (model_idx == 0) {
      const XrPosef &pose = model_location.pose;
      base_pose_.is_active = true;
      base_pose_.position[0] = pose.position.x;
      base_pose_.position[1] = pose.position.y;
      base_pose_.position[2] = pose.position.z;

      base_pose_.orientation_quat[0] = pose.orientation.w;
      base_pose_.orientation_quat[1] = pose.orientation.x;
      base_pose_.orientation_quat[2] = pose.orientation.y;
      base_pose_.orientation_quat[3] = pose.orientation.z;
    }

    XrRenderModelStateGetInfoEXT model_state_get_info = {XR_TYPE_RENDER_MODEL_STATE_GET_INFO_EXT};
    model_state_get_info.displayTime = display_time;

    std::vector<XrRenderModelNodeStateEXT> model_node_states(model_properties.animatableNodeCount);

    XrRenderModelStateEXT model_state = {XR_TYPE_RENDER_MODEL_STATE_EXT};
    model_state.nodeStateCount = (uint32_t)model_node_states.size();
    model_state.nodeStates = model_node_states.data();
    CHECK_XR(g_xrGetRenderModelStateEXT(render_model, &model_state_get_info, &model_state),
             "Failed to obtain interaction render model state.");

    /* Update node local transforms from runtime. */
    for (size_t state_idx = 0; state_idx < model_node_states.size(); ++state_idx) {
      const int32_t node_idx = per_model.node_state_indices[state_idx];
      if (node_idx >= 0) {
        const XrRenderModelNodeStateEXT &node_state = model_node_states[state_idx];
        const XrPosef &pose = node_state.nodePose;

        /* node_state_indices stores global indices into nodes_. */
        GHOST_XrControllerModelNode &node = nodes_[node_idx];
        Eigen::Matrix4f &m = *(Eigen::Matrix4f *)node.local_transform;
        Eigen::Quaternionf q(
            pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);
        m.setIdentity();
        m.block<3, 3>(0, 0) = q.toRotationMatrix();
        m.block<3, 1>(0, 3) = Eigen::Vector3f(pose.position.x, pose.position.y, pose.position.z);
      }
    }

    /* Calculate component world transforms. */
    const int32_t node_start = per_model.node_offset;
    const int32_t node_end = (model_idx + 1 < per_model_data_.size()) ?
                                 per_model_data_[model_idx + 1].node_offset :
                                 nodes_.size();

    std::vector<Eigen::Matrix4f> world_transforms(node_end - node_start);

    for (int32_t i = node_start; i < node_end; ++i) {
      const GHOST_XrControllerModelNode &node = nodes_[i];
      const int32_t local_idx = i - node_start;

      if (node.parent_idx >= 0) {
        const int32_t parent_local_idx = node.parent_idx - node_start;
        world_transforms[local_idx] = world_transforms[parent_local_idx] *
                                      *(Eigen::Matrix4f *)node.local_transform;
      }
      else {
        world_transforms[local_idx] = *(Eigen::Matrix4f *)node.local_transform;
      }

      /* Update component transform if this node has one. */
      if (node.component_idx >= 0) {
        const int32_t comp_idx = per_model.component_offset + node.component_idx;
        memcpy(components_[comp_idx].transform,
               world_transforms[local_idx].data(),
               sizeof(components_[comp_idx].transform));
      }
    }
  }
}

void GHOST_XrControllerModel::getData(GHOST_XrControllerModelData &r_data)
{
  if (data_loaded_) {
    r_data.vertices = vertices_;
    r_data.indices = indices_;
    r_data.components = components_;
    r_data.textures = textures_;
    r_data.base_pose = base_pose_;
  }
  else {
    r_data.vertices.clear();
    r_data.indices.clear();
    r_data.components.clear();
    r_data.textures.clear();
    r_data.base_pose = {false, {0, 0, 0}, {1, 0, 0, 0}};
  }
}

/** \} */
