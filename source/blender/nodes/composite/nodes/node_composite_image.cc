/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_assert.h"
#include "BLI_listbase.h"
#include "BLI_memory_utils.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_utildefines.h"

#include "BKE_image.hh"
#include "BKE_node_runtime.hh"

#include "DNA_image_types.h"
#include "DNA_scene_types.h"

#include "COM_algorithm_extract_alpha.hh"
#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_image_cc {

/* Default declaration for contextless static declarations and when the image is not assigned. */
static void declare_default(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  b.add_output<decl::Float>("Alpha").structure_type(StructureType::Dynamic);
}

/* Declaration for simple single layer images. */
static void declare_single_layer(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
  b.add_output<decl::Float>("Alpha").structure_type(StructureType::Dynamic);
}

/* Declares an already existing output. */
static BaseSocketDeclarationBuilder &declare_existing_output(NodeDeclarationBuilder &b,
                                                             const bNodeSocket *output)
{
  if (output->type == SOCK_VECTOR) {
    const int dimensions = output->default_value_typed<bNodeSocketValueVector>()->dimensions;
    return b.add_output<decl::Vector>(output->name)
        .dimensions(dimensions)
        .structure_type(StructureType::Dynamic);
  }
  return b.add_output(eNodeSocketDatatype(output->type), output->name)
      .structure_type(StructureType::Dynamic);
}

/* Declares the already existing outputs. This is done in cases where the passes can not be read
 * due to an invalid image to retain the links and give the user the opportunity to update the
 * image such that becomes valid again. */
static void declare_existing(NodeDeclarationBuilder &b, const bNode *node)
{
  LISTBASE_FOREACH (const bNodeSocket *, output, &node->outputs) {
    declare_existing_output(b, output);
  }
}

/* Declares an output that matches the type of the given pass. */
static void declare_pass(NodeDeclarationBuilder &b, const RenderPass &pass)
{
  switch (pass.channels) {
    case 1:
      b.add_output<decl::Float>(pass.name).structure_type(StructureType::Dynamic);
      break;
    case 2:
      b.add_output<decl::Vector>(pass.name).dimensions(2).structure_type(StructureType::Dynamic);
      break;
    case 3:
      if (STR_ELEM(pass.chan_id, "RGB", "rgb")) {
        b.add_output<decl::Color>(pass.name).structure_type(StructureType::Dynamic);
      }
      else {
        b.add_output<decl::Vector>(pass.name).dimensions(3).structure_type(StructureType::Dynamic);
      }
      break;
    case 4:
      if (STR_ELEM(pass.chan_id, "RGBA", "rgba")) {
        b.add_output<decl::Color>(pass.name).structure_type(StructureType::Dynamic);
      }
      else {
        b.add_output<decl::Vector>(pass.name).dimensions(4).structure_type(StructureType::Dynamic);
      }
      break;
    default:
      BLI_assert_unreachable();
      break;
  }

  /* The Alpha pass is generated based on the combined pass. */
  if (STREQ(pass.name, RE_PASSNAME_COMBINED)) {
    b.add_output<decl::Float>("Alpha").structure_type(StructureType::Dynamic);
  }
}

static void node_declare_multi_layer(NodeDeclarationBuilder &b,
                                     Image *image,
                                     const ImageUser *image_user,
                                     const bNode *node)
{
  RenderResult *render_result = BKE_image_acquire_renderresult(nullptr, image);
  BLI_SCOPED_DEFER([&]() { BKE_image_release_renderresult(nullptr, image, render_result); });

  if (!render_result) {
    declare_existing(b, node);
    return;
  }

  RenderLayer *render_layer = static_cast<RenderLayer *>(
      BLI_findlink(&render_result->layers, image_user->layer));
  if (!render_layer) {
    declare_existing(b, node);
    return;
  }

  LISTBASE_FOREACH (RenderPass *, pass, &render_layer->passes) {
    declare_pass(b, *pass);
  }
}

/* The image may not necessary have its type initialized correctly yet, so we can't identify if it
 * is multi-layer or not. Further, the render result structure for multi-layer images may also not
 * be initialized yet, so we can't retrieve the passes. So this function prepares the image by
 * acquiring a dummy image buffer since it initializes the necessary data we need as a side effect.
 * This image buffer can be immediately released. Since it carries no important information. */
