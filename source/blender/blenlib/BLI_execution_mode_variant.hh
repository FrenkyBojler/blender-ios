/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#pragma once

#include "BLI_execution_mode.hh"

#include <variant>

namespace blender {

/**
 * A version of #ExecutionMode that is not constexpr and can therefore be used in non-template
 * functions.
 */
using ExecutionModeVariant =
    std::variant<ExecuteParallel, ExecuteParallelGrainSize, ExecuteSerial>;

}  // namespace blender
