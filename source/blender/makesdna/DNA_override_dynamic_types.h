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
struct DynOverrideRuntime;
}  // namespace bke

struct DynOverride {
#ifdef __cplusplus
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_OV;
#endif

  ID id;

  bke::DynOverrideRuntime *runtime = nullptr;
  void *_pad2 = nullptr;
};

}  // namespace blender
