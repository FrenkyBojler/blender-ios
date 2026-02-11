/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenloader
 */

#define DNA_DEPRECATED_ALLOW

#include "NOD_geometry_nodes_srna.hh"

#include "DNA_ID.h"
#include "DNA_modifier_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"
#include "BLI_sys_types.h"

#include "BKE_idprop.hh"
#include "BKE_main.hh"
#include "BKE_node_runtime.hh"

#include "readfile.hh"

#include "versioning_common.hh"

// #include "CLG_log.h"

namespace blender {

// static CLG_LogRef LOG = {"blend.doversion"};

static void version_geometry_nodes_properties(NodesModifierData &nmd)
{
  const IDProperty *old_props = nmd.settings.properties;
  if (!old_props) {
    return;
  }
  if (nmd.modifier.system_properties) {
    return;
  }
  if (!nmd.node_group) {
    return;
  }
  const bNodeTree &ntree = *nmd.node_group;
  ntree.ensure_interface_cache();

  IDProperty *system_props = bke::idprop::create_group("NodesModifierProperties").release();

  IDProperty *inputs = bke::idprop::create_group("inputs").release();
  IDP_AddToGroup(system_props, inputs);

  for (const bNodeTreeInterfaceSocket *input : ntree.interface_inputs()) {
    const StringRef identifier = input->identifier;
    IDProperty *old_value_prop = IDP_GetPropertyFromGroup(old_props, identifier);
    if (!old_value_prop) {
      continue;
    }

    IDProperty *group = bke::idprop::create_group(identifier).release();
    IDP_AddToGroup(inputs, group);

    if (input->flag & NODE_INTERFACE_SOCKET_LAYER_SELECTION) {
      IDP_AddToGroup(
          group, bke::idprop::create("type", int(nodes::GeometryNodesInputType::Layer)).release());
      const StringRefNull layer_name = [&]() {
        const IDProperty *layer_name = IDP_GetPropertyFromGroup(old_props, identifier);
        if (layer_name) {
          return StringRefNull(IDP_string_get(layer_name));
        }
        return StringRefNull();
      }();
      IDP_AddToGroup(group, bke::idprop::create("layer_name", layer_name).release());
      continue;
    }

    IDProperty *new_value_prop = IDP_CopyProperty(old_value_prop);
    STRNCPY(new_value_prop->name, "value");
    IDP_AddToGroup(group, new_value_prop);

    const bool use_attribute = [&]() {
      const IDProperty *use_attribute = IDP_GetPropertyFromGroup(old_props,
                                                                 identifier + "_use_attribute");
      if (!use_attribute) {
        return false;
      }
      if (use_attribute->type == IDP_INT) {
        return bool(IDP_int_get(use_attribute));
      }
      return bool(IDP_bool_get(use_attribute));
    }();

    const auto input_type = use_attribute ? nodes::GeometryNodesInputType::Attribute :
                                            nodes::GeometryNodesInputType::Value;
    IDP_AddToGroup(group, bke::idprop::create("type", int(input_type)).release());
    const StringRefNull attribute_name = [&]() {
      const IDProperty *attribute_name = IDP_GetPropertyFromGroup(old_props,
                                                                  identifier + "_attribute_name");
      if (attribute_name) {
        return StringRefNull(IDP_string_get(attribute_name));
      }
      return StringRefNull();
    }();
    IDP_AddToGroup(group, bke::idprop::create("attribute_name", attribute_name).release());
  }

  IDProperty *outputs = bke::idprop::create_group("outputs").release();
  IDP_AddToGroup(system_props, outputs);
  for (const bNodeTreeInterfaceSocket *output : ntree.interface_outputs()) {
    const StringRef identifier = output->identifier;
    IDProperty *old_name_prop = IDP_GetPropertyFromGroup(old_props,
                                                         identifier + "_attribute_name");
    if (!old_name_prop) {
      continue;
    }
    IDProperty *group = bke::idprop::create_group(identifier).release();
    IDP_AddToGroup(outputs, group);

    IDProperty *new_value_prop = IDP_CopyProperty(old_name_prop);
    STRNCPY(new_value_prop->name, "attribute_name");
    IDP_AddToGroup(group, new_value_prop);
  }

  nmd.modifier.system_properties = system_props;
  IDP_FreeProperty(nmd.settings.properties);
  nmd.settings.properties = nullptr;
}

void do_versions_after_linking_520(FileData * /*fd*/, Main *bmain)
{
  for (Object &object : bmain->objects) {
    for (ModifierData &md : object.modifiers) {
      if (md.type == eModifierType_Nodes) {
        version_geometry_nodes_properties(reinterpret_cast<NodesModifierData &>(md));
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

void blo_do_versions_520(FileData * /*fd*/, Library * /*lib*/, Main * /*bmain*/)
{
  /**
   * Always bump subversion in BKE_blender_version.h when adding versioning
   * code here, and wrap it inside a MAIN_VERSION_FILE_ATLEAST check.
   *
   * \note Keep this message at the bottom of the function.
   */
}

}  // namespace blender
