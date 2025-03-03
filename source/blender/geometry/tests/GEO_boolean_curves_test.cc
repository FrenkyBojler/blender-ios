/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_bounds.hh"
#include "BLI_offset_indices.hh"

#include "BKE_attribute.hh"

#include <fstream>
#include <iostream>
#include <sstream>
#include <type_traits>

/* Should tests draw their output to an HTML file? */
#define DO_DRAW 0

#include "GEO_boolean_curves.hh"

using namespace blender::geometry::boolean;

namespace blender::geometry::tests {

static void CSS_setup_style(std::ofstream &f)
{
  constexpr int border_width = 5;
  constexpr int stroke_width = 3;
  constexpr int stroke_dasharray = 15;

  f << ".ui-group {\n"
       "  border: "
    << border_width << "px solid black;\n"
    << "  text-align: center;\n"
       "}\n"
       "\n";

  f << ".ui-list {\n"
       "  align-items: center;\n"
       "  display: inline-flex;\n"
       "}\n"
       "\n";

  f << ".polygon-A {\n"
       "  fill: blue;\n"
       "  fill-opacity: 0.25;\n"
       "  stroke: blue;\n"
       "  stroke-width: "
    << stroke_width
    << "px;\n"
       "  stroke-dasharray: "
    << stroke_dasharray
    << "px;\n"
       "}\n";
  f << ".polygon-B {\n"
       "  fill: red;\n"
       "  fill-opacity: 0.25;\n"
       "  stroke: red;\n"
       "  stroke-width: "
    << stroke_width
    << "px;\n"
       "  stroke-dasharray: "
    << stroke_dasharray
    << "px;\n"
       "}\n";
  f << ".polygon-C {\n"
       "  fill: green;\n"
       "  stroke: black;\n"
       "  stroke-width: "
    << stroke_width + 1
    << "px;\n"
       "  fill-opacity: 0.75;\n"
       "}\n"
       "\n";

  f << ".cut-A {\n"
       "  fill: none;\n"
       "  stroke: blue;\n"
       "  stroke-width: "
    << stroke_width
    << "px;\n"
       "  stroke-dasharray: "
    << stroke_dasharray
    << "px;\n"
       "}\n";
  f << ".cut-B {\n"
       "  fill: red;\n"
       "  stroke: red;\n"
       "  fill-opacity: 0.25;\n"
       "  stroke-width: "
    << stroke_width
    << "px;\n"
       "  stroke-dasharray: "
    << stroke_dasharray
    << "px;\n"
       "}\n";
  f << ".cut-C {\n"
       "  fill: none;\n"
       "  stroke: black;\n"
       "  stroke-width: "
    << stroke_width + 1
    << "px;\n"
       "}\n";
}

class SVGMapping {
 public:
  float2 topleft;
  float scale;
  float view_width;
  float view_height;

  float SX(const float x) const
  {
    return ((x - topleft[0]) * scale);
  }

  float SY(const float y) const
  {
    return ((topleft[1] - y) * scale);
  }

