/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_array.hh"
#include "BLI_generic_pointer.hh"

struct bNodeTree;
struct bNodeSocket;

namespace blender::nodes::socket_usage_inference {

void infer_inputs_socket_usage(const bNodeTree &tree,
                               Span<GPointer> tree_input_values,
                               MutableSpan<bool> r_input_usages);

void infer_inputs_socket_usage(const bNodeTree &tree,
                               Span<const bNodeSocket *> input_sockets,
                               MutableSpan<bool> r_input_usages);

}  // namespace blender::nodes::socket_usage_inference
