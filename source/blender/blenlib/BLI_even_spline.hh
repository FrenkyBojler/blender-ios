/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Arc length parameterized spline library.
 *
 * Provides cubic Bezier curves and multi-segment splines that are
 * parameterized by arc length, giving even spacing along the curve.
 *
 * Properties of arc length parameterization:
 * - Even spacing along the curve.
 * - Second derivative equals the curve normal multiplied by curvature.
 * - Easy to calculate torsion for space curves (Frenet-Serret formulas).
 */

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace blender {

template<typename Float, int axes, int table_size = 512> class CubicBezier {
  using Vector = VecBase<Float, axes>;

 public:
  Vector ps[4];
  Float length;

  static const int TableSize = table_size;

  CubicBezier(Vector a, Vector b, Vector c, Vector d)
  {
    ps[0] = a;
    ps[1] = b;
    ps[2] = c;
    ps[3] = d;
    length = 0;
    arc_to_t_ = new Float[table_size];
  }

  ~CubicBezier()
  {
    delete[] arc_to_t_;
    arc_to_t_ = nullptr;
  }

  CubicBezier()
  {
    length = 0;
    arc_to_t_ = new Float[table_size];
  }

  CubicBezier(const CubicBezier &b)
  {
    arc_to_t_ = new Float[table_size];
    *this = b;
  }

  CubicBezier &operator=(const CubicBezier &b)
  {
    ps[0] = b.ps[0];
    ps[1] = b.ps[1];
    ps[2] = b.ps[2];
    ps[3] = b.ps[3];
    length = b.length;

    if (!arc_to_t_) {
      arc_to_t_ = new Float[table_size];
    }

    if (b.arc_to_t_) {
      std::memcpy(arc_to_t_, b.arc_to_t_, sizeof(Float) * table_size);
    }

    return *this;
  }

  CubicBezier(CubicBezier &&b) noexcept
  {
    arc_to_t_ = nullptr;
    *this = std::move(b);
  }

  CubicBezier &operator=(CubicBezier &&b) noexcept
  {
    ps[0] = b.ps[0];
    ps[1] = b.ps[1];
    ps[2] = b.ps[2];
    ps[3] = b.ps[3];
    length = b.length;

    delete[] arc_to_t_;
    if (b.arc_to_t_) {
      arc_to_t_ = b.arc_to_t_;
      b.arc_to_t_ = nullptr;
    }
    else {
      arc_to_t_ = new Float[table_size];
    }

    return *this;
  }

  void update()
  {
    Float t = 0.0, dt = Float(1.0) / Float(table_size);

    if (!arc_to_t_) {
      arc_to_t_ = new Float[table_size];
    }

    for (int i = 0; i < table_size; i++) {
      arc_to_t_[i] = Float(-1.0);
    }

    length = 0.0;

    for (int i = 0; i < table_size; i++, t += dt) {
      Float dlen = 0.0;
      for (int j = 0; j < axes; j++) {
        Float dv = dcubic(ps[0][j], ps[1][j], ps[2][j], ps[3][j], t);
        dlen += dv * dv;
      }
      dlen = std::sqrt(dlen) * dt;
      length += dlen;
    }

    const int samples = table_size;
    dt = Float(1.0) / Float(samples);

    t = 0.0;
    Float s = 0.0;

    for (int i = 0; i < samples; i++, t += dt) {
      Float dlen = 0.0;
      for (int j = 0; j < axes; j++) {
        Float dv = dcubic(ps[0][j], ps[1][j], ps[2][j], ps[3][j], t);
        dlen += dv * dv;
      }
      dlen = std::sqrt(dlen) * dt;

      int j = int((s / length) * Float(table_size) * Float(0.999999));
      j = std::min(j, table_size - 1);

      arc_to_t_[j] = t;
      s += dlen;
    }

    arc_to_t_[0] = 0.0;
    arc_to_t_[table_size - 1] = 1.0;

    /* Interpolate gaps in table. */
    for (int i = 0; i < table_size - 1; i++) {
      if (arc_to_t_[i] == Float(-1.0) || arc_to_t_[i + 1] != Float(-1.0)) {
        continue;
      }

      int i1 = i;
      int i2 = i + 1;

      while (arc_to_t_[i2] == Float(-1.0)) {
        i2++;
      }

      Float start = arc_to_t_[i1];
      Float end = arc_to_t_[i2];
      Float dt2 = Float(1.0) / Float(i2 - i1);

      for (int j = i1 + 1; j < i2; j++) {
        Float factor = Float(j - i1) * dt2;
        arc_to_t_[j] = start + (end - start) * factor;
      }

      i = i2 - 1;
    }
  }

  inline Vector evaluate(Float s)
  {
    Float t = arc_to_t(s);
    Vector r;

    for (int i = 0; i < axes; i++) {
      r[i] = cubic(ps[0][i], ps[1][i], ps[2][i], ps[3][i], t);
    }

    return r;
  }

  Vector derivative(Float s, bool exact = true)
  {
    Float t = arc_to_t(s);
    Vector r;

    for (int i = 0; i < axes; i++) {
      r[i] = dcubic(ps[0][i], ps[1][i], ps[2][i], ps[3][i], t) * length;
    }

    if (exact) {
      Float len = std::sqrt(dot(r, r));
      if (len > Float(0.00001)) {
        r = r / len;
      }
    }

    return r;
  }

  Vector derivative2(Float s)
  {
    Float t = arc_to_t(s);
    Vector r;

    Float dx = dcubic(ps[0][0], ps[1][0], ps[2][0], ps[3][0], t);
    Float d2x = d2cubic(ps[0][0], ps[1][0], ps[2][0], ps[3][0], t);
    Float dy = dcubic(ps[0][1], ps[1][1], ps[2][1], ps[3][1], t);
    Float d2y = d2cubic(ps[0][1], ps[1][1], ps[2][1], ps[3][1], t);

    if constexpr (axes == 2) {
      Float div = std::sqrt(dx * dx + dy * dy) * (dx * dx + dy * dy);

      r[0] = ((d2x * dy - d2y * dx) * dy) / div;
      r[1] = (-(d2x * dy - d2y * dx) * dx) / div;
    }
    else if constexpr (axes == 3) {
      Float dz = dcubic(ps[0][2], ps[1][2], ps[2][2], ps[3][2], t);
      Float d2z = d2cubic(ps[0][2], ps[1][2], ps[2][2], ps[3][2], t);

      Float div = std::sqrt(dx * dx + dy * dy + dz * dz) * (dy * dy + dz * dz + dx * dx);

      r[0] = (d2x * dy * dy + d2x * dz * dz - d2y * dx * dy - d2z * dx * dz) / div;
      r[1] = (-(d2x * dx * dy - d2y * dx * dx - d2y * dz * dz + d2z * dy * dz)) / div;
      r[2] = (-(d2x * dx * dz + d2y * dy * dz - d2z * dx * dx - d2z * dy * dy)) / div;
    }
    else {
      for (int i = 0; i < axes; i++) {
        r[i] = d2cubic(ps[0][i], ps[1][i], ps[2][i], ps[3][i], t) * length;
      }
    }

    return r;
  }

  Float curvature(Float s)
  {
    Vector dv2 = derivative2(s);

    if constexpr (axes == 2) {
      Vector dv = derivative(s, true);
      return dv[0] * dv2[1] - dv[1] * dv2[0];
    }

    return std::sqrt(dot(dv2, dv2));
  }

  Float dcurvature(Float s)
  {
    const Float ds = Float(0.0001);
    Float s1, s2;

    if (s > Float(1.0) - ds) {
      s1 = s - ds;
      s2 = s;
    }
    else {
      s1 = s;
      s2 = s + ds;
    }

    Float a = curvature(s1);
    Float b = curvature(s2);

    return (b - a) / ds;
  }

 private:
  Float *arc_to_t_ = nullptr;

  Float cubic(Float k1, Float k2, Float k3, Float k4, Float t)
  {
    return -(((Float(3.0) * (t - Float(1.0)) * k3 - k4 * t) * t -
              Float(3.0) * (t - Float(1.0)) * (t - Float(1.0)) * k2) *
                 t +
             (t - Float(1.0)) * (t - Float(1.0)) * (t - Float(1.0)) * k1);
  }

  Float dcubic(Float k1, Float k2, Float k3, Float k4, Float t)
  {
    return Float(-3.0) *
           ((t - Float(1.0)) * (t - Float(1.0)) * k1 - k4 * t * t +
            (Float(3.0) * t - Float(2.0)) * k3 * t -
            (Float(3.0) * t - Float(1.0)) * (t - Float(1.0)) * k2);
  }

  Float d2cubic(Float k1, Float k2, Float k3, Float k4, Float t)
  {
    return Float(-6.0) * (k1 * t - k1 - Float(3.0) * k2 * t + Float(2.0) * k2 +
                          Float(3.0) * k3 * t - k3 - k4 * t);
  }

  Float dot(Vector a, Vector b)
  {
    Float sum = 0.0;
    for (int i = 0; i < axes; i++) {
      sum += a[i] * b[i];
    }
    return sum;
  }

  Float clamp_s(Float s)
  {
    s = s < Float(0.0) ? Float(0.0) : s;
    s = s >= length ? length * Float(0.999999) : s;
    return s;
  }

  Float arc_to_t(Float s)
  {
    if (length == Float(0.0)) {
      return Float(0.0);
    }

    s = clamp_s(s);

    Float t = s * Float(table_size - 1) / length;

    int i1 = int(std::floor(t));
    int i2 = std::min(i1 + 1, table_size - 1);

    t -= Float(i1);

    Float s1 = arc_to_t_[i1];
    Float s2 = arc_to_t_[i2];

    return s1 + (s2 - s1) * t;
  }
};

template<typename Float,
         int axes,
         typename BezierType = CubicBezier<Float, axes>>
class EvenSpline {
  using Vector = VecBase<Float, axes>;
  struct Segment {
    BezierType bezier;
    Float start = 0.0;

    Segment(const BezierType &bez) : bezier(bez) {}
    Segment(const Segment &b) = default;
    Segment &operator=(const Segment &b) = default;
    Segment() = default;
  };

 public:
  Float length = 0.0;
  blender::Vector<Segment> segments;
  blender::Vector<Float> inflection_points;

  void clear()
  {
    segments.clear();
  }

  EvenSpline() = default;
  ~EvenSpline() = default;

  void add(BezierType &bez)
  {
    Segment seg;
    seg.bezier = bez;
    segments.append(seg);
    update();
  }

  void update()
  {
    length = 0.0;
    for (Segment &seg : segments) {
      seg.start = length;
      length += seg.bezier.length;
    }
    update_inflection_points();
  }

  void update_inflection_points()
  {
    inflection_points.clear();
    inflection_points.append(Float(0.0));

    const int steps = segments.size() * 5;
    Float s = 0.0, ds = length / Float(steps - 1);
    Float dk = 0, lastdk = 0;

    for (int i = 0; i < steps; i++, s += ds, lastdk = dk) {
      dk = dcurvature(s);

      if (i == 0) {
        continue;
      }

      if ((dk < Float(0.0)) != (lastdk < Float(0.0))) {
        inflection_points.append(s - ds * Float(0.5));
      }
    }

    inflection_points.append(Float(1.0));
  }

  int order() noexcept
  {
    return sizeof(segments[0].bezier.ps) / sizeof(*segments[0].bezier.ps);
  }

  inline Vector evaluate(Float s)
  {
    if (s == Float(0.0)) {
      return segments[0].bezier.ps[0];
    }

    if (s >= length) {
      return segments[segments.size() - 1].bezier.ps[order() - 1];
    }

    Segment *seg = get_segment(s);
    return seg->bezier.evaluate(s - seg->start);
  }

  Vector derivative(Float s, bool exact = true)
  {
    if (segments.size() == 0) {
      return Vector(0);
    }

    s = clamp_s(s);
    Segment *seg = get_segment(s);
    return seg->bezier.derivative(s - seg->start, exact);
  }

  Vector derivative2(Float s)
  {
    if (segments.size() == 0) {
      return Vector(0);
    }

    s = clamp_s(s);
    Segment *seg = get_segment(s);
    return seg->bezier.derivative2(s - seg->start);
  }

  Float curvature(Float s)
  {
    if (segments.size() == 0) {
      return Float(0.0);
    }

    s = clamp_s(s);
    Segment *seg = get_segment(s);
    return seg->bezier.curvature(s - seg->start);
  }

  Float dcurvature(Float s)
  {
    if (segments.size() == 0) {
      return Float(0.0);
    }

    s = clamp_s(s);
    Segment *seg = get_segment(s);
    return seg->bezier.dcurvature(s - seg->start);
  }

  Vector closest_point(const Vector p, Float &r_s, Vector &r_tan, Float &r_dis)
  {
    if (segments.size() == 0) {
      return Vector(0);
    }

    Float mindis = FLT_MAX;
    Vector minp(0);
    Float mins = 0.0;
    bool found = false;

    Vector lastdv(0), lastp(0);
    Vector b(0), dvb(0);

    for (int i = 0; i < inflection_points.size(); i++, lastp = b, lastdv = dvb) {
      Float s = inflection_points[i];

      b = evaluate(s);
      dvb = derivative(s, false);

      if (i == 0) {
        continue;
      }

      Vector dva = lastdv;
      Vector a = lastp;

      Vector vec1 = a - p;
      Vector vec2 = b - p;

      Float sign1 = dot(vec1, dva);
      Float sign2 = dot(vec2, dvb);

      if ((sign1 < Float(0.0)) == (sign2 < Float(0.0))) {
        found = true;

        Float len = dot(vec1, vec1);
        if (len < mindis) {
          mindis = len;
          mins = s;
          minp = evaluate(s);
        }
        continue;
      }

      found = true;

      Float ds_local = s - inflection_points[i - 1];
      Float start = s - ds_local;
      Float end = s;
      Float mid = (start + end) * Float(0.5);
      const int binary_steps = 10;

      for (int j = 0; j < binary_steps; j++) {
        Vector dvmid = derivative(mid, false);
        Vector vecmid = evaluate(mid) - p;
        Float sign_mid = dot(vecmid, dvmid);

        if ((sign_mid < Float(0.0)) == (sign1 < Float(0.0))) {
          start = mid;
        }
        else {
          end = mid;
        }
        mid = (start + end) * Float(0.5);
      }

      Vector p2 = evaluate(mid);
      Vector vec_mid = p2 - p;
      Float len = dot(vec_mid, vec_mid);

      if (len < mindis) {
        mindis = len;
        minp = p2;
        mins = mid;
      }
    }

    if (!found) {
      mins = 0.0;
      minp = evaluate(mins);
      Vector vec = minp - p;
      mindis = dot(vec, vec);
    }

    r_tan = derivative(mins, true);
    r_s = mins;
    r_dis = std::sqrt(mindis);

    return minp;
  }

  void pop_front(int n = 1)
  {
    for (int64_t i = 0; i < int64_t(segments.size()) - n; i++) {
      segments[i] = segments[i + n];
    }

    segments.resize(segments.size() - n);
    update();
  }

 private:
  Float dot(Vector a, Vector b)
  {
    Float sum = 0.0;
    for (int i = 0; i < axes; i++) {
      sum += a[i] * b[i];
    }
    return sum;
  }

  Float clamp_s(Float s)
  {
    s = s < Float(0.0) ? Float(0.0) : s;
    s = s >= length ? length * (Float(1.0) - Float(FLT_EPSILON)) : s;
    return s;
  }

  Segment *get_segment(Float s)
  {
    /* Binary search: segments are sorted by start in ascending order. */
    int lo = 0, hi = int(segments.size()) - 1;
    while (lo < hi) {
      int mid = (lo + hi + 1) / 2;
      if (segments[mid].start <= s) {
        lo = mid;
      }
      else {
        hi = mid - 1;
      }
    }
    if (lo >= 0 && lo < int(segments.size())) {
      return &segments[lo];
    }
    return nullptr;
  }
};

using BezierSpline2f = EvenSpline<float, 2>;
using BezierSpline3f = EvenSpline<float, 3>;

}  // namespace blender