  SVGMapping(const Bounds<float2> &bounds)
  {
    constexpr int max_draw_width = 600;
    constexpr int max_draw_height = 400;

    const float draw_margin = (bounds.size().x + bounds.size().y) * 0.05;

    Bounds<float2> bounds_padded = bounds;
    bounds_padded.pad(draw_margin);

    topleft = float2(bounds_padded.min.x, bounds_padded.max.y);
    const float width = bounds_padded.size().x;
    const float height = bounds_padded.size().y;
    const float aspect = height / width;
    view_width = max_draw_width;
    view_height = int(view_width * aspect);
    if (view_height > max_draw_height) {
      view_height = max_draw_height;
      view_width = int(view_height / aspect);
    }
    scale = view_width / width;
  }
};

static void SVG_add_path(std::ofstream &f,
                         const std::string &class_name,
                         const VArraySpan<float2> &points,
                         const IndexMask &polygons,
                         const OffsetIndices<int> points_by_polygon,
                         const VArraySpan<bool> &cyclic,
                         const SVGMapping &mapping)
{
  polygons.foreach_index([&](const int64_t polygon_id) {
    f << "<path class = \"" << class_name << "\" d = \"";

    const IndexRange vert_ids = points_by_polygon[polygon_id];
    /* TODO. */
    // if (polygon_id != 0) {
    //   f << " ";
    // }

    f << "M ";
    for (const int i : vert_ids.index_range()) {
      const float2 &point = points[vert_ids[i]];

      if (i == 1) {
        f << " L ";
      }
      else if (i != 0) {
        f << ", ";
      }
      f << mapping.SX(point[0]) << "," << mapping.SY(point[1]);
    }
    if (cyclic[polygon_id]) {
      f << " Z";
    }

    f << "\"";
    f << " fill-rule=\"evenodd\"";
    f << "/>\n";
  });
}

static bool draw_append = false; /* Will be set to true after first call. */

std::ofstream get_file_stream()
{
#ifdef WIN32
  constexpr const char *drawfile = "./boolean_curves_test_draw.html";
#else
  constexpr const char *drawfile = "/tmp/boolean_curves_test_draw.html";
#endif

  std::ofstream f;
  if (draw_append) {
    f.open(drawfile, std::ios_base::app);
  }
  else {
    f.open(drawfile);
  }
  if (!f) {
    std::cout << "Could not open file " << drawfile << "\n";
    return f;
  }

  if (!draw_append) {
    f << "<!DOCTYPE html>\n";

    f << "<style>\n";

    CSS_setup_style(f);

    f << "</style>\n";
  }

  draw_append = true;

  return f;
}

void draw_divider_start(const std::string &label)
{
  if (!DO_DRAW) {
    return;
  }

  std::ofstream f = get_file_stream();
  if (!f) {
    return;
  }

  f << "<div class=\"ui-group\">\n";
  f << "<h1>" << label << "</h1>\n";
  f << "<div class=\"ui-list\">\n";
}

void draw_divider_end()
{
  if (!DO_DRAW) {
    return;
  }

  std::ofstream f = get_file_stream();
  if (!f) {
    return;
  }

  /* Exit `ui-list` */
  f << "</div>\n";
  /* Exit `ui-group` */
  f << "</div>\n";
}

void draw_results(const std::string &label,
                  const std::string &type,
                  const bke::CurvesGeometry &src_curves,
                  const bke::CurvesGeometry &dst_curves,
                  const IndexMask &clipping_shapes)
{
  if (!DO_DRAW) {
    return;
  }

  std::ofstream f = get_file_stream();
  if (!f) {
    return;
  }

  const OffsetIndices<int> src_points_by_curve = src_curves.points_by_curve();
  const OffsetIndices<int> dst_points_by_curve = dst_curves.points_by_curve();
  const VArraySpan<bool> src_cyclic = src_curves.cyclic();
  const VArraySpan<bool> dst_cyclic = dst_curves.cyclic();
  const VArray<float2> src_points = *src_curves.attributes().lookup<float2>(
      ".positions_2d", bke::AttrDomain::Point);
  const VArray<float2> dst_points = *dst_curves.attributes().lookup<float2>(
      ".positions_2d", bke::AttrDomain::Point);

  /* TODO. */
  IndexMaskMemory memory;
  const IndexMask subject_shapes = clipping_shapes.complement(src_points_by_curve.index_range(),
                                                              memory);

  BLI_assert(src_points.is_span());
  const SVGMapping mapping = SVGMapping(*bounds::min_max(src_points.get_internal_span()));

  f << "<div>\n";
  f << "<svg width=\"" << mapping.view_width << "\" height=\"" << mapping.view_height << "\">\n";

  SVG_add_path(
      f, type + "-A", src_points, subject_shapes, src_points_by_curve, src_cyclic, mapping);
  SVG_add_path(
      f, type + "-B", src_points, clipping_shapes, src_points_by_curve, src_cyclic, mapping);

  SVG_add_path(f,
               type + "-C",
               dst_points,
               dst_points_by_curve.index_range(),
               dst_points_by_curve,
               dst_cyclic,
               mapping);

  f << "</svg>\n";
  f << "<h2>" << label << "</h2>\n";
  f << "</div>\n";
}

static bke::CurvesGeometry create_test_curves(Span<int> offsets,
                                              Span<float2> points,
                                              Span<bool> cyclic,
                                              Span<bool> fills)
{
  BLI_assert(!offsets.is_empty());
  const int curves_num = offsets.size() - 1;
  const int points_num = offsets.last();
  BLI_assert(cyclic.size() == curves_num);
  BLI_assert(fills.size() == curves_num);

  bke::CurvesGeometry curves(points_num, curves_num);
  curves.offsets_for_write().copy_from(offsets);
  curves.cyclic_for_write().copy_from(cyclic);

  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  bke::SpanAttributeWriter<float2> pos_writer = attributes.lookup_or_add_for_write_span<float2>(
      ".positions_2d", bke::AttrDomain::Point);
  pos_writer.span.copy_from(points);
  pos_writer.finish();

  bke::SpanAttributeWriter<bool> fill_writer = attributes.lookup_or_add_for_write_span<bool>(
      "is_fill", bke::AttrDomain::Curve);
  fill_writer.span.copy_from(fills);
  fill_writer.finish();

  return curves;
}

void expect_boolean_result_coord(const bke::CurvesGeometry &dst_curves,
                                 const Array<Vector<float2>> &expected_points)
{
  /* TODO */

  // const OffsetIndices<int> points_by_polygon = OffsetIndices<int>((*result).point_offsets);

  EXPECT_EQ(dst_curves.curves_num(), expected_points.size());
  if (dst_curves.curves_num() != expected_points.size()) {
    return;
  }

  int total_size = 0;
  for (const int i : expected_points.index_range()) {
    total_size += expected_points[i].size();
  }

  const VArray<float2> points = *dst_curves.attributes().lookup<float2>(".positions_2d",
                                                                        bke::AttrDomain::Point);

  EXPECT_EQ(points.size(), total_size);
  if (points.size() != total_size) {
    return;
  }

  // for (const int polygon_id : points_by_polygon.index_range()) {
  //   const IndexRange vert_ids = points_by_polygon[polygon_id];

  //   for (const int i : vert_ids) {
  //     const float2 &point = points[i];
  //     const int j = i - vert_ids.first();
  //     const float2 &expected_point = expected_points[polygon_id][j];

  //     EXPECT_NEAR(point[0], expected_point[0], 1e-4);
  //     EXPECT_NEAR(point[1], expected_point[1], 1e-4);
  //   }
  // }
}

TEST(boolean_curves, Squares)
{
  draw_divider_start("Squares");

  const Array<float2> points = {{0, 0}, {2, 0}, {2, 2}, {0, 2}, {1, 1}, {3, 1}, {3, 3}, {1, 3}};
  const Array<int> points_by_curve = {0, 4, 8};
  const Array<bool> is_fill = {true, true};
  const Array<bool> is_cyclic = {true, true};
  const IndexRange clipping_shapes = IndexRange(1, 1);

  const bke::CurvesGeometry src_curves = create_test_curves(
      points_by_curve, points, is_cyclic, is_fill);

  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Intersect, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{2, 2}, {1, 2}, {1, 1}, {2, 1}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Intersection", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Union, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 3}, {3, 3}, {3, 1}, {2, 1}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Union", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Difference, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Difference", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  draw_divider_end();
}

TEST(boolean_curves, Simple)
{
  draw_divider_start("Simple");

  /**
   * This is a replica of Fig. 10 from
   * Greiner, Günther; Kai Hormann (1998). "Efficient clipping of arbitrary polygons". ACM
   * Transactions on Graphics. 17 (2): 71-83.
   */
  const Array<float2> points = {
      {0, 6}, {8, 6}, {8, 3}, {0, 3}, {6, 0}, {6, 4}, {4, 2}, {2, 4}, {2, 0}};
  const Array<int> points_by_curve = {0, 4, 9};
  const Array<bool> is_fill = {true, true};
  const Array<bool> is_cyclic = {true, true};
  const IndexRange clipping_shapes = IndexRange(1, 1);

  const bke::CurvesGeometry src_curves = create_test_curves(
      points_by_curve, points, is_cyclic, is_fill);

  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Intersect, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{5, 3}, {6, 4}, {6, 3}},
                                                   {{2, 3}, {2, 4}, {3, 3}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Intersection", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Union, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{8, 3}, {8, 6}, {0, 6}, {0, 3}, {2, 3}, {2, 0}, {6, 0}, {6, 3}},
        {{3, 3}, {4, 2}, {5, 3}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Union", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Difference, src_curves, clipping_shapes);

    /* TODO. */
    // const Array<Vector<float2>> expected_points = {
    //     {{8, 3}, {8, 6}, {0, 6}, {0, 3}, {2, 3}, {2, 0}, {6, 0}, {6, 3}},
    //     {{3, 3}, {4, 2}, {5, 3}}};
    // expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Difference", "polygon", src_curves, dst_curves, clipping_shapes);
  }

  draw_divider_end();
}

TEST(boolean_curves, Complex)
{
  draw_divider_start("Complex");

  /**
   * This is a replica of Fig. 16 from
   * Greiner, Günther; Kai Hormann (1998). "Efficient clipping of arbitrary polygons". ACM
   * Transactions on Graphics. 17 (2): 71-83.
   */
  const Array<float2> points = {
      {14, 1}, {0, 5}, {14, 10}, {5, 6}, {14, 6}, {5, 5}, {9, 13}, {13, 0}, {9, 9}, {6, 0}};
  const Array<int> points_by_curve = {0, 6, 10};
  const Array<bool> is_fill = {true, true};
  const Array<bool> is_cyclic = {true, true};
  const IndexRange clipping_shapes = IndexRange(1, 1);

  const bke::CurvesGeometry src_curves = create_test_curves(
      points_by_curve, points, is_cyclic, is_fill);

  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Intersect, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{12.3455, 1.47273}, {12.2, 1.8}, {12.4851, 1.67327}, {12.5663, 1.40964}},
        {{6.71134, 3.08247}, {6.95349, 4.13178}, {7.32258, 3.96774}, {7, 3}},
        {{9.30137, 8.32192}, {9.45361, 7.97938}, {10.4135, 8.40602}, {10.3267, 8.68812}},
        {{7.79641, 7.78443}, {7.65714, 7.18095}, {8.52174, 7.56522}, {8.7027, 8.10811}},
        {{10.3333, 6}, {10.5059, 5.61176}, {11.2479, 5.69421}, {11.1538, 6}},
        {{7.38462, 6}, {7.21053, 5.24561}, {7.76923, 5.30769}, {8, 6}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Intersection", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Union, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{14, 1},
         {12.4851, 1.67327},
         {11.2479, 5.69421},
         {14, 6},
         {11.1538, 6},
         {10.4135, 8.40602},
         {14, 10},
         {10.3267, 8.68812},
         {9, 13},
         {7.79641, 7.78443},
         {0, 5},
         {6.71134, 3.08247},
         {6, 0},
         {7, 3},
         {12.3455, 1.47273},
         {13, 0},
         {12.5663, 1.40964}},
        {{8.7027, 8.10811}, {9, 9}, {9.30137, 8.32192}},
        {{8.52174, 7.56522}, {8, 6}, {10.3333, 6}, {9.45361, 7.97938}},
        {{5, 6}, {7.38462, 6}, {7.65714, 7.18095}},
        {{7.76923, 5.30769}, {7.32258, 3.96774}, {12.2, 1.8}, {10.5059, 5.61176}},
        {{5, 5}, {6.95349, 4.13178}, {7.21053, 5.24561}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Union", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Difference, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{14, 1}, {12.4851, 1.67327}, {12.5663, 1.40964}},
        {{7, 3}, {7.32258, 3.96774}, {12.2, 1.8}, {12.3455, 1.47273}},
        {{0, 5},
         {7.79641, 7.78443},
         {7.65714, 7.18095},
         {5, 6},
         {7.38462, 6},
         {7.21053, 5.24561},
         {5, 5},
         {6.95349, 4.13178},
         {6.71134, 3.08247}},
        {{14, 10}, {10.4135, 8.40602}, {10.3267, 8.68812}},
        {{8.7027, 8.10811}, {8.52174, 7.56522}, {9.45361, 7.97938}, {9.30137, 8.32192}},
        {{14, 6}, {11.2479, 5.69421}, {11.1538, 6}},
        {{8, 6}, {7.76923, 5.30769}, {10.5059, 5.61176}, {10.3333, 6}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Difference", "polygon", src_curves, dst_curves, clipping_shapes);
  }

  draw_divider_end();
}

TEST(boolean_curves, Last_Edge_Loop)
{
  draw_divider_start("Last Edge Loop");

  /**
   * These shapes are designed to test the following:
   *   1: Intersection with the last edge.
   *   2: Segment connected through a full loop around.
   *   3: Multiple intersection on one edge not in order.
   *   4: Having a self intersection.
   */
  const Array<float2> points = {
      {0, 5}, {0, 0}, {7, 0}, {7, 5}, {2, 3}, {0, 7}, {3, 7}, {5, 4}, {6, 6}, {3, 4}, {2, 6}};
  const Array<int> points_by_curve = {0, 4, 11};
  const Array<bool> is_fill = {true, true};
  const Array<bool> is_cyclic = {true, true};
  const IndexRange clipping_shapes = IndexRange(1, 1);

  const bke::CurvesGeometry src_curves = create_test_curves(
      points_by_curve, points, is_cyclic, is_fill);

  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Intersect, src_curves, clipping_shapes);

