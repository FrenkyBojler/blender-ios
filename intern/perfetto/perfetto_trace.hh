/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_perfetto
 */

/* Because the perfetto header is relativly heavy we do not want to include it
   in every translation unit, this lightweight header that will wrap all functionality
   can be included instead. */
 
namespace blender {
    
void perfetto_init();
void perfetto_shutdown();

}