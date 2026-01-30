/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sptext
 */

#pragma once

#include "DNA_vec_types.h"
#include "DNA_captions_types.h" 

namespace blender {

    struct ARegion;
    struct ScrArea;
    struct SpaceCaptions;
    struct bContext;
    struct wmOperatorType;
    struct Scene;

    /* `captions_edit.cc` */

    void CAPTIONS_OT_caption_add(wmOperatorType *ot);

    /* `captions_ops.cc` */

    void captions_operatortypes();

    /* `space_captions.cc` */
    void tag_redraw(ARegion *region, Scene *scene);

    struct SpaceCaptions_Runtime {
    };
} // namespace blender