    /* TODO. */
    // const Array<Vector<float2>> expected_points = {{{0, 5},
    //                                                 {0, 0},
    //                                                 {7, 0},
    //                                                 {7, 5},
    //                                                 {5.5, 5},
    //                                                 {5, 4},
    //                                                 {4.33333, 5},
    //                                                 {4.5, 5},
    //                                                 {3, 4},
    //                                                 {2.5, 5},
    //                                                 {2, 5},
    //                                                 {2, 3},
    //                                                 {1, 5}}};
    // expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Intersection", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Union, src_curves, clipping_shapes);

    /* TODO. */
    // const Array<Vector<float2>> expected_points = {{{0, 5},
    //                                                 {0, 0},
    //                                                 {7, 0},
    //                                                 {7, 5},
    //                                                 {5.5, 5},
    //                                                 {5, 4},
    //                                                 {4.33333, 5},
    //                                                 {4.5, 5},
    //                                                 {3, 4},
    //                                                 {2.5, 5},
    //                                                 {2, 5},
    //                                                 {2, 3},
    //                                                 {1, 5}}};
    // expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Union", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Difference, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{0, 5},
                                                    {0, 0},
                                                    {7, 0},
                                                    {7, 5},
                                                    {5.5, 5},
                                                    {5, 4},
                                                    {4.33333, 5},
                                                    {4.5, 5},
                                                    {3, 4},
                                                    {2.5, 5},
                                                    {2, 5},
                                                    {2, 3},
                                                    {1, 5}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Difference", "polygon", src_curves, dst_curves, clipping_shapes);
  }

  draw_divider_end();
}

