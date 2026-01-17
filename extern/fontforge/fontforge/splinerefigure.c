/* Copyright (C) 2000-2012 by George Williams */
/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.

 * The name of the author may not be used to endorse or promote products
 * derived from this software without specific prior written permission.

 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


/* #define FF_DEBUG_SPLINEREFIGURE */  /* Enable debug output */

#include "splinefont.h"
#include "splineutil.h"

#include <math.h>

/* No-op for Blender builds - FontForge's error reporting not available */
#define IError(...) ((void)0)

/* The slight errors introduced by the optimizer turn out to have nasty */
/*  side effects. An error on the order of 7e-8 in splines[1].b caused */
/*  the rasterizer to have kaniptions */
void SplineRefigure3(Spline *spline) {
    SplinePoint *from = spline->from, *to = spline->to;
    Spline1D *xsp = &spline->splines[0], *ysp = &spline->splines[1];
    Spline old;

    spline->isquadratic = false;
    if ( spline->acceptableextrema )
	old = *spline;
    xsp->d = from->me.x; ysp->d = from->me.y;
    // Set noprevcp and nonextcp based on point values but then make sure both
    // have the same value
    int orig_nonextcp = from->nextcp.x==from->me.x && from->nextcp.y == from->me.y;
    int orig_noprevcp = to->prevcp.x==to->me.x && to->prevcp.y == to->me.y;
    from->nonextcp = orig_nonextcp;
    to->noprevcp = orig_noprevcp;
    if ( !from->nonextcp || !to->noprevcp )
	from->nonextcp = to->noprevcp = false;
#ifdef FF_DEBUG_SPLINEREFIGURE
    fprintf(stderr, "SplineRefigure3: from(%.2f,%.2f) nextcp(%.2f,%.2f) to(%.2f,%.2f) prevcp(%.2f,%.2f) "
                    "orig_nonext=%d orig_noprev=%d final_nonext=%d final_noprev=%d\n",
            from->me.x, from->me.y, from->nextcp.x, from->nextcp.y,
            to->me.x, to->me.y, to->prevcp.x, to->prevcp.y,
            orig_nonextcp, orig_noprevcp, from->nonextcp, to->noprevcp);
#endif
    if ( from->nonextcp && to->noprevcp ) {
	spline->islinear = true;
	xsp->c = to->me.x-from->me.x;
	ysp->c = to->me.y-from->me.y;
	xsp->a = xsp->b = 0;
	ysp->a = ysp->b = 0;
    } else {
	/* from p. 393 (Operator Details, curveto) PostScript Lang. Ref. Man. (Red book) */
	xsp->c = 3*(from->nextcp.x-from->me.x);
	ysp->c = 3*(from->nextcp.y-from->me.y);
	xsp->b = 3*(to->prevcp.x-from->nextcp.x)-xsp->c;
	ysp->b = 3*(to->prevcp.y-from->nextcp.y)-ysp->c;
	xsp->a = to->me.x-from->me.x-xsp->c-xsp->b;
	ysp->a = to->me.y-from->me.y-ysp->c-ysp->b;
	if ( RealNear(xsp->c,0)) xsp->c=0;
	if ( RealNear(ysp->c,0)) ysp->c=0;
	if ( RealNear(xsp->b,0)) xsp->b=0;
	if ( RealNear(ysp->b,0)) ysp->b=0;
	if ( RealNear(xsp->a,0)) xsp->a=0;
	if ( RealNear(ysp->a,0)) ysp->a=0;
	if ( xsp->a!=0 && ( Within16RoundingErrors(xsp->a+from->me.x,from->me.x) ||
			    Within16RoundingErrors(xsp->a+to->me.x,to->me.x)))
	    xsp->a = 0;
	if ( ysp->a!=0 && ( Within16RoundingErrors(ysp->a+from->me.y,from->me.y) ||
			    Within16RoundingErrors(ysp->a+to->me.y,to->me.y)))
	    ysp->a = 0;
	SplineIsLinear(spline);
	spline->islinear = false;
	if ( ysp->a==0 && xsp->a==0 ) {
	    if ( ysp->b==0 && xsp->b==0 )
		spline->islinear = true;	/* This seems extremely unlikely... */
	    else
		spline->isquadratic = true;	/* Only likely if we read in a TTF */
	}
    }
    if ( !isfinite(ysp->a) || !isfinite(xsp->a) || !isfinite(ysp->c) || !isfinite(xsp->c) || !isfinite(ysp->d) || !isfinite(xsp->d))
	IError("NaN value in spline creation");
    LinearApproxFree(spline->approx);
    spline->approx = NULL;
    spline->knowncurved = false;
    spline->knownlinear = spline->islinear;
    SplineIsLinear(spline);
    spline->order2 = false;
#ifdef FF_DEBUG_SPLINEREFIGURE
    fprintf(stderr, "  -> islinear=%d knownlinear=%d a=(%.4f,%.4f) b=(%.4f,%.4f)\n",
            spline->islinear, spline->knownlinear,
            xsp->a, ysp->a, xsp->b, ysp->b);
#endif

    if ( spline->acceptableextrema ) {
	/* I don't check "d", because changes to that reflect simple */
	/*  translations which will not affect the shape of the spline */
	if ( !RealNear(old.splines[0].a,spline->splines[0].a) ||
		!RealNear(old.splines[0].b,spline->splines[0].b) ||
		!RealNear(old.splines[0].c,spline->splines[0].c) ||
		!RealNear(old.splines[1].a,spline->splines[1].a) ||
		!RealNear(old.splines[1].b,spline->splines[1].b) ||
		!RealNear(old.splines[1].c,spline->splines[1].c) )
	    spline->acceptableextrema = false;
    }
}

void SplineRefigure(Spline *spline) {
    SplineRefigure3(spline);
}

