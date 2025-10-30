/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <iosfwd>
#include <string>

#include "BLI_vector.hh"

void rna_write_struct_forward_declarations(std::ostringstream &buffer,
                                           blender::Vector<std::string> structs);
