/* SPDX-FileCopyrightText: 2024 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * C wrapper for FontForge's overlap removal.
 */

#include "fontforge-config.h"
#include "splineoverlap_blender.h"
#include "splinechar.h"
#include "splinefont.h"
#include "splineoverlap.h"
#include "splineutil2.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Debug output for splinesets - disabled for production use */
/* #define FF_DUMP_SPLINESETS */

/* Epsilon for comparing control points to detect line segments */
#define LINE_CP_EPSILON 1e-6

/* Use FontForge's functions for proper initialization */
extern SplinePoint *SplinePointCreate(double x, double y);
extern Spline *SplineMake3(SplinePoint *from, SplinePoint *to);
extern void SplinePointListFree(SplinePointList *spl);
extern void SplinePointListsFree(SplinePointList *spl);
extern void SPLCategorizePoints(SplinePointList *spl);

/* ============================================================================
 * Direct SplineSet API Implementation
 * ============================================================================
 */

FFSplinePoint *FF_CreatePoint(double x, double y)
{
  return (FFSplinePoint *)SplinePointCreate(x, y);
}

void FF_SetPointNextCP(FFSplinePoint *sp, double x, double y)
{
  SplinePoint *p = (SplinePoint *)sp;
  p->nextcp.x = x;
  p->nextcp.y = y;
  p->nonextcp = (fabs(x - p->me.x) < LINE_CP_EPSILON && fabs(y - p->me.y) < LINE_CP_EPSILON) ? 1 : 0;
}

void FF_SetPointPrevCP(FFSplinePoint *sp, double x, double y)
{
  SplinePoint *p = (SplinePoint *)sp;
  p->prevcp.x = x;
  p->prevcp.y = y;
  p->noprevcp = (fabs(x - p->me.x) < LINE_CP_EPSILON && fabs(y - p->me.y) < LINE_CP_EPSILON) ? 1 : 0;
}

int FF_ConnectPoints(FFSplinePoint *from, FFSplinePoint *to)
{
  SplinePoint *fp = (SplinePoint *)from;
  SplinePoint *tp = (SplinePoint *)to;

  Spline *s = SplineMake3(fp, tp);

  return s != NULL ? 1 : 0;
}

FFSplineSet *FF_CreateSplineSet(FFSplinePoint *first, FFSplinePoint *last)
{
  SplineSet *ss = (SplineSet *)calloc(1, sizeof(SplineSet));
  if (ss == NULL) {
    return NULL;
  }
  ss->first = (SplinePoint *)first;
  ss->last = (SplinePoint *)last;
  return (FFSplineSet *)ss;
}

void FF_AppendSplineSet(FFSplineSet **head, FFSplineSet **tail, FFSplineSet *ss)
{
  if (*head == NULL) {
    *head = ss;
    *tail = ss;
  }
  else {
    ((SplineSet *)*tail)->next = (SplineSet *)ss;
    *tail = ss;
  }
}

static int count_splinesets(SplineSet *ss)
{
  int count = 0;
  for (SplineSet *s = ss; s != NULL; s = s->next) {
    count++;
  }
  return count;
}

