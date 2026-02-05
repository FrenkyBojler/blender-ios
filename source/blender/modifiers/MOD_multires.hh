/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

namespace blender {
struct MultiresModifierRuntime {
  std::optional<int> previous_level;
};
}  // namespace blender