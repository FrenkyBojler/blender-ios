/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cmpnodes
 */

#include <cstring>

#include "BLI_assert.h"
#include "BLI_cpp_type.hh"
#include "BLI_generic_pointer.hh"
#include "BLI_index_range.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "MEM_guardedalloc.h"

#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "BLO_read_write.hh"

#include "BKE_context.hh"
#include "BKE_cryptomatte.hh"
#include "BKE_image.hh"
#include "BKE_image_format.hh"
#include "BKE_main.hh"
#include "BKE_scene.hh"

#include "RNA_access.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"

#include "IMB_imbuf.hh"

#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "NOD_compositor_file_output.hh"
#include "NOD_socket_items_blend.hh"
#include "NOD_socket_items_ops.hh"
#include "NOD_socket_items_ui.hh"
#include "NOD_socket_search_link.hh"

#include "node_composite_util.hh"

namespace path_templates = blender::bke::path_templates;

namespace blender::nodes::node_composite_file_output_cc {

NODE_STORAGE_FUNCS(NodeCompositorFileOutput)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_default_layout();

  const bNodeTree *node_tree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  if (!node_tree || !node) {
    return;
  }

  const NodeCompositorFileOutput &storage = node_storage(*node);

  /* Inputs for multi-layer files need to be the same size, while they can be different for
   * individual file outputs. */
  const bool is_multi_layer = storage.format.imtype == R_IMF_IMTYPE_MULTILAYER;
  const CompositorInputRealizationMode realization_mode =
      is_multi_layer ? CompositorInputRealizationMode::OperationDomain :
                       CompositorInputRealizationMode::Transforms;

  for (const int i : IndexRange(storage.items_count)) {
    const NodeCompositorFileOutputItem &item = storage.items[i];
    const eNodeSocketDatatype socket_type = eNodeSocketDatatype(item.socket_type);
    const StringRef name = item.name;
    const std::string identifier = FileOutputItemsAccessor::socket_identifier_for_item(item);
    b.add_input(socket_type, name, identifier)
        .structure_type(StructureType::Dynamic)
        .compositor_realization_mode(realization_mode)
        .socket_name_ptr(&node_tree->id, FileOutputItemsAccessor::item_srna, &item, "name");
  }

  b.add_input<decl::Extend>("", "__extend__");
}

static void node_init(const bContext *C, PointerRNA *pointer)
{
  bNode *node = pointer->data_as<bNode>();
  NodeCompositorFileOutput *data = MEM_callocN<NodeCompositorFileOutput>(__func__);
  node->storage = data;
  data->save_as_render = true;

  Scene *scene = CTX_data_scene(C);
  if (scene) {
    RenderData *render_data = &scene->r;
    STRNCPY(data->base_path, render_data->pic);
    BKE_image_format_copy(&data->format, &render_data->im_format);
    data->format.color_management = R_IMF_COLOR_MANAGEMENT_FOLLOW_SCENE;
    if (BKE_imtype_is_movie(data->format.imtype)) {
      data->format.imtype = R_IMF_IMTYPE_OPENEXR;
    }
  }
  else {
    BKE_image_format_init(&data->format, false);
  }
  BKE_image_format_update_color_space_for_type(&data->format);
}

static void node_free_storage(bNode *node)
{
  socket_items::destruct_array<FileOutputItemsAccessor>(*node);
  NodeCompositorFileOutput &data = node_storage(*node);
  BKE_image_format_free(&data.format);
  MEM_freeN(&data);
}

static void node_copy_storage(bNodeTree * /*destination_node_tree*/,
                              bNode *destination_node,
                              const bNode *source_node)
{
  const NodeCompositorFileOutput &source_storage = node_storage(*source_node);
  NodeCompositorFileOutput *destination_storage = MEM_dupallocN<NodeCompositorFileOutput>(
      __func__, source_storage);
  BKE_image_format_copy(&destination_storage->format, &source_storage.format);
  destination_node->storage = destination_storage;
  socket_items::copy_array<FileOutputItemsAccessor>(*source_node, *destination_node);
}