/* Debug function - enable with #define FF_DUMP_SPLINESETS */
#ifdef FF_DUMP_SPLINESETS
static void DumpSplineSet(SplineSet *spl, const char *label)
{
  fprintf(stderr, "\n=== %s ===\n", label);
  int contour_idx = 0;
  for (SplineSet *ss = spl; ss != NULL; ss = ss->next) {
    int num_splines = FF_SplineSetCountSplines((FFSplineSet *)ss);
    int is_cyclic = FF_SplineSetIsCyclic((FFSplineSet *)ss);
    fprintf(stderr, "Contour %d: %d splines, cyclic=%d\n", contour_idx, num_splines, is_cyclic);

    SplinePoint *sp = ss->first;
    SplinePoint *first = sp;
    int pt_idx = 0;
    do {
      if (sp == NULL) break;
      fprintf(stderr, "  Pt %d: me=(%.2f,%.2f) prevcp=(%.2f,%.2f) nextcp=(%.2f,%.2f) noprev=%d nonext=%d pointtype=%d\n",
          pt_idx, sp->me.x, sp->me.y, sp->prevcp.x, sp->prevcp.y,
          sp->nextcp.x, sp->nextcp.y, sp->noprevcp, sp->nonextcp, sp->pointtype);
      if (sp->next != NULL) {
        Spline *s = sp->next;
        fprintf(stderr, "    Spline: order2=%d islinear=%d knownlinear=%d a=(%.4f,%.4f) b=(%.4f,%.4f)\n",
            s->order2, s->islinear, s->knownlinear, s->splines[0].a, s->splines[1].a, s->splines[0].b, s->splines[1].b);
      }
      if (sp->next == NULL) break;
      sp = sp->next->to;
      pt_idx++;
    } while (sp != first && pt_idx < 50);
    contour_idx++;
  }
  fprintf(stderr, "=== End %s ===\n\n", label);
}
#else
#define DumpSplineSet(spl, label) ((void)0)
#endif

FFSplineSet *FF_RemoveOverlap(FFSplineSet *ss)
{
  if (ss == NULL) {
    return NULL;
  }

  SplineSet *spl = (SplineSet *)ss;

  int input_count = count_splinesets(spl);

  DumpSplineSet(spl, "Input to FF_RemoveOverlap");

  /* Categorize points - required for overlap removal algorithm. */
  for (SplineSet *s = spl; s != NULL; s = s->next) {
    SPLCategorizePoints(s);
  }

  /* Create a minimal SplineChar for SplineSetRemoveOverlap.
   * Only sc->name is currently accessed (for debug messages).
   * The layers allocation is kept for correctness even though not currently used. */
  SplineChar sc = {0};
  sc.name = "blender_glyph";
  sc.layers = (Layer *)calloc(2, sizeof(Layer));
  if (sc.layers != NULL) {
    sc.layer_cnt = 2;
    sc.layers[1].splines = spl;
  }

  /* Call FontForge's overlap removal */
  SplineSet *result = SplineSetRemoveOverlap(&sc, spl, over_remove);

#ifdef FF_DUMP_SPLINESETS
  int output_count = count_splinesets(result);
  fprintf(stderr, "[FF_RemoveOverlap] SplineSet count: %d -> %d\n", input_count, output_count);
  int ss_idx = 0;
  for (SplineSet *ss = result; ss != NULL; ss = ss->next) {
    fprintf(stderr, "  Output SS %d: first=%p last=%p first==last=%d\n",
            ss_idx, (void*)ss->first, (void*)ss->last, ss->first == ss->last);
    if (ss->first != NULL && ss->last != NULL) {
      fprintf(stderr, "    first(%.1f,%.1f) last(%.1f,%.1f)\n",
              ss->first->me.x, ss->first->me.y, ss->last->me.x, ss->last->me.y);
      fprintf(stderr, "    first->prev=%p last->next=%p\n",
              (void*)ss->first->prev, (void*)ss->last->next);
    }
    ss_idx++;
  }
#else
  (void)input_count;
#endif

  free(sc.layers);

  return (FFSplineSet *)result;
}

FFSplineSet *FF_SplineSetGetNext(FFSplineSet *ss)
{
  if (ss == NULL) {
    return NULL;
  }
  return (FFSplineSet *)((SplineSet *)ss)->next;
}

