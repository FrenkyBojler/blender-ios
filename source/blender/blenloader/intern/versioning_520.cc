/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_node_types.h"
#include "DNA_screen_types.h"

#include "BLI_listbase_iterator.hh"
#include "BLI_math_vector.h"
#include "BLI_sys_types.h"

#include "BKE_attribute.hh"
#include "BKE_main.hh"
#include "BKE_mesh_legacy_convert.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"

#include "SEQ_sequencer.hh"

#include "readfile.hh"

#include "versioning_common.hh"

// #include "CLG_log.h"

namespace blender {

// static CLG_LogRef LOG = {"blend.doversion"};

/* Saving file extension is now a property of the File Output node. So inherit this
 * setting from the active scene to restore the old behavior.
 * Note: One limitation is that node groups containing file outputs that are not part of any
 * scene are not affected by versioning. */
static void do_version_file_output_use_file_extension_recursive(bNodeTree &node_tree,
                                                                const Scene &scene)
{
  for (bNode &node : node_tree.nodes) {
    if (node.type_legacy == CMP_NODE_OUTPUT_FILE) {
      NodeCompositorFileOutput *data = static_cast<NodeCompositorFileOutput *>(node.storage);
      data->use_file_extension = (scene.r.scemode & R_EXTENSION) != 0;
    }
    else if (node.type_legacy == NODE_GROUP) {
      bNodeTree *ngroup = id_cast<bNodeTree *>(node.id);
      if (ngroup) {
        do_version_file_output_use_file_extension_recursive(*ngroup, scene);
      }
    }
  }
}

static void do_version_geometry_node_primitive_uvmaps(bNodeTree *node_tree)
{
  using bke::AttrDomain;

  /* Stores a mapping between an output and the final link of the versioning node tree that was
   * added for it, in order to share the same versioning node tree with potentially multiple
   * outgoing links from that same output. */
  Map<bNodeSocket *, bNodeLink *> links_done;

  for (bNodeLink &link : node_tree->links.items_reversed_mutable()) {
    if (!ELEM(link.fromnode->type_legacy,
              GEO_NODE_MESH_PRIMITIVE_UV_SPHERE,
              GEO_NODE_MESH_PRIMITIVE_CYLINDER))
    {
      continue;
    }
    if (!STREQ(link.fromsock->identifier, "UV Map")) {
      continue;
    }

    /* If that output was versioned before, just connect the existing link. */
    bNodeLink *existing_link = links_done.lookup_default(link.fromsock, nullptr);
    if (existing_link != nullptr) {
      version_node_add_link(*node_tree,
                            *existing_link->fromnode,
                            *existing_link->fromsock,
                            *link.tonode,
                            *link.tosock);
      bke::node_remove_link(node_tree, link);
      continue;
    }

    if (link.fromnode->type_legacy == GEO_NODE_MESH_PRIMITIVE_UV_SPHERE) {
      /* Multiply Add node. */
      bNode *multiply_node = bke::node_add_node(nullptr, *node_tree, "ShaderNodeVectorMath");
      multiply_node->parent = link.fromnode->parent;
      multiply_node->location[0] = link.fromnode->location[0] + link.fromnode->width + 20.0f;
      multiply_node->location[1] = link.fromnode->location[1];
      multiply_node->custom1 = NODE_VECTOR_MATH_MULTIPLY_ADD;
      bNodeSocket *multiply_a_input = bke::node_find_socket(*multiply_node, SOCK_IN, "Vector");
      bNodeSocket *multiply_b_input = bke::node_find_socket(*multiply_node, SOCK_IN, "Vector_001");
      bNodeSocket *multiply_c_input = bke::node_find_socket(*multiply_node, SOCK_IN, "Vector_002");
      bNodeSocket *multiply_output = bke::node_find_socket(*multiply_node, SOCK_OUT, "Vector");
      copy_v3_v3(static_cast<bNodeSocketValueVector *>(multiply_b_input->default_value)->value,
                 float3(1.0f, -1.0f, 0.0f));
      copy_v3_v3(static_cast<bNodeSocketValueVector *>(multiply_c_input->default_value)->value,
                 float3(0.0f, 1.0f, 0.0f));
      version_node_add_link(
          *node_tree, *link.fromnode, *link.fromsock, *multiply_node, *multiply_a_input);

      /* Wire up to original socket. */
      bNodeLink *new_link = &version_node_add_link(
          *node_tree, *multiply_node, *multiply_output, *link.tonode, *link.tosock);

      /* Add the new link to the cache. */
      links_done.add_new(link.fromsock, new_link);

      /* Remove the old link. */
      bke::node_remove_link(node_tree, link);
    }
    else if (link.fromnode->type_legacy == GEO_NODE_MESH_PRIMITIVE_CYLINDER) {

      /* Primitive node. */
      bNodeSocket *cylinder_side = bke::node_find_socket(*link.fromnode, SOCK_OUT, "Side");
      bNodeSocket *cylinder_uv = bke::node_find_socket(*link.fromnode, SOCK_OUT, "UV Map");

      /* Field Average node. */
      bNode *average_node = bke::node_add_node(nullptr, *node_tree, "GeometryNodeFieldAverage");
      average_node->parent = link.fromnode->parent;
      average_node->location[0] = link.fromnode->location[0] + link.fromnode->width + 20.0f;
      average_node->location[1] = link.fromnode->location[1];
      average_node->custom1 = CD_PROP_FLOAT3;
      average_node->custom2 = int16_t(AttrDomain::Corner);
      bNodeSocket *average_value = bke::node_find_socket(*average_node, SOCK_IN, "Value");
      bNodeSocket *average_group = bke::node_find_socket(*average_node, SOCK_IN, "Group Index");
      bNodeSocket *average_mean = bke::node_find_socket(*average_node, SOCK_OUT, "Mean");
      version_node_add_link(
          *node_tree, *link.fromnode, *cylinder_side, *average_node, *average_group);
      version_node_add_link(
          *node_tree, *link.fromnode, *cylinder_uv, *average_node, *average_value);

      /* Subtract node. */
      bNode *subtract_node = bke::node_add_node(nullptr, *node_tree, "ShaderNodeVectorMath");
      subtract_node->parent = link.fromnode->parent;
      subtract_node->location[0] = link.fromnode->location[0] + link.fromnode->width + 40.0f;
      subtract_node->location[1] = link.fromnode->location[1];
      subtract_node->custom1 = NODE_VECTOR_MATH_SUBTRACT;
      bNodeSocket *subtract_a_input = bke::node_find_socket(*subtract_node, SOCK_IN, "Vector");
      bNodeSocket *subtract_b_input = bke::node_find_socket(*subtract_node, SOCK_IN, "Vector_001");
      bNodeSocket *subtract_output = bke::node_find_socket(*subtract_node, SOCK_OUT, "Vector");
      version_node_add_link(
          *node_tree, *link.fromnode, *cylinder_uv, *subtract_node, *subtract_a_input);
      version_node_add_link(
          *node_tree, *average_node, *average_mean, *subtract_node, *subtract_b_input);

      /* Multiply node. */
      bNode *multiply_node = bke::node_add_node(nullptr, *node_tree, "ShaderNodeVectorMath");
      multiply_node->parent = link.fromnode->parent;
      multiply_node->location[0] = link.fromnode->location[0] + link.fromnode->width + 60.0f;
      multiply_node->location[1] = link.fromnode->location[1];
      multiply_node->custom1 = NODE_VECTOR_MATH_MULTIPLY;
      bNodeSocket *multiply_a_input = bke::node_find_socket(*multiply_node, SOCK_IN, "Vector");
      bNodeSocket *multiply_b_input = bke::node_find_socket(*multiply_node, SOCK_IN, "Vector_001");
      bNodeSocket *multiply_output = bke::node_find_socket(*multiply_node, SOCK_OUT, "Vector");
      copy_v3_v3(static_cast<bNodeSocketValueVector *>(multiply_b_input->default_value)->value,
                 float3(1.0f, -1.0f, 0.0f));
      version_node_add_link(
          *node_tree, *subtract_node, *subtract_output, *multiply_node, *multiply_a_input);

      /* Add node. */
      bNode *add_node = bke::node_add_node(nullptr, *node_tree, "ShaderNodeVectorMath");
      add_node->parent = link.fromnode->parent;
      add_node->location[0] = link.fromnode->location[0] + link.fromnode->width + 80.0f;
      add_node->location[1] = link.fromnode->location[1];
      add_node->custom1 = NODE_VECTOR_MATH_ADD;
      bNodeSocket *add_a_input = bke::node_find_socket(*add_node, SOCK_IN, "Vector");
      bNodeSocket *add_b_input = bke::node_find_socket(*add_node, SOCK_IN, "Vector_001");
      bNodeSocket *add_output = bke::node_find_socket(*add_node, SOCK_OUT, "Vector");
      version_node_add_link(*node_tree, *average_node, *average_mean, *add_node, *add_a_input);
      version_node_add_link(*node_tree, *multiply_node, *multiply_output, *add_node, *add_b_input);

      /* Switch node. */
      bNode *switch_node = bke::node_add_node(nullptr, *node_tree, "GeometryNodeSwitch");
      switch_node->parent = link.fromnode->parent;
      switch_node->location[0] = link.fromnode->location[0] + link.fromnode->width + 100.0f;
      switch_node->location[1] = link.fromnode->location[1];
      NodeSwitch *data = reinterpret_cast<NodeSwitch *>(switch_node->storage);
      data->input_type = SOCK_VECTOR;
      bNodeSocket *switch_condition = bke::node_find_socket(*switch_node, SOCK_IN, "Switch");
      bNodeSocket *switch_false = bke::node_find_socket(*switch_node, SOCK_IN, "False");
      bNodeSocket *switch_true = bke::node_find_socket(*switch_node, SOCK_IN, "True");
      bNodeSocket *switch_output = bke::node_find_socket(*switch_node, SOCK_OUT, "Output");
      version_node_add_link(*node_tree, *add_node, *add_output, *switch_node, *switch_true);
      version_node_add_link(*node_tree, *link.fromnode, *cylinder_uv, *switch_node, *switch_false);
      version_node_add_link(
          *node_tree, *link.fromnode, *cylinder_side, *switch_node, *switch_condition);

      /* Wire up to original socket. */
      bNodeLink *new_link = &version_node_add_link(
          *node_tree, *switch_node, *switch_output, *link.tonode, *link.tosock);

      /* Add the new link to the cache. */
      links_done.add_new(link.fromsock, new_link);

      /* Remove the old link. */
      bke::node_remove_link(node_tree, link);
    }
  }
}

void do_versions_after_linking_520(FileData * /*fd*/, Main *bmain)
{
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 2)) {
    for (Scene &scene : bmain->scenes) {
      bNodeTree *node_tree = version_get_scene_compositor_node_tree(bmain, &scene);
      if (node_tree == nullptr) {
        continue;
      }
      do_version_file_output_use_file_extension_recursive(*node_tree, scene);
    }
  }

  /* Restore old "UV Map" behavior of geometry nodes Cylinder and UV Sphere primitives. */
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 13)) {
    FOREACH_NODETREE_BEGIN (bmain, node_tree, id_owner) {
      if (node_tree->type == NTREE_GEOMETRY) {
        do_version_geometry_node_primitive_uvmaps(node_tree);
      }
    }
    FOREACH_NODETREE_END;
  }

  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

