/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 */

#include "ANIM_action.hh"
#include "ANIM_action_iterators.hh"

#include "DNA_action_types.h"
#include "DNA_constraint_types.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include <fmt/format.h>

namespace blender::animrig::versioning {

/* For-each-action-slot-use that tracks the RNA path to the property using the action/slot. */

using NlaStripCallback =
    blender::FunctionRef<void(NlaStrip *, blender::StringRefNull strip_rna_path)>;

/**
 * Report the visited strip, and recurse into sub-strips.
 * This cannot be a lambda because it calls itself recursively.
 */
static void visit_strip(NlaStrip *strip,
                        blender::StringRefNull parent_rna_path,
                        NlaStripCallback callback)
{
  /* Construct the strip's RNA path. */
  char strip_name_esc[sizeof(strip->name) * 2];
  BLI_str_escape(strip_name_esc, strip->name, sizeof(strip_name_esc));
  const std::string strip_rna_path = fmt::format(
      "{}.strips[\"{}\"]", parent_rna_path, strip_name_esc);

  callback(strip, strip_rna_path);

  /* Recurse into sub-strips. */
  LISTBASE_FOREACH (NlaStrip *, sub_strip, &strip->strips) {
    visit_strip(sub_strip, strip_rna_path, callback);
  }
}

static void foreach_strip_adt(const AnimData &adt, NlaStripCallback callback)
{
  /* Loop over the tracks & its strips, and visit each strip. */
  LISTBASE_FOREACH (NlaTrack *, nlt, &adt.nla_tracks) {
    char track_name_esc[sizeof(nlt->name) * 2];
    BLI_str_escape(track_name_esc, nlt->name, sizeof(track_name_esc));
    const std::string track_rna_path = fmt::format("animation_data.nla_tracks[\"{}\"]",
                                                   track_name_esc);

    LISTBASE_FOREACH (NlaStrip *, strip, &nlt->strips) {
      visit_strip(strip, track_rna_path, callback);
    }
  }
}
/**
 * This is basically a copy of #blender::animrig::foreach_action_slot_use_with_references(), but
 * then expanded to also track the RNA path of the properties. This is necessary for the creation
 * of library override rules.
 */
void foreach_action_slot_with_rna_path(
    ID &animated_id,
    FunctionRef<void(ID &animated_id,
                     bAction *&action_ptr_ref,
                     blender::animrig::slot_handle_t &slot_handle_ref,
                     char *slot_name,
                     blender::StringRefNull rna_path_to_slot_handle_prop)> callback)
{
  AnimData *adt = BKE_animdata_from_id(&animated_id);

  if (adt) {
    if (adt->action) {
      /* Direct assignment. */
      callback(animated_id,
               adt->action,
               adt->slot_handle,
               adt->last_slot_identifier,
               "animation_data.action_slot_handle");
    }

    /* NLA strips. */
    foreach_strip_adt(*adt, [&](NlaStrip *strip, blender::StringRefNull strip_rna_path) {
      if (!strip->act) {
        return;
      }
      callback(animated_id,
               strip->act,
               strip->action_slot_handle,
               strip->last_slot_identifier,
               fmt::format("{}.{}", strip_rna_path, "action_slot_handle"));
    });
  }

  /* The rest of the code deals with constraints, so only relevant when this is an Object. */
  if (GS(animated_id.name) != ID_OB) {
    return;
  }

  const Object &object = reinterpret_cast<const Object &>(animated_id);

  /**
   * Visit a constraint, and call the callback if it's an Action constraint.
   */
  auto visit_constraint = [&](const bConstraint &constraint,
                              const blender::StringRefNull constraint_rna_path) {
    if (constraint.type != CONSTRAINT_TYPE_ACTION) {
      return;
    }
    bActionConstraint *constraint_data = static_cast<bActionConstraint *>(constraint.data);
    if (!constraint_data->act) {
      return;
    }
    callback(animated_id,
             constraint_data->act,
             constraint_data->action_slot_handle,
             constraint_data->last_slot_identifier,
             fmt::format("{}.{}", constraint_rna_path, "action_slot_handle"));
  };

  /* Visit Object constraints. */
  LISTBASE_FOREACH (bConstraint *, con, &object.constraints) {
    char constraint_name_esc[sizeof(con->name) * 2];
    BLI_str_escape(constraint_name_esc, con->name, sizeof(constraint_name_esc));
    const std::string constraint_rna_path = fmt::format("constraints[\"{}\"]",
                                                        constraint_name_esc);
    visit_constraint(*con, constraint_rna_path);
  }

  /* Visit Pose Bone constraints. */
  if (object.type == OB_ARMATURE) {
    LISTBASE_FOREACH (bPoseChannel *, pchan, &object.pose->chanbase) {
      char bone_name_esc[sizeof(pchan->name) * 2];
      BLI_str_escape(bone_name_esc, pchan->name, sizeof(bone_name_esc));

      LISTBASE_FOREACH (bConstraint *, con, &pchan->constraints) {
        char constraint_name_esc[sizeof(con->name) * 2];
        BLI_str_escape(constraint_name_esc, con->name, sizeof(constraint_name_esc));

        const std::string constraint_rna_path = fmt::format(
            "pose.bones[\"{}\"].constraints[\"{}\"]", bone_name_esc, constraint_name_esc);
        visit_constraint(*con, constraint_rna_path);
      }
    }
  }
}

}  // namespace blender::animrig::versioning
