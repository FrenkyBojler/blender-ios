/* SPDX-FileCopyrightText: 2007 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#pragma once

#include <type_traits>

#include "MEM_guardedalloc.h"

#include "DNA_node_types.h"

#include "BKE_node.hh"

namespace blender {

struct bNode;
struct bNodeTree;
struct bContext;

/* data for initializing node execution */
struct bNodeExecContext {};

struct bNodeExecData {
  void *data; /* custom data storage */
};

/**** Storage Method Templates ****/

template<typename T> void node_free_storage(bNode *node)
{
  if (node->storage) {
    MEM_delete(static_cast<T *>(node->storage));
    node->storage = nullptr;
  }
}

template<typename T>
void node_copy_storage(bNodeTree * /*ntree*/, bNode *dest_node, const bNode *src_node)
{
  if (src_node->storage) {
    if constexpr (std::is_constructible_v<T, dna::internal::ShallowDataConstRef<T>>) {
      /* Can only do shallow copy when using DNA_DEFINE_CXX_METHODS. */
      dest_node->storage = MEM_new<T>(
          __func__, dna::shallow_copy(*static_cast<const T *>(src_node->storage)));
    }
    else {
      dest_node->storage = MEM_new<T>(__func__, *static_cast<const T *>(src_node->storage));
    }
  }
}

/*** Curves Storage ***/

void *node_initexec_curves(bNodeExecContext *context, bNode *node, bNodeInstanceKey key);
void node_copy_curves(bNodeTree *dest_ntree, bNode *dest_node, const bNode *src_node);
void node_free_curves(bNode *node);

/**** Updates ****/
void node_sock_label(bNodeSocket *sock, const char *name);
void node_sock_label_clear(bNodeSocket *sock);
void node_math_update(bNodeTree *ntree, bNode *node);

/**** Labels ****/
void node_blend_label(const bNodeTree *ntree, const bNode *node, char *label, int label_maxncpy);
void node_image_label(const bNodeTree *ntree, const bNode *node, char *label, int label_maxncpy);
void node_math_label(const bNodeTree *ntree, const bNode *node, char *label, int label_maxncpy);
void node_vector_math_label(const bNodeTree *ntree,
                            const bNode *node,
                            char *label,
                            int label_maxncpy);
void node_combsep_color_label(const ListBaseT<bNodeSocket> *sockets, NodeCombSepColorMode mode);

/*** Link Handling */

/**
 * By default there are no links we don't want to connect, when inserting.
 */
bool node_insert_link_default(bke::NodeInsertLinkParams &params);

int node_socket_get_int(bNodeTree *ntree, bNode *node, bNodeSocket *sock);
void node_socket_set_int(bNodeTree *ntree, bNode *node, bNodeSocket *sock, int value);
bool node_socket_get_bool(bNodeTree *ntree, bNode *node, bNodeSocket *sock);
void node_socket_set_bool(bNodeTree *ntree, bNode *node, bNodeSocket *sock, bool value);
float node_socket_get_float(bNodeTree *ntree, bNode *node, bNodeSocket *sock);
void node_socket_set_float(bNodeTree *ntree, bNode *node, bNodeSocket *sock, float value);
void node_socket_get_color(bNodeTree *ntree, bNode *node, bNodeSocket *sock, float *value);
void node_socket_set_color(bNodeTree *ntree, bNode *node, bNodeSocket *sock, const float *value);
void node_socket_get_vector(bNodeTree *ntree, bNode *node, bNodeSocket *sock, float *value);
void node_socket_set_vector(bNodeTree *ntree, bNode *node, bNodeSocket *sock, const float *value);

}  // namespace blender
