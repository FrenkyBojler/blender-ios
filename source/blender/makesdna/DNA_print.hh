/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>
#include <iosfwd>

/* for SDNA_TYPE_FROM_STRUCT() macro */
#include "dna_type_offsets.h"

struct SDNA;
struct SDNA_Struct;

void DNA_print_structs_at_address(const SDNA &sdna,
                                  int struct_id,
                                  const void *data,
                                  const void *address,
                                  int64_t element_num,
                                  std::ostream &stream);

void DNA_print_struct_by_id(int struct_id, const void *data);

#define DNA_print_struct(struct_name, data_ptr) \
  DNA_print_struct_by_id(SDNA_TYPE_FROM_STRUCT(struct_name), data_ptr)
