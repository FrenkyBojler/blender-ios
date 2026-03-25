/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_ustring.hh"

#include "FN_multi_function.hh"

namespace blender::fn::multi_function::registry {

void add_new(const MultiFunction &fn);

template<typename CreateFn> inline void add_new_cb(CreateFn &&create_fn)
{
  static auto fn = create_fn();
  registry::add_new(fn);
}

const MultiFunction &lookup(UString id);

}  // namespace blender::fn::multi_function::registry