static void prepare_image(Image *image, const ImageUser *image_user)
{
  /* Create a copy of image user that represents the structure of the image at the first frame. We
   * do not support a temporally changing image structure, since that changes the topology of the
   * node tree. */
  const int image_start_frame_offset = BKE_image_sequence_guess_offset(image);
  ImageUser initial_frame_image_user = *image_user;
  initial_frame_image_user.framenr = image_start_frame_offset;

  ImBuf *initial_image_buffer = BKE_image_acquire_ibuf(image, &initial_frame_image_user, nullptr);
  BKE_image_release_ibuf(image, initial_image_buffer, nullptr);
}

/* Declares outputs that are linked and existed in the previous state of the node but no longer
 * exist in the new state. The outputs are set as unavailable, so they are not accessible to the
 * user. This is useful to retain links if the user accidentally changed the image or the image was
 * changed through some external factor without an explicit action from the user. */
static void declare_old_linked_outputs(NodeDeclarationBuilder &b)
{
  Set<std::string> added_outputs_identifiers;
  for (const SocketDeclaration *output_declaration : b.declaration().sockets(SOCK_OUT)) {
    added_outputs_identifiers.add_new(output_declaration->identifier);
  }

  const bNodeTree *node_tree = b.tree_or_null();
  const bNode *node = b.node_or_null();
  node_tree->ensure_topology_cache();
  for (const bNodeSocket *output : node->output_sockets()) {
    if (added_outputs_identifiers.contains(output->identifier)) {
      continue;
    }
    if (!output->is_directly_linked()) {
      continue;
    }
    declare_existing_output(b, output).available(false);
  }
}

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (!node) {
    declare_default(b);
    return;
  }

  const bNodeTree *node_tree = b.tree_or_null();
  if (!node_tree) {
    declare_default(b);
    return;
  }

  BLI_SCOPED_DEFER([&]() { declare_old_linked_outputs(b); });

  Image *image = reinterpret_cast<Image *>(node->id);
  const ImageUser *image_user = static_cast<ImageUser *>(node->storage);
  if (!image || !image_user) {
    declare_default(b);
    return;
  }

  prepare_image(image, image_user);

  if (!BKE_image_is_multilayer(image)) {
    declare_single_layer(b);
    return;
  }

  node_declare_multi_layer(b, image, image_user, node);
}

static void node_init(bNodeTree * /*node_tree*/, bNode *node)
{
  ImageUser *iuser = MEM_callocN<ImageUser>(__func__);
  node->storage = iuser;
  iuser->frames = 1;
  iuser->sfra = 1;
  iuser->flag |= IMA_ANIM_ALWAYS;
}

using namespace blender::compositor;

class ImageOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    for (const bNodeSocket *output : this->node()->output_sockets()) {
      if (!is_socket_available(output)) {
        continue;
      }

      this->compute_output(output->identifier);
    }
  }

  void compute_output(StringRef identifier)
  {
    if (!this->should_compute_output(identifier)) {
      return;
    }

    /* Alpha is not an actual pass, but one that is extracted from the combined pass. */
    const bool is_generated_alpha = identifier == "Alpha";
    const char *pass_name = is_generated_alpha ? RE_PASSNAME_COMBINED : identifier.data();
    Result cached_image = this->context().cache_manager().cached_images.get(
        this->context(), this->get_image(), this->get_image_user(), pass_name);

    Result &result = this->get_result(identifier);
    if (!cached_image.is_allocated()) {
      result.allocate_invalid();
      return;
    }

    if (is_generated_alpha) {
      extract_alpha(this->context(), cached_image, result);
    }
    else {
      result.set_type(cached_image.type());
      result.set_precision(cached_image.precision());
      result.wrap_external(cached_image);
    }
  }

  Image *get_image()
  {
    return reinterpret_cast<Image *>(bnode().id);
  }

  ImageUser *get_image_user()
  {
    return static_cast<ImageUser *>(bnode().storage);
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new ImageOperation(context, node);
}

static void register_node()
{
  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeImage", CMP_NODE_IMAGE);
  ntype.ui_name = "Image";
  ntype.ui_description = "Input image or movie file";
  ntype.enum_name_legacy = "IMAGE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  blender::bke::node_type_storage(
      ntype, "ImageUser", node_free_standard_storage, node_copy_standard_storage);
  ntype.get_compositor_operation = get_compositor_operation;
  ntype.labelfunc = node_image_label;
  ntype.flag |= NODE_PREVIEW;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node)

}  // namespace blender::nodes::node_composite_image_cc
