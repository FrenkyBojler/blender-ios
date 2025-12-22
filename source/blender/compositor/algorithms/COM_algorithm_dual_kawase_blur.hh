/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

namespace blender::compositor {

class Context;
class Result;

void dual_kawase_blur(Context &context, const Result &input, Result &output, float radius);

}  // namespace blender::compositor
