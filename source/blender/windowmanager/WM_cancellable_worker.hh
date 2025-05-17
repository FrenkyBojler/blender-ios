/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <atomic>

#include "BLI_function_ref.hh"
#include "BLI_utility_mixins.hh"

struct bContext;

namespace blender::cancellable_worker {

void set_global_context(bContext &C);

void run_cancellable_if_possible(FunctionRef<void()> fn);

void exit_worker_thread();

}  // namespace blender::cancellable_worker