void blo_do_versions_520(FileData * /*fd*/, Library * /*lib*/, Main *bmain)
{
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 1)) {
    for (Scene &scene : bmain->scenes) {
      scene.r.mode |= R_SAVE_OUTPUT;
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 4)) {
    for (Brush &brush : bmain->brushes) {
      if (brush.gpencil_settings != nullptr) {
        brush.blend = 0;
      }
    }
  }
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 5)) {
    FOREACH_NODETREE_BEGIN (bmain, node_tree, id_owner) {
      for (bNode &node : node_tree->nodes) {
        if (node.type_legacy == FN_NODE_INPUT_VECTOR) {
          auto &data = *static_cast<NodeInputVector *>(node.storage);
          data.vector[3] = 0.0f;
          data.dimensions = 3;
        }
      }
    }
    FOREACH_NODETREE_END;
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 6)) {
    for (Scene &scene : bmain->scenes) {
      SequencerToolSettings *sequencer_tool_settings = seq::tool_settings_ensure(&scene);
      sequencer_tool_settings->snap_flag |= SEQ_SNAP_TO_ALL_CHANNEL_STRIPS;
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 7)) {
    for (Scene &scene : bmain->scenes) {
      scene.r.anisotropic_filter = 2;
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 9)) {
    for (Mesh &mesh : bmain->meshes) {
      bke::mesh_freestyle_marks_to_generic(mesh);
    }
  }

  /* Convert H.264 codec value for older files (2.79), see #155775. */
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 10)) {
    for (Scene &scene : bmain->scenes) {
      if (scene.r.ffcodecdata.codec == 28) {
        scene.r.ffcodecdata.codec = 27;
      }
    }
  }

  /* Disable "unified" flags for Grease Pencil Draw mode. */
  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 11)) {
    for (Scene &scene : bmain->scenes) {
      if (scene.toolsettings->gp_paint) {
        UnifiedPaintSettings &settings =
            scene.toolsettings->gp_paint->paint.unified_paint_settings;
        settings.flag &= ~(UNIFIED_PAINT_SIZE | UNIFIED_PAINT_ALPHA | UNIFIED_PAINT_COLOR);
      }
    }
  }

  if (!MAIN_VERSION_FILE_ATLEAST(bmain, 502, 12)) {
    for (bScreen &screen : bmain->screens) {
      for (ScrArea &area : screen.areabase) {
        for (SpaceLink &space : area.spacedata) {
          if (space.spacetype == SPACE_NODE) {
            SpaceNode *space_node = reinterpret_cast<SpaceNode *>(&space);
            space_node->overlay.flag |= SN_OVERLAY_SHOW_RENDER_REGION;
            space_node->overlay.passepartout_alpha = 0.5f;
          }
        }
      }
    }
  }

  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

}  // namespace blender