TEST(boolean_curves, Simple_Cuts)
{
  draw_divider_start("Cuts");

  {
    const Array<float2> points = {
        {5, 7}, {3, 6}, {0, 2}, {0, 0}, {1, 6}, {3, 4}, {3, 1}, {0, 4}, {2, 3}};
    const Array<int> points_by_curve = {0, 4, 9};
    const Array<bool> is_fill = {false, true};
    const Array<bool> is_cyclic = {false, true};
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(
        points_by_curve, points, is_cyclic, is_fill);

    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Difference, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{5, 7}, {3, 6}, {2.14286, 4.85714}},
                                                   {{1.61538, 4.15385}, {1.09091, 3.45455}},
                                                   {{0.857143, 3.14286}, {0, 2}, {0, 0}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Simple Cut 1", "cut", src_curves, dst_curves, clipping_shapes);
  }
  {
    const Array<float2> points = {{5, 5}, {3, 5}, {1, 3}, {1, 1}, {5, 6}, {6, 5}, {1, 0}, {0, 1}};
    const Array<int> points_by_curve = {0, 4, 8};
    const Array<bool> is_fill = {false, true};
    const Array<bool> is_cyclic = {false, true};
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(
        points_by_curve, points, is_cyclic, is_fill);

    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Difference, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{4, 5}, {3, 5}, {1, 3}, {1, 2}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Simple Cut 2", "cut", src_curves, dst_curves, clipping_shapes);
  }
  {
    const Array<float2> points = {{6, 8},
                                  {4, 7},
                                  {1, 3},
                                  {1, 1},
                                  {3, 7},
                                  {5, 5},
                                  {1, 0},
                                  {0, 4},
                                  {2, 3},
                                  {1, 5},
                                  {3, 4},
                                  {2, 6},
                                  {4, 5}};
    const Array<int> points_by_curve = {0, 4, 13};
    const Array<bool> is_fill = {false, true};
    const Array<bool> is_cyclic = {false, true};
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(
        points_by_curve, points, is_cyclic, is_fill);

    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Difference, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{6, 8}, {4, 7}, {3.57143, 6.42857}},
                                                   {{3.4, 6.2}, {2.90909, 5.54545}},
                                                   {{2.5, 5}, {2.09091, 4.45455}},
                                                   {{1.6, 3.8}, {1.27273, 3.36364}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Simple Cut 3", "cut", src_curves, dst_curves, clipping_shapes);
  }
  {
    const Array<float2> points = {{6, 7},
                                  {4, 6},
                                  {1, 2},
                                  {1, 0},
                                  {0, 4},
                                  {2, 2},
                                  {7, 8},
                                  {3, 7},
                                  {4, 5},
                                  {2, 6},
                                  {3, 4},
                                  {1, 5},
                                  {2, 3}};
    const Array<int> points_by_curve = {0, 4, 13};
    const Array<bool> is_fill = {false, true};
    const Array<bool> is_cyclic = {false, true};
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(
        points_by_curve, points, is_cyclic, is_fill);

    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Difference, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{3.7, 5.6}, {3.45455, 5.27273}},
                                                   {{2.8, 4.4}, {2.63636, 4.18182}},
                                                   {{1.9, 3.2}, {1.81818, 3.09091}},
                                                   {{1.42857, 2.57143}, {1, 2}, {1, 0}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Simple Cut 4", "cut", src_curves, dst_curves, clipping_shapes);
  }
  {
    const Array<float2> points = {
        {6, 5}, {4, 5}, {1, 2}, {1, 0}, {1, 4}, {3, 1}, {5, 3}, {2, 5}, {3, 3}};
    const Array<int> points_by_curve = {0, 4, 9};
    const Array<bool> is_fill = {false, true};
    const Array<bool> is_cyclic = {true, true};
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(
        points_by_curve, points, is_cyclic, is_fill);

    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Difference, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{4.4, 3.4}, {6, 5}, {4, 5}, {3.2, 4.2}},
                                                   {{2.66667, 3.66667}, {2.33333, 3.33333}},
                                                   {{1.8, 2.8}, {1, 2}, {1, 0}, {2.6, 1.6}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Cyclical Cut", "cut", src_curves, dst_curves, clipping_shapes);
  }
  draw_divider_end();
}

