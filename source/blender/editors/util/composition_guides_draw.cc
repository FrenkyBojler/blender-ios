#include <cmath>
#include <cstring>

#include "DNA_camera_types.h"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"

namespace blender::ui {

#define M_GOLDEN_RATIO_CONJUGATE 0.618033988749895f

static void drawviewborder_grid3(uint shdr_pos, rctf *rect, float fac)
{
  float x3, y3, x4, y4;

  x3 = rect->xmin + fac * (rect->xmax - rect->xmin);
  y3 = rect->ymin + fac * (rect->ymax - rect->ymin);
  x4 = rect->xmin + (1.0f - fac) * (rect->xmax - rect->xmin);
  y4 = rect->ymin + (1.0f - fac) * (rect->ymax - rect->ymin);

  immBegin(GPU_PRIM_LINES, 8);

  immVertex2f(shdr_pos, rect->xmin, y3);
  immVertex2f(shdr_pos, rect->xmax, y3);

  immVertex2f(shdr_pos, rect->xmin, y4);
  immVertex2f(shdr_pos, rect->xmax, y4);

  immVertex2f(shdr_pos, x3, rect->ymin);
  immVertex2f(shdr_pos, x3, rect->ymax);

  immVertex2f(shdr_pos, x4, rect->ymin);
  immVertex2f(shdr_pos, x4, rect->ymax);

  immEnd();
}

/* harmonious triangle */
static void drawviewborder_triangle(uint shdr_pos, rctf *rect, const char golden, const char dir)
{
  /* Has to create local copies in order to use std::swap*/
  float xmin = rect->xmin;
  float xmax = rect->xmax;
  float ymin = rect->ymin;
  float ymax = rect->ymax;

  float ofs;
  float w = xmax - xmin;
  float h = ymax - ymin;

  immBegin(GPU_PRIM_LINES, 6);

  if (w > h) {
    if (golden) {
      ofs = w * (1.0f - M_GOLDEN_RATIO_CONJUGATE);
    }
    else {
      ofs = h * (h / w);
    }
    if (dir == 'B') {
      std::swap(ymin, ymax);
    }

    immVertex2f(shdr_pos, xmin, ymin);
    immVertex2f(shdr_pos, xmax, ymax);

    immVertex2f(shdr_pos, xmax, ymin);
    immVertex2f(shdr_pos, xmin + (w - ofs), ymax);

    immVertex2f(shdr_pos, xmin, ymax);
    immVertex2f(shdr_pos, xmin + ofs, ymin);
  }
  else {
    if (golden) {
      ofs = h * (1.0f - M_GOLDEN_RATIO_CONJUGATE);
    }
    else {
      ofs = w * (w / h);
    }
    if (dir == 'B') {
      std::swap(xmin, xmax);
    }

    immVertex2f(shdr_pos, xmin, ymin);
    immVertex2f(shdr_pos, xmax, ymax);

    immVertex2f(shdr_pos, xmax, ymin);
    immVertex2f(shdr_pos, xmin, ymin + ofs);

    immVertex2f(shdr_pos, xmin, ymax);
    immVertex2f(shdr_pos, xmax, ymin + (h - ofs));
  }

  immEnd();
}

void draw_composition_guides(uint shdr_pos,
                             eCompositionGuideFlags flag,
                             rctf *rect,
                             const float color[4])
{
  immUniformColor4fv(color);

  if (flag & COMPOSITION_GUIDES_CENTER) {
    float xmid, ymid;

    xmid = rect->xmin + 0.5f * (rect->xmax - rect->xmin);
    ymid = rect->ymin + 0.5f * (rect->ymax - rect->ymin);

    immBegin(GPU_PRIM_LINES, 4);

    immVertex2f(shdr_pos, rect->xmin, ymid);
    immVertex2f(shdr_pos, rect->xmax, ymid);

    immVertex2f(shdr_pos, xmid, rect->ymin);
    immVertex2f(shdr_pos, xmid, rect->ymax);

    immEnd();
  }

  if (flag & COMPOSITION_GUIDES_CENTER_DIAG) {
    immBegin(GPU_PRIM_LINES, 4);

    immVertex2f(shdr_pos, rect->xmin, rect->ymin);
    immVertex2f(shdr_pos, rect->xmax, rect->ymax);

    immVertex2f(shdr_pos, rect->xmin, rect->ymax);
    immVertex2f(shdr_pos, rect->xmax, rect->ymin);

    immEnd();
  }

  if (flag & COMPOSITION_GUIDES_THIRDS) {
    drawviewborder_grid3(shdr_pos, rect, 1.0f / 3.0f);
  }

  if (flag & COMPOSITION_GUIDES_GOLDEN) {
    drawviewborder_grid3(shdr_pos, rect, 1.0f - M_GOLDEN_RATIO_CONJUGATE);
  }

  if (flag & COMPOSITION_GUIDES_GOLDEN_TRI_A) {
    drawviewborder_triangle(shdr_pos, rect, 0, 'A');
  }

  if (flag & COMPOSITION_GUIDES_GOLDEN_TRI_B) {
    drawviewborder_triangle(shdr_pos, rect, 0, 'B');
  }

  if (flag & COMPOSITION_GUIDES_HARMONY_TRI_A) {
    drawviewborder_triangle(shdr_pos, rect, 1, 'A');
  }

  if (flag & COMPOSITION_GUIDES_HARMONY_TRI_B) {
    drawviewborder_triangle(shdr_pos, rect, 1, 'B');
  }
}
}  // namespace blender::ui
