/* SPDX-FileCopyrightText: 2007 by Janne Karhu. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */
#include "DNA_brush_types.h"
#include "DNA_scene_types.h"
#pragma once

struct bContext;
struct Depsgraph;
struct Main;
struct Object;
struct ReportList;
struct Scene;
struct wmKeyConfig;

/* `particle_edit.cc` */

bool ED_object_particle_edit_mode_supported(const Object *ob);
void ED_object_particle_edit_mode_enter_ex(bContext *C,
                                           Depsgraph *depsgraph,
                                           Scene *scene,
                                           Object *ob);
int pe_brush_size_get(const Scene *scene, const ParticleBrushData *brush);
void pe_brush_size_set(const Scene *scene, ParticleBrushData *brush, int value);

void ED_object_particle_edit_mode_enter(bContext *C);

void ED_object_particle_edit_mode_exit_ex(Scene *scene, Object *ob);
void ED_object_particle_edit_mode_exit(bContext *C);

bool PE_poll(bContext *C);
bool PE_hair_poll(bContext *C);
bool PE_poll_view3d(bContext *C);

/* `rigidbody_object.cc` */

bool ED_rigidbody_object_add(Main *bmain, Scene *scene, Object *ob, int type, ReportList *reports);
void ED_rigidbody_object_remove(Main *bmain, Scene *scene, Object *ob);

/* `rigidbody_constraint.cc` */

bool ED_rigidbody_constraint_add(
    Main *bmain, Scene *scene, Object *ob, int type, ReportList *reports);
void ED_rigidbody_constraint_remove(Main *bmain, Scene *scene, Object *ob);

/* operators */
void ED_operatortypes_physics();
void ED_keymap_physics(wmKeyConfig *keyconf);