static bool node_insert_link(bNodeTree *node_tree, bNode *node, bNodeLink *link)
{
  return socket_items::try_add_item_via_any_extend_socket<FileOutputItemsAccessor>(
      *node_tree, *node, *node, *link);
}

static void node_operators()
{
  socket_items::ops::make_common_operators<FileOutputItemsAccessor>();
}

static void node_layout(uiLayout *layout, bContext * /*context*/, PointerRNA *pointer)
{
  layout->prop(pointer, "base_path", UI_ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
}

static void item_layout(uiLayout *layout, bContext *context, PointerRNA *pointer)
{
  PointerRNA format_pointer = RNA_pointer_get(pointer, "format");

  layout->prop(
      pointer, "override_node_format", UI_ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

  const bool override_node_format = RNA_boolean_get(pointer, "override_node_format");

  if (override_node_format) {
    {
      uiLayout *column = &layout->column(true);
      column->use_property_split_set(true);
      column->use_property_decorate_set(false);
      column->prop(pointer, "save_as_render", UI_ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    }

    const bool use_color_management = RNA_boolean_get(pointer, "save_as_render");

    uiLayout *column = &layout->column(false);
    uiTemplateImageSettings(column, &format_pointer, use_color_management);

    if (!use_color_management) {
      uiLayout *column = &layout->column(true);
      column->use_property_split_set(true);
      column->use_property_decorate_set(false);

      PointerRNA linear_settings_ptr = RNA_pointer_get(&format_pointer,
                                                       "linear_colorspace_settings");
      column->prop(&linear_settings_ptr, "name", UI_ITEM_NONE, IFACE_("Color Space"), ICON_NONE);
    }

    Scene *scene = CTX_data_scene(context);
    const bool is_multiview = scene->r.scemode & R_MULTIVIEW;
    if (is_multiview) {
      column = &layout->column(false);
      uiTemplateImageFormatViews(column, &format_pointer, nullptr);
    }
  }
}

static void node_layout_ex(uiLayout *layout, bContext *context, PointerRNA *pointer)
{
  node_layout(layout, context, pointer);

  {
    uiLayout *column = &layout->column(true);
    column->use_property_split_set(true);
    column->use_property_decorate_set(false);
    column->prop(pointer, "save_as_render", UI_ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  }
  const bool save_as_render = RNA_boolean_get(pointer, "save_as_render");
  PointerRNA format_pointer = RNA_pointer_get(pointer, "format");
  uiTemplateImageSettings(layout, &format_pointer, save_as_render);

  if (!save_as_render) {
    uiLayout *col = &layout->column(true);
    col->use_property_split_set(true);
    col->use_property_decorate_set(false);

    PointerRNA linear_settings_ptr = RNA_pointer_get(&format_pointer,
                                                     "linear_colorspace_settings");
    col->prop(&linear_settings_ptr, "name", UI_ITEM_NONE, IFACE_("Color Space"), ICON_NONE);
  }

  /* disable stereo output for multilayer, too much work for something that no one will use */
  /* if someone asks for that we can implement it */
  Scene *scene = CTX_data_scene(context);
  const bool is_multiview = scene->r.scemode & R_MULTIVIEW;
  if (is_multiview) {
    uiTemplateImageFormatViews(layout, &format_pointer, nullptr);
  }

  bNodeTree &tree = *reinterpret_cast<bNodeTree *>(pointer->owner_id);
  bNode &node = *pointer->data_as<bNode>();

  if (uiLayout *panel = layout->panel(
          context, "file_output_items", false, IFACE_("File Output Items")))
  {
    socket_items::ui::draw_items_list_with_operators<FileOutputItemsAccessor>(
        context, panel, tree, node);
    socket_items::ui::draw_active_item_props<FileOutputItemsAccessor>(
        tree, node, [&](PointerRNA *item_ptr) {
          panel->use_property_split_set(true);
          panel->use_property_decorate_set(false);
          panel->prop(item_ptr, "socket_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
          const bool is_multilayer = RNA_enum_get(&format_pointer, "file_format") ==
                                     R_IMF_IMTYPE_MULTILAYER;
          if (!is_multilayer) {
            item_layout(panel, context, item_ptr);
          }
        });
  }
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  const NodeCompositorFileOutput &data = node_storage(node);
  BKE_image_format_blend_write(&writer, const_cast<ImageFormatData *>(&data.format));
  socket_items::blend_write<FileOutputItemsAccessor>(&writer, node);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  NodeCompositorFileOutput &data = node_storage(node);
  BKE_image_format_blend_read_data(&reader, &data.format);
  socket_items::blend_read_data<FileOutputItemsAccessor>(&reader, node);
}

using namespace blender::compositor;

class FileOutputOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    if (is_multi_layer()) {
      execute_multi_layer();
    }
    else {
      execute_single_layer();
    }
  }

  /* --------------------
   * Single Layer Images.
   */

  void execute_single_layer()
  {
    const NodeCompositorFileOutput &storage = node_storage(bnode());
    for (const int i : IndexRange(storage.items_count)) {
      const NodeCompositorFileOutputItem &item = storage.items[i];
      const std::string identifier = FileOutputItemsAccessor::socket_identifier_for_item(item);
      const Result &result = get_input(identifier);
      /* We only write images, not single values. */
      if (result.is_single_value()) {
        continue;
      }

      char base_path[FILE_MAX];

      if (!get_single_layer_image_base_path(item.name, base_path)) {
        /* TODO: propagate this error to the render pipeline and UI. */
        BKE_report(nullptr,
                   RPT_ERROR,
                   "Invalid path template in File Output node. Skipping writing file.");
        continue;
      }

      /* The image saving code expects EXR images to have a different structure than standard
       * images. In particular, in EXR images, the buffers need to be stored in passes that are, in
       * turn, stored in a render layer. On the other hand, in non-EXR images, the buffers need to
       * be stored in views. An exception to this is stereo images, which needs to have the same
       * structure as non-EXR images. */
      const auto &format = item.override_node_format ? item.format : node_storage(bnode()).format;
      const bool save_as_render = item.override_node_format ? item.save_as_render :
                                                              node_storage(bnode()).save_as_render;
      const bool is_exr = format.imtype == R_IMF_IMTYPE_OPENEXR;
      const int views_count = BKE_scene_multiview_num_views_get(&context().get_render_data());
      if (is_exr && !(format.views_format == R_IMF_VIEWS_STEREO_3D && views_count == 2)) {
        execute_single_layer_multi_view_exr(result, format, base_path, item.name);
        continue;
      }

      char image_path[FILE_MAX];
      get_single_layer_image_path(base_path, format, image_path);

      const int2 size = result.domain().size;
      FileOutput &file_output = context().render_context()->get_file_output(
          image_path, format, size, save_as_render);

      add_view_for_result(file_output, result, context().get_view_name().data());

      add_meta_data_for_result(file_output, result, item.name);
    }
  }

  /* -----------------------------------
   * Single Layer Multi-View EXR Images.
   */

  void execute_single_layer_multi_view_exr(const Result &result,
                                           const ImageFormatData &format,
                                           const char *base_path,
                                           const char *layer_name)
  {
    const bool has_views = format.views_format != R_IMF_VIEWS_INDIVIDUAL;

    /* The EXR stores all views in the same file, so we supply an empty view to make sure the file
     * name does not contain a view suffix. */
    char image_path[FILE_MAX];
    const char *path_view = has_views ? "" : context().get_view_name().data();

    if (!get_multi_layer_exr_image_path(base_path, path_view, false, image_path)) {
      BLI_assert_unreachable();
      return;
    }

    const int2 size = result.domain().size;
    FileOutput &file_output = context().render_context()->get_file_output(
        image_path, format, size, true);

    /* The EXR stores all views in the same file, so we add the actual render view. Otherwise, we
     * add a default unnamed view. */
    const char *view_name = has_views ? context().get_view_name().data() : "";
    file_output.add_view(view_name);
    add_pass_for_result(file_output, result, "", view_name);

    add_meta_data_for_result(file_output, result, layer_name);
  }

  /* -----------------------
   * Multi-Layer EXR Images.
   */

  void execute_multi_layer()
  {
    const bool store_views_in_single_file = is_multi_view_exr();
    const char *view = context().get_view_name().data();

    /* If we are saving all views in a single multi-layer file, we supply an empty view to make
     * sure the file name does not contain a view suffix. */
    char image_path[FILE_MAX];
    const char *write_view = store_views_in_single_file ? "" : view;
    if (!get_multi_layer_exr_image_path(get_base_path(), write_view, true, image_path)) {
      /* TODO: propagate this error to the render pipeline and UI. */
      BKE_report(
          nullptr, RPT_ERROR, "Invalid path template in File Output node. Skipping writing file.");
      return;
    }

    const int2 size = compute_domain().size;
    const ImageFormatData format = node_storage(bnode()).format;
    FileOutput &file_output = context().render_context()->get_file_output(
        image_path, format, size, true);

    /* If we are saving views in separate files, we needn't store the view in the channel names, so
     * we add an unnamed view. */
    const char *pass_view = store_views_in_single_file ? view : "";
    file_output.add_view(pass_view);

    const NodeCompositorFileOutput &storage = node_storage(bnode());
    for (const int i : IndexRange(storage.items_count)) {
      const NodeCompositorFileOutputItem &item = storage.items[i];
      const std::string identifier = FileOutputItemsAccessor::socket_identifier_for_item(item);
      const Result &input_result = get_input(identifier);
      add_pass_for_result(file_output, input_result, item.name, pass_view);

      add_meta_data_for_result(file_output, input_result, item.name);
    }
  }

  /* Read the data stored in the given result and add a pass of the given name, view, and read
   * buffer. The pass channel identifiers follows the EXR conventions. */
  void add_pass_for_result(FileOutput &file_output,
                           const Result &result,
                           const char *pass_name,
                           const char *view_name)
  {
    /* For single values, we fill a buffer that covers the domain of the operation with the value
     * of the result. */
    const int2 size = result.is_single_value() ? this->compute_domain().size :
                                                 result.domain().size;

    /* The image buffer in the file output will take ownership of this buffer and freeing it will
     * be its responsibility. */
    float *buffer = nullptr;
    if (result.is_single_value()) {
      buffer = this->inflate_result(result, size);
    }
    else {
      if (context().use_gpu()) {
        GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
        buffer = static_cast<float *>(GPU_texture_read(result, GPU_DATA_FLOAT, 0));
      }
      else {
        /* Copy the result into a new buffer. */
        buffer = static_cast<float *>(MEM_dupallocN(result.cpu_data().data()));
      }
    }

    switch (result.type()) {
      case ResultType::Color:
        /* Use lowercase rgba for Cryptomatte layers because the EXR internal compression rules
         * specify that all uppercase RGBA channels will be compressed, and Cryptomatte should not
         * be compressed. */
        if (result.meta_data.is_cryptomatte_layer()) {
          file_output.add_pass(pass_name, view_name, "rgba", buffer);
        }
        else {
          file_output.add_pass(pass_name, view_name, "RGBA", buffer);
        }
        break;
      case ResultType::Float3:
        /* Float3 results might be stored in 4-component textures due to hardware limitations, so
         * we need to convert the buffer to a 3-component buffer on the host. */
        if (this->context().use_gpu() && GPU_texture_component_len(GPU_texture_format(result))) {
          file_output.add_pass(pass_name, view_name, "XYZ", float4_to_float3_image(size, buffer));
        }
        else {
          file_output.add_pass(pass_name, view_name, "XYZ", buffer);
        }
        break;
      case ResultType::Float4:
        file_output.add_pass(pass_name, view_name, "XYZW", buffer);
        break;
      case ResultType::Float:
        file_output.add_pass(pass_name, view_name, "V", buffer);
        break;
      case ResultType::Float2:
        file_output.add_pass(pass_name, view_name, "XY", buffer);
        break;
      case ResultType::Int2:
        file_output.add_pass(pass_name, view_name, "XY", buffer);
        break;
      case ResultType::Int:
        file_output.add_pass(pass_name, view_name, "V", buffer);
        break;
      case ResultType::Bool:
        file_output.add_pass(pass_name, view_name, "V", buffer);
        break;
    }
  }

  /* Allocates and fills an image buffer of the specified size with the value of the given single
   * value result. */
  float *inflate_result(const Result &result, const int2 size)
  {
    BLI_assert(result.is_single_value());

    const int64_t length = int64_t(size.x) * size.y;
    const int64_t buffer_size = length * result.channels_count();
    float *buffer = MEM_malloc_arrayN<float>(buffer_size, "File Output Inflated Buffer.");

    switch (result.type()) {
      case ResultType::Float:
      case ResultType::Float2:
      case ResultType::Float3:
      case ResultType::Float4:
      case ResultType::Color: {
        const GPointer single_value = result.single_value();
        single_value.type()->fill_assign_n(single_value.get(), buffer, length);
        return buffer;
      }
      case ResultType::Int: {
        const float value = float(result.get_single_value<int32_t>());
        CPPType::get<float>().fill_assign_n(&value, buffer, length);
        return buffer;
      }
      case ResultType::Int2: {
        const float2 value = float2(result.get_single_value<int2>());
        CPPType::get<float2>().fill_assign_n(&value, buffer, length);
        return buffer;
      }
      case ResultType::Bool: {
        const float value = float(result.get_single_value<bool>());
        CPPType::get<float>().fill_assign_n(&value, buffer, length);
        return buffer;
      }
    }

    BLI_assert_unreachable();
    return nullptr;
  }

  /* Read the data stored the given result and add a view of the given name and read buffer. */
  void add_view_for_result(FileOutput &file_output, const Result &result, const char *view_name)
  {
    /* The image buffer in the file output will take ownership of this buffer and freeing it will
     * be its responsibility. */
    float *buffer = nullptr;
    if (context().use_gpu()) {
      GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
      buffer = static_cast<float *>(GPU_texture_read(result, GPU_DATA_FLOAT, 0));
    }
    else {
      /* Copy the result into a new buffer. */
      buffer = static_cast<float *>(MEM_dupallocN(result.cpu_data().data()));
    }

    const int2 size = result.domain().size;
    switch (result.type()) {
      case ResultType::Color:
        file_output.add_view(view_name, 4, buffer);
        break;
      case ResultType::Float4:
        file_output.add_view(view_name, 4, buffer);
        break;
      case ResultType::Float3:
        /* Float3 results might be stored in 4-component textures due to hardware limitations, so
         * we need to convert the buffer to a 3-component buffer on the host. */
        if (this->context().use_gpu() && GPU_texture_component_len(GPU_texture_format(result))) {
          file_output.add_view(view_name, 3, float4_to_float3_image(size, buffer));
        }
        else {
          file_output.add_view(view_name, 3, buffer);
        }
        break;
      case ResultType::Float:
        file_output.add_view(view_name, 1, buffer);
        break;
      case ResultType::Float2:
      case ResultType::Int2:
      case ResultType::Int:
      case ResultType::Bool:
        /* Not supported. */
        BLI_assert_unreachable();
        break;
    }
  }

  /* Given a float4 image, return a newly allocated float3 image that ignores the last channel. The
   * input image is freed. */
  float *float4_to_float3_image(int2 size, float *float4_image)
  {
    float *float3_image = MEM_malloc_arrayN<float>(3 * size_t(size.x) * size_t(size.y),
                                                   "File Output Vector Buffer.");

    parallel_for(size, [&](const int2 texel) {
      for (int i = 0; i < 3; i++) {
        const int64_t pixel_index = int64_t(texel.y) * size.x + texel.x;
        float3_image[pixel_index * 3 + i] = float4_image[pixel_index * 4 + i];
      }
    });

    MEM_freeN(float4_image);
    return float3_image;
  }

  /* Add Cryptomatte meta data to the file if they exist for the given result of the given layer
   * name. We do not write any other meta data for now. */
  void add_meta_data_for_result(FileOutput &file_output, const Result &result, const char *name)
  {
    StringRef cryptomatte_layer_name = bke::cryptomatte::BKE_cryptomatte_extract_layer_name(name);

    if (result.meta_data.is_cryptomatte_layer()) {
      file_output.add_meta_data(
          bke::cryptomatte::BKE_cryptomatte_meta_data_key(cryptomatte_layer_name, "name"),
          cryptomatte_layer_name);
    }

    if (!result.meta_data.cryptomatte.manifest.empty()) {
      file_output.add_meta_data(
          bke::cryptomatte::BKE_cryptomatte_meta_data_key(cryptomatte_layer_name, "manifest"),
          result.meta_data.cryptomatte.manifest);
    }

    if (!result.meta_data.cryptomatte.hash.empty()) {
      file_output.add_meta_data(
          bke::cryptomatte::BKE_cryptomatte_meta_data_key(cryptomatte_layer_name, "hash"),
          result.meta_data.cryptomatte.hash);
    }

    if (!result.meta_data.cryptomatte.conversion.empty()) {
      file_output.add_meta_data(
          bke::cryptomatte::BKE_cryptomatte_meta_data_key(cryptomatte_layer_name, "conversion"),
          result.meta_data.cryptomatte.conversion);
    }
  }

  /**
   * Get the base path of the image to be saved, based on the base path of the
   * node. The base name is an optional initial name of the image, which will
   * later be concatenated with other information like the frame number, view,
   * and extension. If the base name is empty, then the base path represents a
   * directory, so a trailing slash is ensured.
   *
   * Note: this takes care of path template expansion as well.
   *
   * If there are any errors processing the path, `bath_base` will be set to an
   * empty string.
   *
   * \return True on success, false if there were any errors processing the
   * path.
   */
  bool get_single_layer_image_base_path(const char *base_name, char *r_base_path)
  {
    path_templates::VariableMap template_variables;
    BKE_add_template_variables_general(template_variables, &this->bnode().owner_tree().id);
    BKE_add_template_variables_for_render_path(template_variables, context().get_scene());
    BKE_add_template_variables_for_node(template_variables, this->bnode());

    /* Do template expansion on the node's base path. */
    char node_base_path[FILE_MAX] = "";
    STRNCPY(node_base_path, get_base_path());
    {
      blender::Vector<path_templates::Error> errors = BKE_path_apply_template(
          node_base_path, FILE_MAX, template_variables);
      if (!errors.is_empty()) {
        r_base_path[0] = '\0';
        return false;
      }
    }

    if (base_name[0]) {
      /* Do template expansion on the socket's sub path ("base name"). */
      char sub_path[FILE_MAX] = "";
      STRNCPY(sub_path, base_name);
      {
        blender::Vector<path_templates::Error> errors = BKE_path_apply_template(
            sub_path, FILE_MAX, template_variables);
        if (!errors.is_empty()) {
          r_base_path[0] = '\0';
          return false;
        }
      }

      /* Combine the base path and sub path. */
      BLI_path_join(r_base_path, FILE_MAX, node_base_path, sub_path);
    }
    else {
      /* Just use the base path, as a directory. */
      BLI_strncpy(r_base_path, node_base_path, FILE_MAX);
      BLI_path_slash_ensure(r_base_path, FILE_MAX);
    }

    return true;
  }

  /* Get the path of the image to be saved based on the given format. */
  void get_single_layer_image_path(const char *base_path,
                                   const ImageFormatData &format,
                                   char *r_image_path)
  {
    BKE_image_path_from_imformat(r_image_path,
                                 base_path,
                                 BKE_main_blendfile_path_from_global(),
                                 /* No variables, because path templating is
                                  * already done by
                                  * `get_single_layer_image_base_path()` before
                                  * this is called. */
                                 nullptr,
                                 context().get_frame_number(),
                                 &format,
                                 use_file_extension(),
                                 true,
                                 nullptr);
  }

  /**
   * Get the path of the EXR image to be saved. If the given view is not empty,
   * its corresponding file suffix will be appended to the name.
   *
   * If there are any errors processing the path, the resulting path will be
   * empty.
   *
   * \param apply_template Whether to run templating on the path or not. This is
   * needed because this function is called from more than one place, some of
   * which have already applied templating to the path and some of which
   * haven't. Double-applying templating can give incorrect results.
   *
   * \return True on success, false if there were any errors processing the
   * path.
   */
  bool get_multi_layer_exr_image_path(const char *base_path,
                                      const char *view,
                                      const bool apply_template,
                                      char *r_image_path)
  {
    const Scene *scene = &context().get_scene();
    const RenderData &render_data = context().get_render_data();
    path_templates::VariableMap template_variables;
    BKE_add_template_variables_general(template_variables, &this->bnode().owner_tree().id);
    BKE_add_template_variables_for_render_path(template_variables, *scene);
    BKE_add_template_variables_for_node(template_variables, this->bnode());

    const char *suffix = BKE_scene_multiview_view_suffix_get(&render_data, view);
    const char *relbase = BKE_main_blendfile_path_from_global();
    blender::Vector<path_templates::Error> errors = BKE_image_path_from_imtype(
        r_image_path,
        base_path,
        relbase,
        apply_template ? &template_variables : nullptr,
        context().get_frame_number(),
        R_IMF_IMTYPE_MULTILAYER,
        use_file_extension(),
        true,
        suffix);

    if (!errors.is_empty()) {
      r_image_path[0] = '\0';
    }

    return errors.is_empty();
  }

  bool is_multi_layer()
  {
    return node_storage(bnode()).format.imtype == R_IMF_IMTYPE_MULTILAYER;
  }

  const char *get_base_path()
  {
    return node_storage(bnode()).base_path;
  }

  /* Add the file format extensions to the rendered file name. */
  bool use_file_extension()
  {
    return context().get_render_data().scemode & R_EXTENSION;
  }

  /* If true, save views in a multi-view EXR file, otherwise, save each view in its own file. */
  bool is_multi_view_exr()
  {
    if (!is_multi_view_scene()) {
      return false;
    }

    return node_storage(bnode()).format.views_format == R_IMF_VIEWS_MULTIVIEW;
  }

  bool is_multi_view_scene()
  {
    return context().get_render_data().scemode & R_MULTIVIEW;
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new FileOutputOperation(context, node);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeOutputFile", CMP_NODE_OUTPUT_FILE);
  ntype.ui_name = "File Output";
  ntype.ui_description = "Write image file to disk";
  ntype.enum_name_legacy = "OUTPUT_FILE";
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  ntype.insert_link = node_insert_link;
  ntype.register_operators = node_operators;
  ntype.initfunc_api = node_init;
  ntype.flag |= NODE_PREVIEW;
  blender::bke::node_type_storage(
      ntype, "NodeCompositorFileOutput", node_free_storage, node_copy_storage);
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  ntype.get_compositor_operation = get_compositor_operation;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_file_output_cc

namespace blender::nodes {

StructRNA *FileOutputItemsAccessor::item_srna = &RNA_NodeCompositorFileOutputItem;

void FileOutputItemsAccessor::blend_write_item(BlendWriter *writer, const ItemT &item)
{
  BLO_write_string(writer, item.name);
  BKE_image_format_blend_write(writer, const_cast<ImageFormatData *>(&item.format));
}

void FileOutputItemsAccessor::blend_read_data_item(BlendDataReader *reader, ItemT &item)
{
  BLO_read_string(reader, &item.name);
  BKE_image_format_blend_read_data(reader, &item.format);
}

std::string FileOutputItemsAccessor::validate_name(const StringRef name)
{
  /* TODO: Validate filename. */
  return name;
}

}  // namespace blender::nodes
