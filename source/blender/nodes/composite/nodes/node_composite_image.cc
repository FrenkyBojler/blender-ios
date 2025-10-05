/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_assert.h"
#include "BLI_listbase.h"
#include "BLI_memory_utils.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"

#include "BKE_image.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"

#include "DNA_image_types.h"
#include "DNA_layer_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "COM_algorithm_extract_alpha.hh"
#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"
/* **************** IMAGE (and RenderResult, multi-layer image) ******************** */

static blender::bke::bNodeSocketTemplate cmp_node_rlayers_out[] = {
    {SOCK_RGBA, N_("Image"), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_FLOAT, N_("Alpha"), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_FLOAT, N_(RE_PASSNAME_DEPTH), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_VECTOR, N_(RE_PASSNAME_NORMAL), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_VECTOR, N_(RE_PASSNAME_UV), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_VECTOR, N_(RE_PASSNAME_VECTOR), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_VECTOR, N_(RE_PASSNAME_POSITION), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_DEPRECATED), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_DEPRECATED), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_SHADOW), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_AO), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_DEPRECATED), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_DEPRECATED), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_DEPRECATED), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_FLOAT, N_(RE_PASSNAME_INDEXOB), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_FLOAT, N_(RE_PASSNAME_INDEXMA), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_FLOAT, N_(RE_PASSNAME_MIST), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_EMIT), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_ENVIRONMENT), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_DIFFUSE_DIRECT), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_DIFFUSE_INDIRECT), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_DIFFUSE_COLOR), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_GLOSSY_DIRECT), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_GLOSSY_INDIRECT), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_GLOSSY_COLOR), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_TRANSM_DIRECT), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_TRANSM_INDIRECT), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_TRANSM_COLOR), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_SUBSURFACE_DIRECT), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_SUBSURFACE_INDIRECT), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {SOCK_RGBA, N_(RE_PASSNAME_SUBSURFACE_COLOR), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {-1, ""},
};
#define NUM_LEGACY_SOCKETS (ARRAY_SIZE(cmp_node_rlayers_out) - 1)

static const char *cmp_node_legacy_pass_name(const char *name)
{
  if (STREQ(name, "Diffuse Direct")) {
    return "DiffDir";
  }
  if (STREQ(name, "Diffuse Indirect")) {
    return "DiffInd";
  }
  if (STREQ(name, "Diffuse Color")) {
    return "DiffCol";
  }
  if (STREQ(name, "Glossy Direct")) {
    return "GlossDir";
  }
  if (STREQ(name, "Glossy Indirect")) {
    return "GlossInd";
  }
  if (STREQ(name, "Glossy Color")) {
    return "GlossCol";
  }
  if (STREQ(name, "Transmission Direct")) {
    return "TransDir";
  }
  if (STREQ(name, "Transmission Indirect")) {
    return "TransInd";
  }
  if (STREQ(name, "Transmission Color")) {
    return "TransCol";
  }
  if (STREQ(name, "Volume Direct")) {
    return "VolumeDir";
  }
  if (STREQ(name, "Volume Indirect")) {
    return "VolumeInd";
  }
  if (STREQ(name, "Volume Color")) {
    return "VolumeCol";
  }
  if (STREQ(name, "Ambient Occlusion")) {
    return "AO";
  }
  if (STREQ(name, "Environment")) {
    return "Env";
  }
  if (STREQ(name, "Material Index")) {
    return "IndexMA";
  }
  if (STREQ(name, "Object Index")) {
    return "IndexOB";
  }
  if (STREQ(name, "Grease Pencil")) {
    return "GreasePencil";
  }
  if (STREQ(name, "Emission")) {
    return "Emit";
  }

  return nullptr;
}

