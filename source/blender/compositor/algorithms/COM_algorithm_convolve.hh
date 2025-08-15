/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "COM_context.hh"
#include "COM_result.hh"

namespace blender::compositor {

/* Convolves the given color input by the given kernel and write the result to the given output.
 * The output will be allocated internally and is thus expected not to be previously allocated. */
void convolve(Context &context, const Result &input, const Result &kernel, Result &output);

}  // namespace blender::compositor