int FF_SplineSetIsCyclic(FFSplineSet *ss)
{
  if (ss == NULL) {
    return 0;
  }
  SplineSet *spl = (SplineSet *)ss;

  /* A contour is cyclic if first == last */
  if (spl->first == spl->last) {
    return 1;
  }
  /* Or if last->next connects back to first */
  if (spl->first != NULL && spl->last != NULL && spl->last->next != NULL &&
      spl->last->next->to == spl->first) {
    return 1;
  }
  /* Or if first->prev connects from last (another common representation) */
  if (spl->first != NULL && spl->last != NULL && spl->first->prev != NULL &&
      spl->first->prev->from == spl->last) {
    return 1;
  }
#ifdef FF_DUMP_SPLINESETS
  /* Debug: print why not cyclic */
  fprintf(stderr, "[FF_SplineSetIsCyclic] NOT CYCLIC: first=%p last=%p first==last=%d\n",
          (void*)spl->first, (void*)spl->last, spl->first == spl->last);
  if (spl->first != NULL && spl->last != NULL) {
    fprintf(stderr, "  first coords: (%.1f, %.1f)  last coords: (%.1f, %.1f)\n",
            spl->first->me.x, spl->first->me.y, spl->last->me.x, spl->last->me.y);
  }
  if (spl->last != NULL) {
    fprintf(stderr, "  last->next=%p", (void*)spl->last->next);
    if (spl->last->next != NULL) {
      fprintf(stderr, " last->next->to=%p (first=%p)", (void*)spl->last->next->to, (void*)spl->first);
    }
    fprintf(stderr, "\n");
  }
  if (spl->first != NULL) {
    fprintf(stderr, "  first->prev=%p", (void*)spl->first->prev);
    if (spl->first->prev != NULL) {
      fprintf(stderr, " first->prev->from=%p (last=%p)", (void*)spl->first->prev->from, (void*)spl->last);
    }
    fprintf(stderr, "\n");
  }
#endif
  return 0;
}

int FF_SplineSetCountSplines(FFSplineSet *ss)
{
  if (ss == NULL) {
    return 0;
  }
  SplineSet *spl = (SplineSet *)ss;
  SplinePoint *sp = spl->first;
  if (sp == NULL) {
    return 0;
  }

  int count = 0;
  SplinePoint *first = sp;
  do {
    if (sp->next != NULL) {
      count++;
      sp = sp->next->to;
    }
    else {
      break;
    }
  } while (sp != NULL && sp != first);

  return count;
}

FFSplinePoint *FF_SplineSetGetFirst(FFSplineSet *ss)
{
  if (ss == NULL) {
    return NULL;
  }
  return (FFSplinePoint *)((SplineSet *)ss)->first;
}

FFSplinePoint *FF_PointGetNext(FFSplinePoint *sp, FFSplinePoint *first)
{
  if (sp == NULL) {
    return NULL;
  }
  SplinePoint *p = (SplinePoint *)sp;
  if (p->next == NULL) {
    return NULL;
  }
  SplinePoint *next = p->next->to;
  if (next == (SplinePoint *)first) {
    return NULL; /* Wrapped around */
  }
  return (FFSplinePoint *)next;
}

double FF_PointGetX(FFSplinePoint *sp)
{
  return ((SplinePoint *)sp)->me.x;
}

double FF_PointGetY(FFSplinePoint *sp)
{
  return ((SplinePoint *)sp)->me.y;
}

double FF_PointGetNextCPX(FFSplinePoint *sp)
{
  return ((SplinePoint *)sp)->nextcp.x;
}

double FF_PointGetNextCPY(FFSplinePoint *sp)
{
  return ((SplinePoint *)sp)->nextcp.y;
}

double FF_PointGetPrevCPX(FFSplinePoint *sp)
{
  return ((SplinePoint *)sp)->prevcp.x;
}

double FF_PointGetPrevCPY(FFSplinePoint *sp)
{
  return ((SplinePoint *)sp)->prevcp.y;
}

/* FontForge's SplineSetReverse function */
extern SplineSet *SplineSetReverse(SplineSet *spl);

void FF_SplineSetReverse(FFSplineSet *ss)
{
  if (ss != NULL) {
    SplineSetReverse((SplineSet *)ss);
  }
}

void FF_FreeSplineSets(FFSplineSet *ss)
{
  SplinePointListsFree((SplineSet *)ss);
}

