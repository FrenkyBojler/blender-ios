/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <string>

#include "BLI_function_ref.hh"
#include "BLI_utility_mixins.hh"

struct bContext;

namespace blender::blocking_work {

void run(FunctionRef<void()> fn);

class StatusScope : NonCopyable, NonMovable {
 public:
  StatusScope(std::string message);
  ~StatusScope();
};

void set_global_context(bContext &C);
void exit_worker_thread();

}  // namespace blender::blocking_work