static void cmp_node_image_add_pass_output(bNodeTree *ntree,
                                           bNode *node,
                                           const char *name,
                                           const char *passname,
                                           int rres_index,
                                           eNodeSocketDatatype type,
                                           int /*is_rlayers*/,
                                           LinkNodePair *available_sockets,
                                           int *prev_index)
{
  bNodeSocket *sock = (bNodeSocket *)BLI_findstring(
      &node->outputs, name, offsetof(bNodeSocket, name));

  /* Rename legacy socket names to new ones. */
  if (sock == nullptr) {
    const char *legacy_name = cmp_node_legacy_pass_name(name);
    if (legacy_name) {
      sock = (bNodeSocket *)BLI_findstring(
          &node->outputs, legacy_name, offsetof(bNodeSocket, name));
      if (sock) {
        STRNCPY(sock->name, name);
        STRNCPY(sock->identifier, name);
      }
    }
  }

  /* Replace if types don't match. */
  if (sock && sock->type != type) {
    blender::bke::node_remove_socket(*ntree, *node, *sock);
    sock = nullptr;
  }

  /* Create socket if it doesn't exist yet. */
  if (sock == nullptr) {
    if (rres_index >= 0) {
      sock = node_add_socket_from_template(
          ntree, node, &cmp_node_rlayers_out[rres_index], SOCK_OUT);
    }
    else {
      sock = blender::bke::node_add_static_socket(
          *ntree, *node, SOCK_OUT, type, PROP_NONE, name, name);
    }
    /* extra socket info */
    NodeImageLayer *sockdata = MEM_callocN<NodeImageLayer>(__func__);
    sock->storage = sockdata;
  }

  NodeImageLayer *sockdata = (NodeImageLayer *)sock->storage;
  if (sockdata) {
    STRNCPY_UTF8(sockdata->pass_name, passname);
  }

  /* Reorder sockets according to order that passes are added. */
  const int after_index = (*prev_index)++;
  bNodeSocket *after_sock = (bNodeSocket *)BLI_findlink(&node->outputs, after_index);
  BLI_remlink(&node->outputs, sock);
  BLI_insertlinkafter(&node->outputs, after_sock, sock);

  BLI_linklist_append(available_sockets, sock);
}

static eNodeSocketDatatype socket_type_from_pass(const RenderPass *pass)
{
  switch (pass->channels) {
    case 1:
      return SOCK_FLOAT;
    case 2:
    case 3:
      if (STR_ELEM(pass->chan_id, "RGB", "rgb")) {
        return SOCK_RGBA;
      }
      else {
        return SOCK_VECTOR;
      }
    case 4:
      if (STR_ELEM(pass->chan_id, "RGBA", "rgba")) {
        return SOCK_RGBA;
      }
      else {
        return SOCK_VECTOR;
      }
    default:
      break;
  }

  BLI_assert_unreachable();
  return SOCK_FLOAT;
}

static void cmp_node_image_create_outputs(bNodeTree *ntree,
                                          bNode *node,
                                          LinkNodePair *available_sockets)
{
  Image *ima = (Image *)node->id;
  ImBuf *ibuf;
  int prev_index = -1;
  if (ima) {
    ImageUser *iuser = (ImageUser *)node->storage;
    ImageUser load_iuser = {nullptr};
    int offset = BKE_image_sequence_guess_offset(ima);

    /* It is possible that image user in this node is not
     * properly updated yet. In this case loading image will
     * fail and sockets detection will go wrong.
     *
     * So we manually construct image user to be sure first
     * image from sequence (that one which is set as filename
     * for image data-block) is used for sockets detection. */
    load_iuser.framenr = offset;

    /* make sure ima->type is correct */
    ibuf = BKE_image_acquire_ibuf(ima, &load_iuser, nullptr);

    if (ima->rr) {
      RenderLayer *rl = (RenderLayer *)BLI_findlink(&ima->rr->layers, iuser->layer);

      if (rl) {
        LISTBASE_FOREACH (RenderPass *, rpass, &rl->passes) {
          const eNodeSocketDatatype type = socket_type_from_pass(rpass);
          cmp_node_image_add_pass_output(ntree,
                                         node,
                                         rpass->name,
                                         rpass->name,
                                         -1,
                                         type,
                                         false,
                                         available_sockets,
                                         &prev_index);
          /* Special handling for the Combined pass to ensure compatibility. */
          if (STREQ(rpass->name, RE_PASSNAME_COMBINED)) {
            cmp_node_image_add_pass_output(ntree,
                                           node,
                                           "Alpha",
                                           rpass->name,
                                           -1,
                                           SOCK_FLOAT,
                                           false,
                                           available_sockets,
                                           &prev_index);
          }
        }
        BKE_image_release_ibuf(ima, ibuf, nullptr);
        return;
      }
    }
  }

  cmp_node_image_add_pass_output(ntree,
                                 node,
                                 "Image",
                                 RE_PASSNAME_COMBINED,
                                 -1,
                                 SOCK_RGBA,
                                 false,
                                 available_sockets,
                                 &prev_index);
  cmp_node_image_add_pass_output(ntree,
                                 node,
                                 "Alpha",
                                 RE_PASSNAME_COMBINED,
                                 -1,
                                 SOCK_FLOAT,
                                 false,
                                 available_sockets,
                                 &prev_index);

  if (ima) {
    BKE_image_release_ibuf(ima, ibuf, nullptr);
  }
}

