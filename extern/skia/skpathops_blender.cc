/* SPDX-FileCopyrightText: 2024 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Skia PathOps wrapper for overlap removal.
 *
 * This implementation uses Skia's PathOps Simplify function.
 * When WITH_SKIA_PATHOPS is defined, it links against Skia.
 * Otherwise, it provides stub implementations that pass through unchanged.
 */

#include "skpathops_blender.h"

#include <cstdlib>
#include <vector>

#ifdef WITH_SKIA_PATHOPS
#  include "include/core/SkPath.h"
#  include "include/core/SkPathBuilder.h"
#  include "include/pathops/SkPathOps.h"
#endif

/* ============================================================================
 * Internal structures
 * ============================================================================ */

struct SKPathBuilder {
#ifdef WITH_SKIA_PATHOPS
  SkPathBuilder builder;
#else
  /* Fallback: store path data manually */
  struct Segment {
    enum Type { MOVE, LINE, CUBIC, CLOSE } type;
    double points[6]; /* Up to 3 points (6 coords) for cubic */
  };
  std::vector<Segment> segments;
#endif
};

struct SKPath {
#ifdef WITH_SKIA_PATHOPS
  SkPath path;
#else
  std::vector<SKPathBuilder::Segment> segments;
#endif
};

struct SKPathIterator {
#ifdef WITH_SKIA_PATHOPS
  SkPath::Iter iter;
  bool started;
#else
  const SKPath *path;
  size_t index;
#endif
};

/* ============================================================================
 * Path Builder API
 * ============================================================================ */

SKPathBuilder *SK_CreatePathBuilder(void)
{
  return new (std::nothrow) SKPathBuilder();
}

void SK_MoveTo(SKPathBuilder *builder, double x, double y)
{
  if (builder == nullptr) {
    return;
  }
#ifdef WITH_SKIA_PATHOPS
  builder->builder.moveTo(SkScalar(x), SkScalar(y));
#else
  SKPathBuilder::Segment seg;
  seg.type = SKPathBuilder::Segment::MOVE;
  seg.points[0] = x;
  seg.points[1] = y;
  builder->segments.push_back(seg);
#endif
}

void SK_LineTo(SKPathBuilder *builder, double x, double y)
{
  if (builder == nullptr) {
    return;
  }
#ifdef WITH_SKIA_PATHOPS
  builder->builder.lineTo(SkScalar(x), SkScalar(y));
#else
  SKPathBuilder::Segment seg;
  seg.type = SKPathBuilder::Segment::LINE;
  seg.points[0] = x;
  seg.points[1] = y;
  builder->segments.push_back(seg);
#endif
}

void SK_CubicTo(SKPathBuilder *builder,
                double cp1x,
                double cp1y,
                double cp2x,
                double cp2y,
                double x,
                double y)
{
  if (builder == nullptr) {
    return;
  }
#ifdef WITH_SKIA_PATHOPS
  builder->builder.cubicTo(
      SkScalar(cp1x), SkScalar(cp1y), SkScalar(cp2x), SkScalar(cp2y), SkScalar(x), SkScalar(y));
#else
  SKPathBuilder::Segment seg;
  seg.type = SKPathBuilder::Segment::CUBIC;
  seg.points[0] = cp1x;
  seg.points[1] = cp1y;
  seg.points[2] = cp2x;
  seg.points[3] = cp2y;
  seg.points[4] = x;
  seg.points[5] = y;
  builder->segments.push_back(seg);
#endif
}

void SK_Close(SKPathBuilder *builder)
{
  if (builder == nullptr) {
    return;
  }
#ifdef WITH_SKIA_PATHOPS
  builder->builder.close();
#else
  SKPathBuilder::Segment seg;
  seg.type = SKPathBuilder::Segment::CLOSE;
  builder->segments.push_back(seg);
#endif
}

SKPath *SK_FinishPath(SKPathBuilder *builder)
{
  if (builder == nullptr) {
    return nullptr;
  }

  SKPath *path = new (std::nothrow) SKPath();
  if (path == nullptr) {
    delete builder;
    return nullptr;
  }

#ifdef WITH_SKIA_PATHOPS
  path->path = builder->builder.detach();
#else
  path->segments = std::move(builder->segments);
#endif

  delete builder;
  return path;
}

void SK_FreePathBuilder(SKPathBuilder *builder)
{
  delete builder;
}

/* ============================================================================
 * Simplify API
 * ============================================================================ */

void SK_SetFillType(SKPath *path, SKFillType fill_type)
{
  if (path == nullptr) {
    return;
  }
#ifdef WITH_SKIA_PATHOPS
  switch (fill_type) {
    case SK_FILL_WINDING:
      path->path.setFillType(SkPathFillType::kWinding);
      break;
    case SK_FILL_EVENODD:
      path->path.setFillType(SkPathFillType::kEvenOdd);
      break;
  }
#else
  (void)fill_type;
#endif
}