TEST(boolean_curves, Squares_With_Holes)
{
  draw_divider_start("Squares With Holes");

  const Array<float2> points = {{0, 0},
                                {0, 5},
                                {5, 5},
                                {5, 0},

                                {1, 1},
                                {1, 4},
                                {4, 4},
                                {4, 1},

                                {2, 2},
                                {2, 7},
                                {7, 7},
                                {7, 2},

                                {3, 3},
                                {3, 6},
                                {6, 6},
                                {6, 3}};
  const Array<int> points_by_curve = {0, 4, 8, 12, 16};
  const IndexRange clipping_shapes = IndexRange::from_begin_end(2, 4);
  const Array<bool> is_fill = {true, true, true, true};
  const Array<bool> is_cyclic = {true, true, true, true};

  const bke::CurvesGeometry src_curves = create_test_curves(
      points_by_curve, points, is_cyclic, is_fill);

  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Intersect, src_curves, clipping_shapes);

    /* TODO. */
    // const Array<Vector<float2>> expected_points = {
    //     {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}}};
    // expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Intersection", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Union, src_curves, clipping_shapes);

    /* TODO. */
    // const Array<Vector<float2>> expected_points = {
    //     {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}}};
    // expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Union", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  {
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Difference, src_curves, clipping_shapes);

    /* TODO. */
    // const Array<Vector<float2>> expected_points = {
    //     {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}}};
    // expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Difference", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  draw_divider_end();
}