struct RLayerUpdateData {
  LinkNodePair *available_sockets;
  int prev_index;
};

void node_cmp_rlayers_register_pass(bNodeTree *ntree,
                                    bNode *node,
                                    Scene *scene,
                                    ViewLayer *view_layer,
                                    const char *name,
                                    eNodeSocketDatatype type)
{
  RLayerUpdateData *data = (RLayerUpdateData *)node->storage;

  if (scene == nullptr || view_layer == nullptr || data == nullptr || node->id != (ID *)scene) {
    return;
  }

  ViewLayer *node_view_layer = (ViewLayer *)BLI_findlink(&scene->view_layers, node->custom1);
  if (node_view_layer != view_layer) {
    return;
  }

  /* Special handling for the Combined pass to ensure compatibility. */
  if (STREQ(name, RE_PASSNAME_COMBINED)) {
    cmp_node_image_add_pass_output(
        ntree, node, "Image", name, -1, type, true, data->available_sockets, &data->prev_index);
    cmp_node_image_add_pass_output(ntree,
                                   node,
                                   "Alpha",
                                   name,
                                   -1,
                                   SOCK_FLOAT,
                                   true,
                                   data->available_sockets,
                                   &data->prev_index);
  }
  else {
    cmp_node_image_add_pass_output(
        ntree, node, name, name, -1, type, true, data->available_sockets, &data->prev_index);
  }
}

struct CreateOutputUserData {
  bNodeTree &ntree;
  bNode &node;
};

static void cmp_node_rlayer_create_outputs_cb(void *userdata,
                                              Scene *scene,
                                              ViewLayer *view_layer,
                                              const char *name,
                                              int /*channels*/,
                                              const char * /*chanid*/,
                                              eNodeSocketDatatype type)
{
  CreateOutputUserData &data = *(CreateOutputUserData *)userdata;
  node_cmp_rlayers_register_pass(&data.ntree, &data.node, scene, view_layer, name, type);
}

static void cmp_node_rlayer_create_outputs(bNodeTree *ntree,
                                           bNode *node,
                                           LinkNodePair *available_sockets)
{
  Scene *scene = (Scene *)node->id;

  if (scene) {
    RenderEngineType *engine_type = RE_engines_find(scene->r.engine);
    if (engine_type && engine_type->update_render_passes) {
      ViewLayer *view_layer = (ViewLayer *)BLI_findlink(&scene->view_layers, node->custom1);
      if (view_layer) {
        RLayerUpdateData *data = MEM_mallocN<RLayerUpdateData>("render layer update data");
        data->available_sockets = available_sockets;
        data->prev_index = -1;
        node->storage = data;

        CreateOutputUserData userdata = {*ntree, *node};

        RenderEngine *engine = RE_engine_create(engine_type);
        RE_engine_update_render_passes(
            engine, scene, view_layer, cmp_node_rlayer_create_outputs_cb, &userdata);
        RE_engine_free(engine);

        if ((scene->r.mode & R_EDGE_FRS) &&
            (view_layer->freestyle_config.flags & FREESTYLE_AS_RENDER_PASS))
        {
          node_cmp_rlayers_register_pass(
              ntree, node, scene, view_layer, RE_PASSNAME_FREESTYLE, SOCK_RGBA);
        }

        if (view_layer->grease_pencil_flags & GREASE_PENCIL_AS_SEPARATE_PASS) {
          node_cmp_rlayers_register_pass(
              ntree, node, scene, view_layer, RE_PASSNAME_GREASE_PENCIL, SOCK_RGBA);
        }

        /* Register LPE passes */
        if (!BLI_listbase_is_empty(&view_layer->lpes)) {
          LISTBASE_FOREACH (ViewLayerLPE *, lpe, &view_layer->lpes) {
            if (lpe->name[0] != '\0' && lpe->expression[0] != '\0') {
              node_cmp_rlayers_register_pass(
                  ntree, node, scene, view_layer, lpe->name, SOCK_RGBA);
            }
          }
        }

        MEM_freeN(data);
        node->storage = nullptr;

        return;
      }
    }
  }

  int prev_index = -1;
  cmp_node_image_add_pass_output(ntree,
                                 node,
                                 "Image",
                                 RE_PASSNAME_COMBINED,
                                 RRES_OUT_IMAGE,
                                 SOCK_RGBA,
                                 true,
                                 available_sockets,
                                 &prev_index);
  cmp_node_image_add_pass_output(ntree,
                                 node,
                                 "Alpha",
                                 RE_PASSNAME_COMBINED,
                                 RRES_OUT_ALPHA,
                                 SOCK_FLOAT,
                                 true,
                                 available_sockets,
                                 &prev_index);
}

