/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "COM_context.hh"

namespace blender::compositor {

/* ------------------------------------------------------------------------------------------------
 * Evaluator
 *
 * TODO. */
class Evaluator {
 private:
  /* A reference to the compositor context. */
  Context &context_;

 public:
  /* Construct an evaluator from a context. */
  Evaluator(Context &context);

  /* Evaluates the compositor node tree by compiling it into an operations stream and evaluating
   * it. */
  void evaluate();
};

}  // namespace blender::compositor
