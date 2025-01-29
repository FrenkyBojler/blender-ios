/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 */

#include "DNA_modifier_types.h"

#pragma once

struct ModifierData;

namespace blender::deg {

class ModifierDataBackup {
 public:
  explicit ModifierDataBackup(ModifierData *modifier_data);

  ModifierType type;
  void *runtime;
};

}  // namespace blender::deg