/* XXX make this into a generic socket verification function for dynamic socket replacement
 * (multi-layer, groups, static templates). */
static void cmp_node_image_verify_outputs(bNodeTree *ntree, bNode *node, bool rlayer)
{
  bNodeSocket *sock, *sock_next;
  LinkNodePair available_sockets = {nullptr, nullptr};

  /* XXX make callback */
  if (rlayer) {
    cmp_node_rlayer_create_outputs(ntree, node, &available_sockets);
  }
  else {
    cmp_node_image_create_outputs(ntree, node, &available_sockets);
  }

  /* Get rid of sockets whose passes are not available in the image.
   * If sockets that are not available would be deleted, the connections to them would be lost
   * when e.g. opening a file (since there's no render at all yet).
   * Therefore, sockets with connected links will just be set as unavailable.
   *
   * Another important detail comes from compatibility with the older socket model, where there
   * was a fixed socket per pass type that was just hidden or not. Therefore, older versions expect
   * the first 31 passes to belong to a specific pass type.
   * So, we keep those 31 always allocated before the others as well,
   * even if they have no links attached. */
  int sock_index = 0;
  for (sock = (bNodeSocket *)node->outputs.first; sock; sock = sock_next, sock_index++) {
    sock_next = sock->next;
    if (BLI_linklist_index(available_sockets.list, sock) >= 0) {
      sock->flag &= ~SOCK_HIDDEN;
      blender::bke::node_set_socket_availability(*ntree, *sock, true);
    }
    else {
      bNodeLink *link;
      for (link = (bNodeLink *)ntree->links.first; link; link = link->next) {
        if (link->fromsock == sock) {
          break;
        }
      }
      if (!link && (!rlayer || sock_index >= NUM_LEGACY_SOCKETS)) {
        MEM_freeN(reinterpret_cast<NodeImageLayer *>(sock->storage));
        blender::bke::node_remove_socket(*ntree, *node, *sock);
      }
      else {
        blender::bke::node_set_socket_availability(*ntree, *sock, false);
      }
    }
  }

  BLI_linklist_free(available_sockets.list, nullptr);
}

namespace blender::nodes::node_composite_image_cc {

/** Default declaration for contextless static declarations and when the image is not assigned. */
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
        .structure_type(StructureType::Dynamic)
        .available(output->is_available());
  }
  return b.add_output(eNodeSocketDatatype(output->type), output->name)
      .structure_type(StructureType::Dynamic)
      .available(output->is_available());
}

/* Declares the already existing outputs. This is done in cases where the passes can not be read
 * due to an invalid image to retain the links and give the user the opportunity to update the
 * image such that becomes valid again. */
static void declare_existing(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  for (const bNodeSocket &output : node->outputs) {
    declare_existing_output(b, &output);
  }
}

/* Declares an output that matches the type of the given pass. */
static void declare_pass(NodeDeclarationBuilder &b, const RenderPass &pass)
{
  switch (pass.channels) {
    case 1:
      b.add_output<decl::Float>(pass.name).structure_type(StructureType::Dynamic);
      return;
    case 2:
      b.add_output<decl::Vector>(pass.name).dimensions(2).structure_type(StructureType::Dynamic);
      return;
    case 3:
      if (STR_ELEM(pass.chan_id, "RGB", "rgb")) {
        b.add_output<decl::Color>(pass.name).structure_type(StructureType::Dynamic);
        return;
      }
      b.add_output<decl::Vector>(pass.name).dimensions(3).structure_type(StructureType::Dynamic);
      return;
    case 4:
      if (STR_ELEM(pass.chan_id, "RGBA", "rgba")) {
        b.add_output<decl::Color>(pass.name).structure_type(StructureType::Dynamic);
        return;
      }
      b.add_output<decl::Vector>(pass.name).dimensions(4).structure_type(StructureType::Dynamic);
      return;
  }

  BLI_assert_unreachable();
}

