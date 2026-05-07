/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_animsys.h"
#include "BKE_idprop.hh"
#include "BLI_string_utf8.h"
#include "DNA_node_tree_interface_types.h"
#include "NOD_nodes_srna.hh"

namespace blender::nodes {

void update_properties_from_changed_socket_identifiers(
    Main &bmain,
    ID &id,
    IDProperty &group,
    const Span<const bNodeTreeInterfaceSocket *> io_sockets,
    const StringRef root_rna_path)
{
  Vector<AnimationBasePathChange> animation_changes;
  for (const bNodeTreeInterfaceSocket *io_socket : io_sockets) {
    for (const int i : IndexRange(io_socket->old_identifiers_num)) {
      const StringRef old_identifier = io_socket->old_identifiers[i];
      if (IDProperty *prop = IDP_GetPropertyFromGroup(&group, old_identifier)) {
        /* Reinsert in group with new identifier. */
        IDP_RemoveFromGroup(&group, prop);
        STRNCPY_UTF8(prop->name, io_socket->identifier);
        IDP_AddToGroup(&group, prop);

        animation_changes.append({
            .src_basepath = fmt::format("{}.{}", root_rna_path, old_identifier),
            .dst_basepath = fmt::format("{}.{}", root_rna_path, io_socket->identifier),
        });
      }
    }
  }
  if (animation_changes.is_empty()) {
    return;
  }
  BKE_animdata_copy_by_basepath(bmain, id, id, animation_changes);
  for (const AnimationBasePathChange &change : animation_changes) {
    BKE_animdata_fix_paths_remove(&id, change.src_basepath.c_str());
  }
}

}  // namespace blender::nodes
