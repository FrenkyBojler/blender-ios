/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup obj
 */

#pragma once

#include "BLI_math_vector_types.hh"

struct Curve;
struct Nurb;
struct OBJExportParams;

namespace blender::bke {
class CurvesGeometry;
}

namespace blender::io::obj {

/**
 * Finds the range within the control points that represents the sequence of valid spans or
 * 'segments'.
 *
 * For example, if a NURBS curve of order 2 has following 5 knots:
 *      [0, 0, 0, 1, 1]
 * associated to three control points. Valid control point range would be
 * the interval [1, 2] and the knot sequence [0, 0, 1, 1] since the first
 * knot/point does not contribute to any span/segment.
 */
Span<float> valid_nurb_control_point_range(int8_t order,
                                           Span<float> knots,
                                           IndexRange &point_range);

/**
 * Curve object wrapper for curve objects exported by the exporter. Curve objects can contain
 * multiple individual splines.
 */
class IOBJCurve {
 public:
  virtual ~IOBJCurve() = default;

  virtual const float4x4 &object_transform() const = 0;

  virtual const char *get_curve_name() const = 0;

  /**
   *  Number of splines associated with the Curve object.assign_if_different
   */
  virtual int total_splines() const = 0;
  /**
   * \param spline_index: Zero-based index of spline of interest.
   * \return Total vertices in a spline.
   */
  virtual int total_spline_vertices(int spline_index) const = 0;
  /**
   * Get the number of control points on the U-dimension.
   */
  virtual int num_control_points_u(int spline_index) const = 0;
  /**
   * Get the number of control points on the V-dimension.
   */
  virtual int num_control_points_v(int spline_index) const = 0;
  /**
   * Get the degree of the NURBS spline for the U-dimension.
   */
  virtual int get_nurbs_degree_u(int spline_index) const = 0;
  /**
   * Get the degree of the NURBS spline for the V-dimension.
   */
  virtual int get_nurbs_degree_v(int spline_index) const = 0;
  /**
   * True if the indexed spline is cyclic along U dimension.
   */
  virtual bool get_cyclic_u(int spline_index) const = 0;
  /**
   * Get the knot vector for the U-dimension. Computes knots using the buffer if necessary.
   */
  virtual Span<float> get_knots_u(int spline_index, Vector<float> &buffer) const = 0;
  /**
   * Get coordinates for the (non-looped) spline control points.
   */
  virtual Span<float3> vertex_coordinates(int spline_index,
                                          Vector<float3> &dynamic_point_buffer) const = 0;
};

/**
 * Provides access to the a Curve Object's properties.
 * Only support NURBS, TODO: support other types.
 *
 * \note Used for legacy Curve export.
 */
class OBJCurves : public IOBJCurve, NonCopyable {
 private:
  const bke::CurvesGeometry &curve_;
  const float4x4 transform_;
  const std::string name_;

 public:
  OBJCurves(const bke::CurvesGeometry &curve, const float4x4 &transform, const std::string &name);
  virtual ~OBJCurves() override = default;

  const float4x4 &object_transform() const override;

  const char *get_curve_name() const override;

  /**
   *  Number of splines associated with the Curve object.assign_if_different
   */
  int total_splines() const override;
  /**
   * \param spline_index: Zero-based index of spline of interest.
   * \return Total vertices in a spline.
   */
  int total_spline_vertices(int spline_index) const override;
  /**
   * Get the number of control points on the U-dimension.
   */
  int num_control_points_u(int spline_index) const override;
  /**
   * Get the number of control points on the V-dimension.
   */
  int num_control_points_v(int spline_index) const override;
  /**
   * Get the degree of the NURBS spline for the U-dimension.
   */
  int get_nurbs_degree_u(int spline_index) const override;
  /**
   * Get the degree of the NURBS spline for the V-dimension.
   */
  int get_nurbs_degree_v(int spline_index) const override;
  /**
   * True if the indexed spline is cyclic along U dimension.
   */
  bool get_cyclic_u(int spline_index) const override;
  /**
   * Get the knot vector for the U-dimension. Computes knots using the buffer if necessary.
   */
  Span<float> get_knots_u(int spline_index, Vector<float> &buffer) const override;
  /**
   * Get coordinates for the (non-looped) spline control points.
   */
  Span<float3> vertex_coordinates(int spline_index,
                                  Vector<float3> &dynamic_point_buffer) const override;
};

/**
 * Provides access to the a Curve Object's properties.
 * Only #CU_NURBS type is supported.
 *
 * \note Used for legacy Curve export.
 */
class OBJLegacyCurve : public IOBJCurve, NonCopyable {
 private:
  const Object *export_object_eval_;
  const Curve *export_curve_;

  const Nurb *get_spline(int spline_index) const;

 public:
  OBJLegacyCurve(const Depsgraph *depsgraph, Object *curve_object);
  virtual ~OBJLegacyCurve() override = default;

  const float4x4 &object_transform() const override;

  const char *get_curve_name() const override;

  /**
   *  Number of splines associated with the Curve object.assign_if_different
   */
  int total_splines() const override;
  /**
   * \param spline_index: Zero-based index of spline of interest.
   * \return Total vertices in a spline.
   */
  int total_spline_vertices(int spline_index) const override;
  /**
   * Get the number of control points on the U-dimension.
   */
  int num_control_points_u(int spline_index) const override;
  /**
   * Get the number of control points on the V-dimension.
   */
  int num_control_points_v(int spline_index) const override;
  /**
   * Get the degree of the NURBS spline for the U-dimension.
   */
  int get_nurbs_degree_u(int spline_index) const override;
  /**
   * Get the degree of the NURBS spline for the V-dimension.
   */
  int get_nurbs_degree_v(int spline_index) const override;
  /**
   * True if the indexed spline is cyclic along U dimension.
   */
  bool get_cyclic_u(int spline_index) const override;
  /**
   * Get the knot vector for the U-dimension. Computes knots using the buffer if necessary.
   */
  Span<float> get_knots_u(int spline_index, Vector<float> &buffer) const override;
  /**
   * Get coordinates for the (non-looped) spline control points.
   */
  Span<float3> vertex_coordinates(int spline_index,
                                  Vector<float3> &dynamic_point_buffer) const override;
};

}  // namespace blender::io::obj
