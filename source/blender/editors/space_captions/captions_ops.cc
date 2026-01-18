/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spcaptions
 */

#include <cstdlib>

#include "DNA_space_types.h"
#include "WM_api.hh"
#include "captions_intern.hh"


namespace blender {

/* ************************** registration **********************************/
  void captions_operatortypes()
  {
      /* `captions_edit.cc` */
        WM_operatortype_append(CAPTIONS_OT_caption_add);
  }
  
} // namespace blender
