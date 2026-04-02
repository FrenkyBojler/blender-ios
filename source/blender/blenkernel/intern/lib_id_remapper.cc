/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_ID.h"

#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_remap.hh"

namespace blender::bke::id {

void IDRemapper::add(ID *old_id, ID *new_id)
{
  BLI_assert(old_id != nullptr);
  BLI_assert(new_id == nullptr || this->allow_idtype_mismatch ||
             (GS(old_id->name) == GS(new_id->name)));
  BLI_assert(BKE_idtype_idcode_to_idfilter(GS(old_id->name)) != 0);

  mappings_.add(old_id, new_id);
  source_types_ |= BKE_idtype_idcode_to_idfilter(GS(old_id->name));
}

void IDRemapper::add_overwrite(ID *old_id, ID *new_id)
{
  BLI_assert(old_id != nullptr);
  BLI_assert(new_id == nullptr || this->allow_idtype_mismatch ||
             (GS(old_id->name) == GS(new_id->name)));
  BLI_assert(BKE_idtype_idcode_to_idfilter(GS(old_id->name)) != 0);

  mappings_.add_overwrite(old_id, new_id);
  source_types_ |= BKE_idtype_idcode_to_idfilter(GS(old_id->name));
}

IDRemapperApplyResult IDRemapper::get_mapping_result(ID *id,
                                                     IDRemapperApplyOptions options,
                                                     const ID *id_self) const
{
  if (!mappings_.contains(id)) {
    return IdRemapResultSourceUnavailable;
  }
  const ID *new_id = mappings_.lookup(id);
  if ((options & IdRemapApplyUnmapWhenRemappingToSelf) != 0 && id_self == new_id) {
    new_id = nullptr;
  }
  if (new_id == nullptr) {
    return IdRemapResultSourceUnassigned;
  }

  return IdRemapResultSourceRemapped;
}

IDRemapperApplyResult IDRemapper::apply(ID **r_id_ptr,
                                        IDRemapperApplyOptions options,
                                        ID *id_self) const
{
  BLI_assert(r_id_ptr != nullptr);
  BLI_assert_msg(
      ((options & IdRemapApplyUnmapWhenRemappingToSelf) == 0 || id_self != nullptr),
      "ID_REMAP_APPLY_WHEN_REMAPPING_TO_SELF requires a non-null `id_self` parameter.");

  if (*r_id_ptr == nullptr) {
    return IdRemapResultSourceNotMappable;
  }

  ID *const *new_id = mappings_.lookup_ptr(*r_id_ptr);
  if (new_id == nullptr) {
    return IdRemapResultSourceUnavailable;
  }

  if (options & IdRemapApplyUpdateRefcount) {
    id_us_min(*r_id_ptr);
  }

  *r_id_ptr = *new_id;
  if (options & IdRemapApplyUnmapWhenRemappingToSelf && *r_id_ptr == id_self) {
    *r_id_ptr = nullptr;
  }
  if (*r_id_ptr == nullptr) {
    return IdRemapResultSourceUnassigned;
  }

  if (options & IdRemapApplyUpdateRefcount) {
    /* Do not handle ID_TAG_INDIRECT/ID_TAG_EXTERN here. */
    id_us_plus_no_lib(*r_id_ptr);
  }

  if (options & IdRemapApplyEnsureReal) {
    id_us_ensure_real(*r_id_ptr);
  }
  return IdRemapResultSourceRemapped;
}

StringRefNull IDRemapper::result_to_string(const IDRemapperApplyResult result)
{
  switch (result) {
    case IdRemapResultSourceNotMappable:
      return "not_mappable";
    case IdRemapResultSourceUnavailable:
      return "unavailable";
    case IdRemapResultSourceUnassigned:
      return "unassigned";
    case IdRemapResultSourceRemapped:
      return "remapped";
  }
  BLI_assert_unreachable();
  return "";
}

void IDRemapper::print() const
{
  this->iter([](ID *old_id, ID *new_id) {
    if (old_id != nullptr && new_id != nullptr) {
      printf("Remap %s(%p) to %s(%p)\n", old_id->name, old_id, new_id->name, new_id);
    }
    if (old_id != nullptr && new_id == nullptr) {
      printf("Unassign %s(%p)\n", old_id->name, old_id);
    }
  });
}

}  // namespace blender::bke::id
