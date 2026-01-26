/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"

namespace blender {

namespace bke {
struct DynamicOverrideRuntime;
}  // namespace bke

struct DynamicOverride {
#ifdef __cplusplus
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_OV;
#endif

  ID id;
  /** Animation data (must be immediately after id for utilities to use it). */
  struct AnimData *adt = nullptr;

  bke::DynamicOverrideRuntime *runtime = nullptr;
  void *_pad2 = nullptr;
};

}  // namespace blender
