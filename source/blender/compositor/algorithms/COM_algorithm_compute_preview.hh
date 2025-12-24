/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "COM_context.hh"
#include "COM_result.hh"

namespace blender::compositor {

/* Computes a lower resolution version of the given result and sets it as a preview for the given
 * node after applying the appropriate color management specified in the given context. */
void compute_preview(Context &context, const bNode &node, const Result &input_result);

}  // namespace blender::compositor