TEST(boolean_curves, Multiple_Shapes)
{
  draw_divider_start("Multiple Shapes");

  const Array<float2> points = {{0, 2},
                                {0, 7},
                                {5, 7},
                                {5, 2},

                                {2, 0},
                                {2, 5},
                                {7, 5},
                                {7, 0},

                                {3, 3},
                                {3, 8},
                                {8, 8},
                                {8, 3}};
  const Array<int> points_by_curve = {0, 4, 8, 12};
  const Array<bool> is_fill = {true, true, true};
  const Array<bool> is_cyclic = {true, true, true};

  const bke::CurvesGeometry src_curves = create_test_curves(
      points_by_curve, points, is_cyclic, is_fill);

  /**
   * Multiple separate but intersecting subject shapes.
   * The two subject shapes should be affected by the clipping shape, but not join into one.
   */
  {
    const IndexRange clipping_shapes = IndexRange::from_begin_end(2, 3);
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Intersect, src_curves, clipping_shapes);

    /* TODO. */
    // const Array<Vector<float2>> expected_points = {
    //     {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}}};
    // expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("2 Subjects Intersection", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  {
    const IndexRange clipping_shapes = IndexRange::from_begin_end(2, 3);
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Difference, src_curves, clipping_shapes);

    /* TODO. */
    // const Array<Vector<float2>> expected_points = {
    //     {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}}};
    // expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("2 Subjects Difference", "polygon", src_curves, dst_curves, clipping_shapes);
  }

  /**
   * Multiple separate but intersecting clipping shapes.
   * The subject shape should be affected as if the two clipping shapes were union.
   */
  {
    const IndexRange clipping_shapes = IndexRange::from_begin_end(0, 2);
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Intersect, src_curves, clipping_shapes);

    /* TODO. */
    // const Array<Vector<float2>> expected_points = {
    //     {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}}};
    // expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("2 Clipping Intersection", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  {
    const IndexRange clipping_shapes = IndexRange::from_begin_end(0, 2);
    const bke::CurvesGeometry dst_curves = curve_boolean(
        Operation::Difference, src_curves, clipping_shapes);

    /* TODO. */
    // const Array<Vector<float2>> expected_points = {
    //     {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}}};
    // expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("2 Clipping Difference", "polygon", src_curves, dst_curves, clipping_shapes);
  }
  draw_divider_end();
}

}  // namespace blender::geometry::tests
