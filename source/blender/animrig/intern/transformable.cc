/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup animrig
 */

 #include "ANIM_transformable.hh"
 #include "RNA_access.hh"
 
 namespace blender::animrig{

    PropertyRNA &TransformablePoseBone::get_local_space() {
        PropertyRNA *prop = RNA_struct_find_property(&this->ptr_, "matrix_basis");
        return *prop;
    }

    PropertyRNA &TransformableObject::get_local_space() {
        PropertyRNA *prop = RNA_struct_find_property(&this->ptr_, "matrix_local");
        return *prop;
    }
    
 }