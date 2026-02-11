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

#include "readfile.hh"

#include "versioning_common.hh"

// #include "CLG_log.h"

namespace blender {

// static CLG_LogRef LOG = {"blend.doversion"};

static void version_geometry_nodes_properties(NodesModifierData &nmd)
{
  if (nmd.modifier.system_properties) {
    return;
  }
  if (!nmd.node_group) {
    return;
  }
  NodesModifierSettings &settings = nmd.settings;
  const bNodeTree &ntree = *nmd.node_group;
  ntree.ensure_interface_cache();

  IDProperty *inputs = bke::idprop::create_group("inputs").release();
  for (const bNodeTreeInterfaceSocket *socket : ntree.interface_inputs()) {
    const StringRef identifier = socket->identifier;
    IDProperty *old_value_prop = IDP_GetPropertyFromGroup(nmd.settings.properties, identifier);
    if (!old_value_prop) {
      continue;
    }
    IDProperty *group = bke::idprop::create_group(identifier).release();
    IDP_AddToGroup(inputs, group);
    IDProperty *new_value_prop = IDP_CopyProperty(old_value_prop);
    STRNCPY(new_value_prop->name, "value");

    const IDProperty *use_attribute = IDP_GetPropertyFromGroup(nmd.settings.properties,
                                                               identifier + "_use_attribute");
    const auto input_type = IDP_bool_get(use_attribute) ?
                                nodes::GeometryNodesInputType::Attribute :
                                nodes::GeometryNodesInputType::Value;
    IDP_AddToGroup(group, bke::idprop::create("type", int(input_type)).release());

    IDProperty *outputs = bke::idprop::create_group("outputs").release();
    IDProperty *panels = bke::idprop::create_group("panels").release();
    for (IDProperty &idprop : settings.properties->data.group) {
      const StringRef name = idprop.name;
      if (name.endswith("_use_attribute")) {
        continue;
      }
      if (name.endswith("_attribute_name")) {
        continue;
      }
    }
    IDProperty *system_props = bke::idprop::create_group("NodesModifierProperties").release();
  }
}

void do_versions_after_linking_520(FileData * /*fd*/, Main * /*bmain*/)
{
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
