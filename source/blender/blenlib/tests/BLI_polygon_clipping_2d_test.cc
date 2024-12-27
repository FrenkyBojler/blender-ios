/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_offset_indices.hh"

#include <fstream>
#include <iostream>
#include <sstream>
#include <type_traits>

/* Should tests draw their output to an HTML file? */
#define DO_DRAW 0

#include "BLI_polygon_clipping_2d.hh"

namespace blender::polygonboolean {

static void CSS_setup_style(std::ofstream &f)
{
  constexpr int border_width = 5;
  constexpr int stroke_width = 3;
  constexpr int stroke_dasharray = 15;

  f << "div {\n"
       "  border: "
    << border_width
    << "px solid black;\n"
       "  text-align: center;\n"
       "}\n"
       "\n";

  f << ".polygon-A {\n"
       "  fill: none;\n"
       "  stroke: red;\n"
       "  stroke-width: "
    << stroke_width
    << "px;\n"
       "  stroke-dasharray: "
    << stroke_dasharray
    << "px;\n"
       "}\n";
  f << ".polygon-B {\n"
       "  fill: none;\n"
       "  stroke: blue;\n"
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
    constexpr int max_draw_width = 800;
    constexpr int max_draw_height = 600;

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

static void SVG_add_polygon(std::ofstream &f,
                            const std::string &class_name,
                            const Span<float2> points,
                            const SVGMapping &mapping)
{
  f << "<polygon class = \"" << class_name << "\" points = \"";
  for (const int i : points.index_range()) {
    const float2 &point = points[i];
    if (i != 0) {
      f << ", ";
    }
    f << mapping.SX(point[0]) << "," << mapping.SY(point[1]);
  }
  f << "\"/>\n";
}

static void SVG_add_polygons_as_path(std::ofstream &f,
                                     const std::string &class_name,
                                     const Span<float2> points,
                                     const OffsetIndices<int> points_by_polygon,
                                     const SVGMapping &mapping)
{
  f << "<path class = \"" << class_name << "\" d = \"";
  for (const int polygon_id : points_by_polygon.index_range()) {
    const IndexRange vert_ids = points_by_polygon[polygon_id];
    if (polygon_id != 0) {
      f << " ";
    }

    f << "M ";
    for (const int i : vert_ids) {
      const float2 &point = points[i];
      const int j = i - vert_ids.first();

      if (j == 1) {
        f << " L ";
      }
      else if (j != 0) {
        f << ", ";
      }
      f << mapping.SX(point[0]) << "," << mapping.SY(point[1]);
    }
    f << " Z";
  }

  f << "\"";

  f << " fill-rule=\"evenodd\"";

  f << "/>\n";
}

static void SVG_add_line(std::ofstream &f,
                         const std::string &class_name,
                         const Span<float2> points,
                         const SVGMapping &mapping)
{
  f << "<path class = \"" << class_name << "\" d = \"";

  f << "M ";
  for (const int i : points.index_range()) {
    const float2 &point = points[i];

    if (i == 1) {
      f << " L ";
    }
    else if (i != 0) {
      f << ", ";
    }
    f << mapping.SX(point[0]) << "," << mapping.SY(point[1]);
  }

  f << "\"";

  f << "/>\n";
}

static void SVG_add_lines(std::ofstream &f,
                          const std::string &class_name,
                          const Span<float2> points,
                          const OffsetIndices<int> points_by_polygon,
                          const SVGMapping &mapping)
{
  f << "<path class = \"" << class_name << "\" d = \"";
  for (const int polygon_id : points_by_polygon.index_range()) {
    const IndexRange vert_ids = points_by_polygon[polygon_id];
    if (polygon_id != 0) {
      f << " ";
    }

    f << "M ";
    for (const int i : vert_ids) {
      const float2 &point = points[i];
      const int j = i - vert_ids.first();

      if (j == 1) {
        f << " L ";
      }
      else if (j != 0) {
        f << ", ";
      }
      f << mapping.SX(point[0]) << "," << mapping.SY(point[1]);
    }
  }

  f << "\"";

  f << "/>\n";
}

static bool draw_append = false; /* Will be set to true after first call. */

std::ofstream get_file_stream()
{
  /* Would like to use BKE_tempdir_base() here, but that brings in dependence on kernel library.
   * This is just for developer debugging anyway, and should never be called in production Blender.
   */
#ifdef WIN32
  constexpr const char *drawfile = "./polygon_clipping_test_draw.html";
#else
  constexpr const char *drawfile = "/tmp/polygon_clipping_test_draw.html";
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

void draw_polygons(const std::string &label,
                   const Span<float2> curve_a,
                   const Span<float2> curve_b,
                   const std::optional<BooleanResult> &result)
{
  if (!DO_DRAW || !result.has_value()) {
    return;
  }

  const Bounds<float2> bounds = *bounds::merge(bounds::min_max(curve_a), bounds::min_max(curve_b));
  SVGMapping mapping = SVGMapping(bounds);

  std::ofstream f = get_file_stream();
  if (!f) {
    return;
  }

  f << "<div>\n";
  f << "<h1>" << label << "</h1>\n";

  f << "<svg width=\"" << mapping.view_width << "\" height=\"" << mapping.view_height << "\">\n";

  SVG_add_polygon(f, "polygon-A", curve_a, mapping);
  SVG_add_polygon(f, "polygon-B", curve_b, mapping);
  Array<float2> points((*result).point_offsets.last());
  calculate_positions(curve_a, curve_b, (*result), points.as_mutable_span());

  const OffsetIndices<int> points_by_polygon = OffsetIndices<int>((*result).point_offsets);

  if (points_by_polygon.size() == 1) {
    SVG_add_polygon(f, "polygon-C", points, mapping);
  }
  else {
    SVG_add_polygons_as_path(f, "polygon-C", points, points_by_polygon, mapping);
  }

  f << "</svg>\n";

  f << "</div>\n";
}

void draw_cut(const std::string &label,
              const bool is_a_cyclic,
              const Span<float2> curve_a,
              const Span<float2> curve_b,
              const std::optional<BooleanResult> &result)
{
  if (!DO_DRAW || !result.has_value()) {
    return;
  }

  const Bounds<float2> bounds = *bounds::merge(bounds::min_max(curve_a), bounds::min_max(curve_b));
  SVGMapping mapping = SVGMapping(bounds);

  std::ofstream f = get_file_stream();
  if (!f) {
    return;
  }

  f << "<div>\n";
  f << "<h1>" << label << "</h1>\n";

  f << "<svg width=\"" << mapping.view_width << "\" height=\"" << mapping.view_height << "\">\n";

  if (is_a_cyclic) {
    SVG_add_polygon(f, "cut-A", curve_a, mapping);
  }
  else {
    SVG_add_line(f, "cut-A", curve_a, mapping);
  }
  SVG_add_polygon(f, "cut-B", curve_b, mapping);

  Array<float2> points((*result).verts.size());
  interpolate_position_a(curve_a, (*result), points.as_mutable_span());
  const OffsetIndices<int> points_by_polygon = OffsetIndices<int>((*result).offsets);

  if (points_by_polygon.size() == 1) {
    SVG_add_line(f, "cut-C", points, mapping);
  }
  else {
    SVG_add_lines(f, "cut-C", points, points_by_polygon, mapping);
  }

  f << "</div>\n";
}

void expect_boolean_result_coord(const Span<float2> urve_a,
                                 const Span<float2> curve_b,
                                 const std::optional<BooleanResult> &result,
                                 const Array<Vector<float2>> &expected_points)
{
  EXPECT_TRUE(result.has_value());
  if (!result.has_value()) {
    return;
  }

  /* TODO */

  // const OffsetIndices<int> points_by_polygon = OffsetIndices<int>((*result).point_offsets);

  // EXPECT_EQ(points_by_polygon.size(), expected_points.size());
  // if (points_by_polygon.size() != expected_points.size()) {
  //   return;
  // }

  // int total_size = 0;
  // for (const int i : expected_points.index_range()) {
  //   total_size += expected_points[i].size();
  // }

  // Array<float2> points((*result).segment_offsets.last());
  // calculate_positions(curve_a, curve_b, (*result), points.as_mutable_span());

  // EXPECT_EQ(points.size(), total_size);
  // if (points.size() != total_size) {
  //   return;
  // }

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

TEST(polygonboolean, Squares_A_And_B)
{
  const Array<float2> points_a = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
  const Array<float2> points_b = {{1, 1}, {3, 1}, {3, 3}, {1, 3}};
  std::optional<BooleanResult> result = curve_boolean_calc(Operation::And, points_a, points_b);

  const Array<Vector<float2>> expected_points = {{{2, 2}, {1, 2}, {1, 1}, {2, 1}}};
  expect_boolean_result_coord(points_a, points_b, result, expected_points);

  draw_polygons("Squares A intersection B", points_a, points_b, result);
}

TEST(polygonboolean, Squares_A_Or_B)
{
  const Array<float2> points_a = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
  const Array<float2> points_b = {{1, 1}, {3, 1}, {3, 3}, {1, 3}};
  std::optional<BooleanResult> result = curve_boolean_calc(Operation::Or, points_a, points_b);

  const Array<Vector<float2>> expected_points = {
      {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 3}, {3, 3}, {3, 1}, {2, 1}}};
  expect_boolean_result_coord(points_a, points_b, result, expected_points);

  draw_polygons("Squares A Union B", points_a, points_b, result);
}

TEST(polygonboolean, Squares_A_Not_B)
{
  const Array<float2> points_a = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
  const Array<float2> points_b = {{1, 1}, {3, 1}, {3, 3}, {1, 3}};
  std::optional<BooleanResult> result = curve_boolean_calc(Operation::Not, points_a, points_b);

  const Array<Vector<float2>> expected_points = {{{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}}};
  expect_boolean_result_coord(points_a, points_b, result, expected_points);

  draw_polygons("Squares A Difference B", points_a, points_b, result);
}

TEST(polygonboolean, Simple_Intersection)
{
  /**
   * This is a replica of Fig. 10 from
   * Greiner, Günther; Kai Hormann (1998). "Efficient clipping of arbitrary polygons". ACM
   * Transactions on Graphics. 17 (2): 71-83.
   */
  const Array<float2> points_a = {{0, 6}, {8, 6}, {8, 3}, {0, 3}};
  const Array<float2> points_b = {{6, 0}, {6, 4}, {4, 2}, {2, 4}, {2, 0}};
  std::optional<BooleanResult> result = curve_boolean_calc(Operation::And, points_a, points_b);

  const Array<Vector<float2>> expected_points = {{{5, 3}, {6, 4}, {6, 3}},
                                                 {{2, 3}, {2, 4}, {3, 3}}};
  expect_boolean_result_coord(points_a, points_b, result, expected_points);

  draw_polygons("Simple Intersection", points_a, points_b, result);
}

TEST(polygonboolean, Simple_Union)
{
  /**
   * This is a replica of Fig. 10 from
   * Greiner, Günther; Kai Hormann (1998). "Efficient clipping of arbitrary polygons". ACM
   * Transactions on Graphics. 17 (2): 71-83.
   */
  const Array<float2> points_a = {{0, 6}, {8, 6}, {8, 3}, {0, 3}};
  const Array<float2> points_b = {{6, 0}, {6, 4}, {4, 2}, {2, 4}, {2, 0}};
  std::optional<BooleanResult> result = curve_boolean_calc(Operation::Or, points_a, points_b);

  const Array<Vector<float2>> expected_points = {
      {{8, 3}, {8, 6}, {0, 6}, {0, 3}, {2, 3}, {2, 0}, {6, 0}, {6, 3}}, {{3, 3}, {4, 2}, {5, 3}}};
  expect_boolean_result_coord(points_a, points_b, result, expected_points);

  draw_polygons("Simple Union", points_a, points_b, result);
}

TEST(polygonboolean, Complex_A_And_B)
{
  /**
   * This is a replica of Fig. 16 from
   * Greiner, Günther; Kai Hormann (1998). "Efficient clipping of arbitrary polygons". ACM
   * Transactions on Graphics. 17 (2): 71-83.
   */
  const Array<float2> points_a = {{14, 1}, {0, 5}, {14, 10}, {5, 6}, {14, 6}, {5, 5}};
  const Array<float2> points_b = {{9, 13}, {13, 0}, {9, 9}, {6, 0}};
  std::optional<BooleanResult> result = curve_boolean_calc(Operation::And, points_a, points_b);

  const Array<Vector<float2>> expected_points = {
      {{12.3455, 1.47273}, {12.2, 1.8}, {12.4851, 1.67327}, {12.5663, 1.40964}},
      {{6.71134, 3.08247}, {6.95349, 4.13178}, {7.32258, 3.96774}, {7, 3}},
      {{9.30137, 8.32192}, {9.45361, 7.97938}, {10.4135, 8.40602}, {10.3267, 8.68812}},
      {{7.79641, 7.78443}, {7.65714, 7.18095}, {8.52174, 7.56522}, {8.7027, 8.10811}},
      {{10.3333, 6}, {10.5059, 5.61176}, {11.2479, 5.69421}, {11.1538, 6}},
      {{7.38462, 6}, {7.21053, 5.24561}, {7.76923, 5.30769}, {8, 6}}};
  expect_boolean_result_coord(points_a, points_b, result, expected_points);

  draw_polygons("Complex A Intersection B", points_a, points_b, result);
}

TEST(polygonboolean, Complex_A_Or_B)
{
  /**
   * This is a replica of Fig. 16 from
   * Greiner, Günther; Kai Hormann (1998). "Efficient clipping of arbitrary polygons". ACM
   * Transactions on Graphics. 17 (2): 71-83.
   */
  const Array<float2> points_a = {{14, 1}, {0, 5}, {14, 10}, {5, 6}, {14, 6}, {5, 5}};
  const Array<float2> points_b = {{9, 13}, {13, 0}, {9, 9}, {6, 0}};
  std::optional<BooleanResult> result = curve_boolean_calc(Operation::Or, points_a, points_b);

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
  expect_boolean_result_coord(points_a, points_b, result, expected_points);

  draw_polygons("Complex A Union B", points_a, points_b, result);
}

TEST(polygonboolean, Complex_A_Not_B)
{
  /**
   * This is a replica of Fig. 16 from
   * Greiner, Günther; Kai Hormann (1998). "Efficient clipping of arbitrary polygons". ACM
   * Transactions on Graphics. 17 (2): 71-83.
   */
  const Array<float2> points_a = {{14, 1}, {0, 5}, {14, 10}, {5, 6}, {14, 6}, {5, 5}};
  const Array<float2> points_b = {{9, 13}, {13, 0}, {9, 9}, {6, 0}};
  std::optional<BooleanResult> result = curve_boolean_calc(Operation::Not, points_a, points_b);

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
  expect_boolean_result_coord(points_a, points_b, result, expected_points);

  draw_polygons("Complex A Difference B", points_a, points_b, result);
}

TEST(polygonboolean, Last_Segment_Interection)
{
  /**
   * These shapes are designed to test the following:
   *   1: Intersection with the last segment to others.
   *   2: Getting the next intersection point through a full loop from last segment to the first.
   *   3: Sorting multiple intersection points on the same segment.
   *   4: Self intersection in one of the shapes.
   */
  const Array<float2> points_a = {{0, 5}, {0, 0}, {7, 0}, {7, 5}};
  const Array<float2> points_b = {{2, 3}, {0, 7}, {3, 7}, {5, 4}, {6, 6}, {3, 4}, {2, 6}};
  std::optional<BooleanResult> result = curve_boolean_calc(Operation::Not, points_a, points_b);

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
  expect_boolean_result_coord(points_a, points_b, result, expected_points);

  draw_polygons("Last Segment Loop", points_a, points_b, result);
}

TEST(polygonboolean, Simple_Cut)
{
  const Array<float2> points_a = {{5, 7}, {3, 6}, {0, 2}, {0, 0}};
  const Array<float2> points_b = {{1, 6}, {3, 4}, {3, 1}, {0, 4}, {2, 3}};
  std::optional<BooleanResult> result = curve_boolean_cut(false, points_a, points_b);

  const Array<Vector<float2>> expected_points = {{{5, 7}, {3, 6}, {2.14286, 4.85714}},
                                                 {{1.61538, 4.15385}, {1.09091, 3.45455}},
                                                 {{0.857143, 3.14286}, {0, 2}, {0, 0}}};
  expect_boolean_result_coord(points_a, points_b, result, expected_points);

  draw_cut("Simple Cut", false, points_a, points_b, result);
}

TEST(polygonboolean, Simple_Cut_2)
{
  const Array<float2> points_a = {{5, 5}, {3, 5}, {1, 3}, {1, 1}};
  const Array<float2> points_b = {{5, 6}, {6, 5}, {1, 0}, {0, 1}};
  std::optional<BooleanResult> result = curve_boolean_cut(false, points_a, points_b);

  const Array<Vector<float2>> expected_points = {{{4, 5}, {3, 5}, {1, 3}, {1, 2}}};
  expect_boolean_result_coord(points_a, points_b, result, expected_points);

  draw_cut("Simple Cut 2", false, points_a, points_b, result);
}

TEST(polygonboolean, Simple_Cut_3)
{
  const Array<float2> points_a = {{6, 8}, {4, 7}, {1, 3}, {1, 1}};
  const Array<float2> points_b = {
      {3, 7}, {5, 5}, {1, 0}, {0, 4}, {2, 3}, {1, 5}, {3, 4}, {2, 6}, {4, 5}};
  std::optional<BooleanResult> result = curve_boolean_cut(false, points_a, points_b);

  const Array<Vector<float2>> expected_points = {{{6, 8}, {4, 7}, {3.57143, 6.42857}},
                                                 {{3.4, 6.2}, {2.90909, 5.54545}},
                                                 {{2.5, 5}, {2.09091, 4.45455}},
                                                 {{1.6, 3.8}, {1.27273, 3.36364}}};
  expect_boolean_result_coord(points_a, points_b, result, expected_points);

  draw_cut("Simple Cut 3", false, points_a, points_b, result);
}

TEST(polygonboolean, Simple_Cut_4)
{
  const Array<float2> points_a = {{6, 7}, {4, 6}, {1, 2}, {1, 0}};
  const Array<float2> points_b = {
      {0, 4}, {2, 2}, {7, 8}, {3, 7}, {4, 5}, {2, 6}, {3, 4}, {1, 5}, {2, 3}};
  std::optional<BooleanResult> result = curve_boolean_cut(false, points_a, points_b);

  const Array<Vector<float2>> expected_points = {{{3.7, 5.6}, {3.45455, 5.27273}},
                                                 {{2.8, 4.4}, {2.63636, 4.18182}},
                                                 {{1.9, 3.2}, {1.81818, 3.09091}},
                                                 {{1.42857, 2.57143}, {1, 2}, {1, 0}}};
  expect_boolean_result_coord(points_a, points_b, result, expected_points);

  draw_cut("Simple Cut 4", false, points_a, points_b, result);
}

TEST(polygonboolean, Cyclical_Cut)
{
  const Array<float2> points_a = {{6, 5}, {4, 5}, {1, 2}, {1, 0}};
  const Array<float2> points_b = {{1, 4}, {3, 1}, {5, 3}, {2, 5}, {3, 3}};
  std::optional<BooleanResult> result = curve_boolean_cut(true, points_a, points_b);

  const Array<Vector<float2>> expected_points = {{{4.4, 3.4}, {6, 5}, {4, 5}, {3.2, 4.2}},
                                                 {{2.66667, 3.66667}, {2.33333, 3.33333}},
                                                 {{1.8, 2.8}, {1, 2}, {1, 0}, {2.6, 1.6}}};
  expect_boolean_result_coord(points_a, points_b, result, expected_points);

  draw_cut("Cyclical Cut", true, points_a, points_b, result);
}

}  // namespace blender::polygonboolean