SKPath *SK_Simplify(SKPath *path)
{
  if (path == nullptr) {
    return nullptr;
  }

#ifdef WITH_SKIA_PATHOPS
  SkPath simplified;
  bool ok = Simplify(path->path, &simplified);

  if (!ok) {
    delete path;
    return nullptr;
  }

  path->path = std::move(simplified);
  return path;
#else
  /* Stub: return path unchanged when Skia is not available. */
  return path;
#endif
}

/* ============================================================================
 * Iterator API
 * ============================================================================ */

SKPathIterator *SK_CreateIterator(SKPath *path)
{
  if (path == nullptr) {
    return nullptr;
  }

  SKPathIterator *iter = new (std::nothrow) SKPathIterator();
  if (iter == nullptr) {
    return nullptr;
  }

#ifdef WITH_SKIA_PATHOPS
  iter->iter.setPath(path->path, false);
  iter->started = false;
#else
  iter->path = path;
  iter->index = 0;
#endif

  return iter;
}

SKPathVerb SK_IteratorNext(SKPathIterator *iter, double points[8])
{
  if (iter == nullptr) {
    return SK_VERB_DONE;
  }

#ifdef WITH_SKIA_PATHOPS
  SkPoint pts[4];
  SkPath::Verb verb = iter->iter.next(pts);

  switch (verb) {
    case SkPath::kMove_Verb:
      points[0] = double(pts[0].fX);
      points[1] = double(pts[0].fY);
      return SK_VERB_MOVE;

    case SkPath::kLine_Verb:
      points[0] = double(pts[1].fX);
      points[1] = double(pts[1].fY);
      return SK_VERB_LINE;

    case SkPath::kCubic_Verb:
      points[0] = double(pts[1].fX);
      points[1] = double(pts[1].fY);
      points[2] = double(pts[2].fX);
      points[3] = double(pts[2].fY);
      points[4] = double(pts[3].fX);
      points[5] = double(pts[3].fY);
      return SK_VERB_CUBIC;

    case SkPath::kQuad_Verb:
      /* Convert quadratic to cubic for consistent output */
      {
        double qp0x = double(pts[0].fX);
        double qp0y = double(pts[0].fY);
        double qp1x = double(pts[1].fX);
        double qp1y = double(pts[1].fY);
        double qp2x = double(pts[2].fX);
        double qp2y = double(pts[2].fY);

        /* Quadratic to cubic conversion:
         * CP1 = P0 + 2/3 * (P1 - P0)
         * CP2 = P2 + 2/3 * (P1 - P2) */
        points[0] = qp0x + (2.0 / 3.0) * (qp1x - qp0x);
        points[1] = qp0y + (2.0 / 3.0) * (qp1y - qp0y);
        points[2] = qp2x + (2.0 / 3.0) * (qp1x - qp2x);
        points[3] = qp2y + (2.0 / 3.0) * (qp1y - qp2y);
        points[4] = qp2x;
        points[5] = qp2y;
        return SK_VERB_CUBIC;
      }

    case SkPath::kConic_Verb:
      /* Conics need special handling - approximate as cubic */
      {
        double cp0x = double(pts[0].fX);
        double cp0y = double(pts[0].fY);
        double cp1x = double(pts[1].fX);
        double cp1y = double(pts[1].fY);
        double cp2x = double(pts[2].fX);
        double cp2y = double(pts[2].fY);
        /* Simple approximation - treat as quadratic */
        points[0] = cp0x + (2.0 / 3.0) * (cp1x - cp0x);
        points[1] = cp0y + (2.0 / 3.0) * (cp1y - cp0y);
        points[2] = cp2x + (2.0 / 3.0) * (cp1x - cp2x);
        points[3] = cp2y + (2.0 / 3.0) * (cp1y - cp2y);
        points[4] = cp2x;
        points[5] = cp2y;
        return SK_VERB_CUBIC;
      }

    case SkPath::kClose_Verb:
      return SK_VERB_CLOSE;

    case SkPath::kDone_Verb:
    default:
      return SK_VERB_DONE;
  }
#else
  /* Stub implementation */
  if (iter->index >= iter->path->segments.size()) {
    return SK_VERB_DONE;
  }

  const auto &seg = iter->path->segments[iter->index++];

  switch (seg.type) {
    case SKPathBuilder::Segment::MOVE:
      points[0] = seg.points[0];
      points[1] = seg.points[1];
      return SK_VERB_MOVE;

    case SKPathBuilder::Segment::LINE:
      points[0] = seg.points[0];
      points[1] = seg.points[1];
      return SK_VERB_LINE;

    case SKPathBuilder::Segment::CUBIC:
      points[0] = seg.points[0];
      points[1] = seg.points[1];
      points[2] = seg.points[2];
      points[3] = seg.points[3];
      points[4] = seg.points[4];
      points[5] = seg.points[5];
      return SK_VERB_CUBIC;

    case SKPathBuilder::Segment::CLOSE:
      return SK_VERB_CLOSE;

    default:
      return SK_VERB_DONE;
  }
#endif
}

void SK_FreeIterator(SKPathIterator *iter)
{
  delete iter;
}

void SK_FreePath(SKPath *path)
{
  delete path;
}
