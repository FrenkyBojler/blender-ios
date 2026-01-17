/* -*- coding: utf-8 -*- */
/* Copyright (C) 2013 by Ben Martin */
/*
 * 
 * FontForge is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * FontForge is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with FontForge.  If not, see <http://www.gnu.org/licenses/>.
 * 
 * For more details see the COPYING file in the root directory of this
 * distribution.
 */

#ifndef FONTFORGE_FFGLIB_H
#define FONTFORGE_FFGLIB_H

#ifdef BLENDER
/* Stub GLib types for Blender - we don't need GLib functionality */
typedef void* gpointer;
typedef int gint;
typedef unsigned int guint;
typedef char gchar;
typedef int gboolean;
typedef struct _GHashTable GHashTable;
typedef struct _GList_Glib GList_Glib;
typedef int GPid;
#define g_ascii_strtod strtod
#define g_free free
#define g_malloc malloc
#define g_new(type, n) ((type*)malloc(sizeof(type) * (n)))
#define g_new0(type, n) ((type*)calloc((n), sizeof(type)))
#define TRUE 1
#define FALSE 0

/* Stub UI types for Blender - we don't need UI functionality */
/* Note: GImage, GRect, GPoint, Color, unichar_t are defined in other FontForge headers */
typedef void* GWindow;
typedef void* GGadget;
typedef void* GFont;
typedef void* GEvent;
typedef void* GTimer;
typedef void* GIC;
typedef void* GMenuItem;
typedef void* GMenuItem2;
typedef void* GTextInfo;
typedef void* GDraw;
typedef void* GCursor;
typedef void* GResFont;
typedef void* SearchData;
#else
#define GList  GList_Glib
#define GMenuItem GMenuItem_GIO
#define GMenu GMenu_GIO
#define GTimer GTimer_GTK

#ifdef __GNU__
# undef extended
#endif

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>
#include <glib/gstdio.h>

#ifdef __GNU__
# define extended	double
#endif

#undef GList
#undef GMenuItem
#undef GTimer
#endif /* BLENDER */

#endif /* FONTFORGE_FFGLIB_H */
