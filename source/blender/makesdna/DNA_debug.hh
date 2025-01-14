/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

// Include std::ostream as forward declaration:
#include <iosfwd>

struct SDNA;
struct SDNA_Struct;

void DNA_struct_debug_print(const SDNA &sdna,
                            const SDNA_Struct &sdna_struct,
                            const void *data,
                            const void *address,
                            int indent,
                            std::ostream &stream);
