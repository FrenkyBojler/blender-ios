/* SPDX-FileCopyrightText: 2024 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Stubs for FontForge functions not needed in Blender's minimal build.
 */

#include "fontforge-config.h"
#include "splinefont.h"
#include "ustring.h"

struct edgeinfo;

/* UI interface stub - not used in Blender */
struct ui_interface *ui_interface = NULL;

/* FreeType stubs - Blender uses its own FreeType integration */
int hasFreeType(void) { return 0; }
void *FreeTypeFontContext(SplineFont *sf, SplineChar *sc, struct fontviewbase *fv, int layer) {
    (void)sf; (void)sc; (void)fv; (void)layer; return NULL;
}
void FreeTypeFreeContext(void *freetypecontext) { (void)freetypecontext; }
BDFChar *SplineCharFreeTypeRasterize(void *freetypecontext, int gid, int ptsize, int dpi, int depth) {
    (void)freetypecontext; (void)gid; (void)ptsize; (void)dpi; (void)depth; return NULL;
}
BDFChar *SplineCharFreeTypeRasterizeNoHints(SplineChar *sc, int layer, int ptsize, int dpi, int depth) {
    (void)sc; (void)layer; (void)ptsize; (void)dpi; (void)depth; return NULL;
}

/* Bounds calculation stubs */
void SplineCharFindBounds(SplineChar *sc, DBounds *b) {
    (void)sc;
    b->minx = b->miny = 0;
    b->maxx = b->maxy = 1000;
}

void SplineCharQuickConservativeBounds(SplineChar *sc, DBounds *b) {
    SplineCharFindBounds(sc, b);
}

void SplineFontFindBounds(SplineFont *sf, DBounds *b) {
    (void)sf;
    b->minx = b->miny = 0;
    b->maxx = b->maxy = 1000;
}

void SplineFontQuickConservativeBounds(SplineFont *sf, DBounds *b) {
    SplineFontFindBounds(sf, b);
}

/* NOTE: IterateSplineSolve, CheckExtremaForSingleBitErrors, and SplineFindExtrema
 * are now provided by FontForge's original implementations (exposed in splineutil.c).
 * The stubs were removed to avoid duplicate symbol errors. */

int SplineApproximate(Spline *s, double p1, double p2, double *x, double *y) {
    (void)s; (void)p1; (void)p2; (void)x; (void)y;
    return 0;
}

/* Point type change stub - not called in Blender's use case since
 * SPLCategorizePoints uses pconvert_flag_by_geom, not pconvert_flag_force_type */
void SPChangePointType(SplinePoint *sp, int pointtype) {
    (void)sp; (void)pointtype;
}

/* Other stubs */
int SSHasClip(SplineSet *ss) { (void)ss; return 0; }
int SCWorthOutputting(SplineChar *sc) { (void)sc; return 1; }
SplineChar *SFGetChar(SplineFont *sf, int unienc, const char *name) { (void)sf; (void)unienc; (void)name; return NULL; }
void PatternSCBounds(SplineChar *sc, DBounds *b) { (void)sc; b->minx = b->miny = 0; b->maxx = b->maxy = 100; }
void MatInverse(double m[6], double r[6]) { (void)m; (void)r; }

/* Stubs for splineutil2.c functions not needed in Blender */
void SSRegenerateFromSpiros(SplineSet *spl, int layer) { (void)spl; (void)layer; }
void IntersectLines(BasePoint *inter, BasePoint *line1_1, BasePoint *line1_2, BasePoint *line2_1, BasePoint *line2_2) {
    (void)line1_1; (void)line1_2; (void)line2_1; (void)line2_2;
    inter->x = inter->y = 0;
}
void ElFreeEI(struct edgeinfo *ei) { (void)ei; }
void NormVec(BasePoint *v) { (void)v; }
int BpColinear(BasePoint *a, BasePoint *b, BasePoint *c) { (void)a; (void)b; (void)c; return 0; }
void MatMultiply(double m1[6], double m2[6], double r[6]) { (void)m1; (void)m2; (void)r; }


/* NOT STUBS (small implementations). */
char *copy(const char *x) {return strdup(x);}
