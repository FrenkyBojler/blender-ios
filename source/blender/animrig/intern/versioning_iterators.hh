/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 */

#include "BLI_function_ref.hh"
#include "BLI_string_ref.hh"

#include "BKE_action.hh"

struct ID;
struct bAction;

namespace blender::animrig::versioning {

/**
 * This is basically a copy of #blender::animrig::foreach_action_slot_use_with_references(), but
 * then expanded to also track the RNA path of the properties. This is necessary for the creation
 * of library override rules.
 */
void foreach_action_slot_with_rna_path(
    ID &animated_id,
    FunctionRef<void(ID &animated_id,
                     bAction *&action_ptr_ref,
                     slot_handle_t &slot_handle_ref,
                     char *slot_name,
                     StringRefNull rna_path_to_slot_handle_prop)> callback);

}  // namespace blender::animrig::versioning