static void node_declare_multi_layer(NodeDeclarationBuilder &b,
                                     Image *image,
                                     const ImageUser *image_user)
{
  RenderResult *render_result = BKE_image_acquire_renderresult(nullptr, image);
  BLI_SCOPED_DEFER([&]() { BKE_image_release_renderresult(nullptr, image, render_result); });

  if (!render_result) {
    declare_existing(b);
    return;
  }

  RenderLayer *render_layer = static_cast<RenderLayer *>(
      BLI_findlink(&render_result->layers, image_user->layer));
  if (!render_layer) {
    declare_existing(b);
    return;
  }

  bool has_alpha_pass = false;
  for (RenderPass &pass : render_layer->passes) {
    if (StringRef(pass.name) == "Alpha") {
      has_alpha_pass = true;
      break;
    }
  }

  for (RenderPass &pass : render_layer->passes) {
    declare_pass(b, pass);

    /* If the image does not have an alpha pass add an extra alpha pass that is generated based on
     * the combined pass, if the combined pass is an RGBA pass. */
    if (!has_alpha_pass && StringRef(pass.name) == RE_PASSNAME_COMBINED && pass.channels == 4 &&
        StringRef(pass.chan_id) == "RGBA")
    {
      b.add_output<decl::Float>("Alpha").structure_type(StructureType::Dynamic);
    }
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

  /* Avoid unnecessary updates, only changes to the Image/Image User data are of interest. */
  if (!(node->runtime->update & NODE_UPDATE_ID)) {
    declare_existing(b);
    return;
  }

  prepare_image(image, image_user);

  if (!BKE_image_is_multilayer(image)) {
    declare_single_layer(b);
    return;
  }

  node_declare_multi_layer(b, image, image_user);
}

static void node_init(bNodeTree * /*node_tree*/, bNode *node)
{
  node->flag |= NODE_PREVIEW;

  ImageUser *iuser = MEM_new<ImageUser>(__func__);
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
    for (const bNodeSocket *output : this->node().output_sockets()) {
      if (!is_socket_available(output)) {
        continue;
      }

      this->compute_output(output->identifier);
    }
  }

  void compute_output(StringRef identifier)
  {
    Result &result = this->get_result(identifier);
    if (!result.should_compute()) {
      return;
    }

    if (!this->get_image() || !this->get_image_user()) {
      result.allocate_invalid();
      return;
    }

    if (identifier == "Alpha") {
      this->compute_alpha();
      return;
    }

    Result cached_image = this->context().cache_manager().cached_images.get(
        this->context(), this->get_image(), this->get_image_user(), identifier.data());
    if (!cached_image.is_allocated()) {
      result.allocate_invalid();
      return;
    }

    result.set_type(cached_image.type());
    result.set_precision(cached_image.precision());
    result.wrap_external(cached_image);
  }

  void compute_alpha()
  {
    Result &result = this->get_result("Alpha");
    Result cached_alpha = this->context().cache_manager().cached_images.get(
        this->context(), this->get_image(), this->get_image_user(), "Alpha");

    /* For single layer images, the returned cached alpha is actually just the image, and we just
     * extract the alpha from it. */
    if (!BKE_image_is_multilayer(this->get_image())) {
      if (!cached_alpha.is_allocated()) {
        result.allocate_invalid();
        return;
      }

      extract_alpha(this->context(), cached_alpha, result);
      return;
    }

    /* For multi-layer images, if the returned cached alpha is allocated, that means that an actual
     * pass called Alpha exists, and we just return it as is. */
    if (cached_alpha.is_allocated()) {
      result.set_type(cached_alpha.type());
      result.set_precision(cached_alpha.precision());
      result.wrap_external(cached_alpha);
      return;
    }

    /* Otherwise, we try to extract the alpha from the combined pass if it exists. */
    Result cached_combined_image = this->context().cache_manager().cached_images.get(
        this->context(), this->get_image(), this->get_image_user(), RE_PASSNAME_COMBINED);
    if (!cached_combined_image.is_allocated()) {
      result.allocate_invalid();
      return;
    }
    extract_alpha(this->context(), cached_combined_image, result);
  }

  Image *get_image()
  {
    return reinterpret_cast<Image *>(node().id);
  }

  ImageUser *get_image_user()
  {
    return static_cast<ImageUser *>(node().storage);
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new ImageOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeImage", CMP_NODE_IMAGE);
  ntype.ui_name = "Image";
  ntype.ui_description = "Input image or movie file";
  ntype.enum_name_legacy = "IMAGE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  bke::node_type_storage(
      ntype, "ImageUser", node_free_standard_storage, node_copy_standard_storage);
  ntype.get_compositor_operation = get_compositor_operation;
  ntype.labelfunc = node_image_label;
  ntype.flag |= NODE_PREVIEW;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_image_cc
