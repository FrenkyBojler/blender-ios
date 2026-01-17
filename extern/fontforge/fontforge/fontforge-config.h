/* Minimal FontForge config for Blender's overlap removal.
 * This is NOT the full fontforge-config.h - only what's needed for splineoverlap.c */

#ifndef FONTFORGE_CONFIG_BLENDER_H
#define FONTFORGE_CONFIG_BLENDER_H

#define FONTFORGE_VERSION "20230101"

/* Use double precision for extended type */
#define FONTFORGE_CONFIG_USE_DOUBLE 1

/* Disable features not needed */
#define _NO_PYTHON 1
#define _NO_LIBSPIRO 1
#define _NO_FFSCRIPT 1
#define X_DISPLAY_MISSING 1

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

/* Include GLib stubs for Blender */
#ifdef BLENDER
#include "ffglib.h"
#endif

#endif /* FONTFORGE_CONFIG_BLENDER_H */
