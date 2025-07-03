/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_sdna_types.h"

namespace blender::dna::pointers {

struct PointerInfo {
  int64_t offset;
  const char *member_type_name = nullptr;
  const char *name = nullptr;
};

struct StructInfo {
  Vector<PointerInfo> pointers;
  int size = 0;
};

class PointersInDNA {
 private:
  const SDNA &sdna_;
  Vector<StructInfo> structs_;

 public:
  explicit PointersInDNA(const SDNA &sdna);

  const StructInfo &get_for_struct(const int struct_nr) const
  {
    return structs_[struct_nr];
  }

 private:
  void gather_pointer_members_recursive(const SDNA_Struct &sdna_struct,
                                        int initial_offset,
                                        StructInfo &r_struct_info) const;
};

}  // namespace blender::dna::pointers
