/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_ustring.hh"

#include "FN_multi_function.hh"

namespace blender::fn::multi_function::registry {

void register_function(UString id, const MultiFunction &fn);

const MultiFunction &lookup(UString id);

}  // namespace blender::fn::multi_function::registry
