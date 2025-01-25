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
                         const Span<float2> points,
                         const OffsetIndices<int> points_by_polygon,
                         const Span<bool> cyclic,
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
    if (cyclic[polygon_id]) {
      f << " Z";
    }
  }

  f << "\"";

  f << " fill-rule=\"evenodd\"";

  f << "/>\n";
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

  f << "</div>\n";
  f << "</div>\n";
}

void draw_results(const std::string &label,
                  const std::string &type,
                  const Span<float2> input_points,
                  const OffsetIndices<int> points_by_curve,
                  const IndexRange clipping_shapes,
                  const Span<bool> is_cyclic,
                  const BooleanResult &result)
{
  if (!DO_DRAW) {
    return;
  }

  std::ofstream f = get_file_stream();
  if (!f) {
    return;
  }

  f << "<div>\n";

  const Bounds<float2> bounds = *bounds::min_max(input_points);
  SVGMapping mapping = SVGMapping(bounds);

  f << "<svg width=\"" << mapping.view_width << "\" height=\"" << mapping.view_height << "\">\n";

  const IndexRange subject_shapes = IndexRange::from_begin_end(0, clipping_shapes.first());

  Array<int> offset_a(subject_shapes.size() + 1);
  Array<int> offset_b(clipping_shapes.size() + 1);

  int offset = 0;

  for (const int i : subject_shapes.index_range()) {
    offset_a[i] = offset;
    offset += points_by_curve[subject_shapes[i]].size();
  }
  offset_a.last() = offset;

  offset = 0; /* Reuse. */

  for (const int i : clipping_shapes.index_range()) {
    offset_b[i] = offset;
    offset += points_by_curve[clipping_shapes[i]].size();
  }
  offset_b.last() = offset;

  const Span<float2> a_points = input_points.slice(
      IndexRange::from_begin_end_inclusive(points_by_curve[subject_shapes.first()].first(),
                                           points_by_curve[subject_shapes.last()].last()));
  const Span<float2> b_points = input_points.slice(
      IndexRange::from_begin_end_inclusive(points_by_curve[clipping_shapes.first()].first(),
                                           points_by_curve[clipping_shapes.last()].last()));

  SVG_add_path(f,
               type + "-A",
               a_points,
               OffsetIndices<int>(offset_a),
               is_cyclic.slice(subject_shapes),
               mapping);
  SVG_add_path(f,
               type + "-B",
               b_points,
               OffsetIndices<int>(offset_b),
               is_cyclic.slice(clipping_shapes),
               mapping);
  Array<float2> output_points(result.point_offsets.last());
  calculate_positions(input_points, result, output_points.as_mutable_span());

  const OffsetIndices<int> points_by_polygon = OffsetIndices<int>(result.point_offsets);

  SVG_add_path(f, type + "-C", output_points, points_by_polygon, result.cyclic, mapping);

  f << "</svg>\n";

  f << "<h2>" << label << "</h2>\n";

  f << "</div>\n";
}

static bke::CurvesGeometry create_test_curves(Span<int> offsets,
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

  bke::SpanAttributeWriter<bool> fill_writer =
      curves.attributes_for_write().lookup_or_add_for_write_span<bool>("is_fill",
                                                                       bke::AttrDomain::Curve);
  fill_writer.span.copy_from(fills);
  fill_writer.finish();

  return curves;
}

void expect_boolean_result_coord(const Span<float2> curve_subj,
                                 const Span<float2> curve_clip,
                                 const BooleanResult &result,
                                 const Array<Vector<float2>> &expected_points)
{
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
  // calculate_positions(curve_subj, curve_clip, (*result), points.as_mutable_span());

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

TEST(boolean_curves, Squares)
{
  draw_divider_start("Squares");

  const Array<float2> points_subj = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
  const Array<float2> points_clip = {{1, 1}, {3, 1}, {3, 3}, {1, 3}};
  const Array<bool> is_fill = {true, true};
  const Array<bool> is_cyclic = {true, true};

  /* TODO */
  const Array<int> points_by_curve(
      {0, int(points_subj.size()), int(points_subj.size() + points_clip.size())});
  Array<float2> points(points_subj.size() + points_clip.size());
  array_utils::copy(points_subj.as_span(),
                    points.as_mutable_span().slice(IndexRange(points_subj.size())));
  array_utils::copy(
      points_clip.as_span(),
      points.as_mutable_span().slice(IndexRange(points_subj.size(), points_clip.size())));
  const IndexRange clipping_shapes = IndexRange(1, 1);

  const bke::CurvesGeometry src_curves = create_test_curves(points_by_curve, is_cyclic, is_fill);

  {
    BooleanResult result = curve_boolean_calc(
        Operation::Intersect, src_curves, points, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{2, 2}, {1, 2}, {1, 1}, {2, 1}}};
    expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Intersection",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  {
    BooleanResult result = curve_boolean_calc(
        Operation::Union, src_curves, points, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 3}, {3, 3}, {3, 1}, {2, 1}}};
    expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Union",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  {
    BooleanResult result = curve_boolean_calc(
        Operation::Difference, src_curves, points, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}}};
    expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Difference",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
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
  const Array<float2> points_subj = {{0, 6}, {8, 6}, {8, 3}, {0, 3}};
  const Array<float2> points_clip = {{6, 0}, {6, 4}, {4, 2}, {2, 4}, {2, 0}};
  const Array<bool> is_fill = {true, true};
  const Array<bool> is_cyclic = {true, true};

  /* TODO */
  const Array<int> points_by_curve(
      {0, int(points_subj.size()), int(points_subj.size() + points_clip.size())});
  Array<float2> points(points_subj.size() + points_clip.size());
  array_utils::copy(points_subj.as_span(),
                    points.as_mutable_span().slice(IndexRange(points_subj.size())));
  array_utils::copy(
      points_clip.as_span(),
      points.as_mutable_span().slice(IndexRange(points_subj.size(), points_clip.size())));
  const IndexRange clipping_shapes = IndexRange(1, 1);

  const bke::CurvesGeometry src_curves = create_test_curves(points_by_curve, is_cyclic, is_fill);

  {
    BooleanResult result = curve_boolean_calc(
        Operation::Intersect, src_curves, points, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{5, 3}, {6, 4}, {6, 3}},
                                                   {{2, 3}, {2, 4}, {3, 3}}};
    expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Intersection",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  {
    BooleanResult result = curve_boolean_calc(
        Operation::Union, src_curves, points, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{8, 3}, {8, 6}, {0, 6}, {0, 3}, {2, 3}, {2, 0}, {6, 0}, {6, 3}},
        {{3, 3}, {4, 2}, {5, 3}}};
    expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Union",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  {
    BooleanResult result = curve_boolean_calc(
        Operation::Difference, src_curves, points, clipping_shapes);

    /* TODO. */
    // const Array<Vector<float2>> expected_points = {
    //     {{8, 3}, {8, 6}, {0, 6}, {0, 3}, {2, 3}, {2, 0}, {6, 0}, {6, 3}},
    //     {{3, 3}, {4, 2}, {5, 3}}};
    // expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Difference",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
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
  const Array<float2> points_subj = {{14, 1}, {0, 5}, {14, 10}, {5, 6}, {14, 6}, {5, 5}};
  const Array<float2> points_clip = {{9, 13}, {13, 0}, {9, 9}, {6, 0}};
  const Array<bool> is_fill = {true, true};
  const Array<bool> is_cyclic = {true, true};

  /* TODO */
  const Array<int> points_by_curve(
      {0, int(points_subj.size()), int(points_subj.size() + points_clip.size())});
  Array<float2> points(points_subj.size() + points_clip.size());
  array_utils::copy(points_subj.as_span(),
                    points.as_mutable_span().slice(IndexRange(points_subj.size())));
  array_utils::copy(
      points_clip.as_span(),
      points.as_mutable_span().slice(IndexRange(points_subj.size(), points_clip.size())));
  const IndexRange clipping_shapes = IndexRange(1, 1);

  const bke::CurvesGeometry src_curves = create_test_curves(points_by_curve, is_cyclic, is_fill);

  {
    BooleanResult result = curve_boolean_calc(
        Operation::Intersect, src_curves, points, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{12.3455, 1.47273}, {12.2, 1.8}, {12.4851, 1.67327}, {12.5663, 1.40964}},
        {{6.71134, 3.08247}, {6.95349, 4.13178}, {7.32258, 3.96774}, {7, 3}},
        {{9.30137, 8.32192}, {9.45361, 7.97938}, {10.4135, 8.40602}, {10.3267, 8.68812}},
        {{7.79641, 7.78443}, {7.65714, 7.18095}, {8.52174, 7.56522}, {8.7027, 8.10811}},
        {{10.3333, 6}, {10.5059, 5.61176}, {11.2479, 5.69421}, {11.1538, 6}},
        {{7.38462, 6}, {7.21053, 5.24561}, {7.76923, 5.30769}, {8, 6}}};
    expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Intersection",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  {
    BooleanResult result = curve_boolean_calc(
        Operation::Union, src_curves, points, clipping_shapes);

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
    expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Union",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  {
    BooleanResult result = curve_boolean_calc(
        Operation::Difference, src_curves, points, clipping_shapes);

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
    expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Difference",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
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
  const Array<float2> points_subj = {{0, 5}, {0, 0}, {7, 0}, {7, 5}};
  const Array<float2> points_clip = {{2, 3}, {0, 7}, {3, 7}, {5, 4}, {6, 6}, {3, 4}, {2, 6}};
  const Array<bool> is_fill = {true, true};
  const Array<bool> is_cyclic = {true, true};

  /* TODO */
  const Array<int> points_by_curve(
      {0, int(points_subj.size()), int(points_subj.size() + points_clip.size())});
  Array<float2> points(points_subj.size() + points_clip.size());
  array_utils::copy(points_subj.as_span(),
                    points.as_mutable_span().slice(IndexRange(points_subj.size())));
  array_utils::copy(
      points_clip.as_span(),
      points.as_mutable_span().slice(IndexRange(points_subj.size(), points_clip.size())));
  const IndexRange clipping_shapes = IndexRange(1, 1);

  const bke::CurvesGeometry src_curves = create_test_curves(points_by_curve, is_cyclic, is_fill);

  {
    BooleanResult result = curve_boolean_calc(
        Operation::Intersect, src_curves, points, clipping_shapes);

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
    // expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Intersection",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  {
    BooleanResult result = curve_boolean_calc(
        Operation::Union, src_curves, points, clipping_shapes);

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
    // expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Union",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  {
    BooleanResult result = curve_boolean_calc(
        Operation::Difference, src_curves, points, clipping_shapes);

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
    expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Difference",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }

  draw_divider_end();
}

TEST(boolean_curves, Simple_Cuts)
{
  draw_divider_start("Cuts");

  {
    const Array<float2> points_subj = {{5, 7}, {3, 6}, {0, 2}, {0, 0}};
    const Array<float2> points_clip = {{1, 6}, {3, 4}, {3, 1}, {0, 4}, {2, 3}};
    const Array<bool> is_fill = {false, true};
    const Array<bool> is_cyclic = {false, true};

    /* TODO */
    const Array<int> points_by_curve(
        {0, int(points_subj.size()), int(points_subj.size() + points_clip.size())});
    Array<float2> points(points_subj.size() + points_clip.size());
    array_utils::copy(points_subj.as_span(),
                      points.as_mutable_span().slice(IndexRange(points_subj.size())));
    array_utils::copy(
        points_clip.as_span(),
        points.as_mutable_span().slice(IndexRange(points_subj.size(), points_clip.size())));
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(points_by_curve, is_cyclic, is_fill);

    BooleanResult result = curve_boolean_calc(
        Operation::Difference, src_curves, points, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{5, 7}, {3, 6}, {2.14286, 4.85714}},
                                                   {{1.61538, 4.15385}, {1.09091, 3.45455}},
                                                   {{0.857143, 3.14286}, {0, 2}, {0, 0}}};
    expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Simple Cut 1",
                 "cut",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  {
    const Array<float2> points_subj = {{5, 5}, {3, 5}, {1, 3}, {1, 1}};
    const Array<float2> points_clip = {{5, 6}, {6, 5}, {1, 0}, {0, 1}};
    const Array<bool> is_fill = {false, true};
    const Array<bool> is_cyclic = {false, true};

    /* TODO */
    const Array<int> points_by_curve(
        {0, int(points_subj.size()), int(points_subj.size() + points_clip.size())});
    Array<float2> points(points_subj.size() + points_clip.size());
    array_utils::copy(points_subj.as_span(),
                      points.as_mutable_span().slice(IndexRange(points_subj.size())));
    array_utils::copy(
        points_clip.as_span(),
        points.as_mutable_span().slice(IndexRange(points_subj.size(), points_clip.size())));
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(points_by_curve, is_cyclic, is_fill);

    BooleanResult result = curve_boolean_calc(
        Operation::Difference, src_curves, points, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{4, 5}, {3, 5}, {1, 3}, {1, 2}}};
    expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Simple Cut 2",
                 "cut",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  {
    const Array<float2> points_subj = {{6, 8}, {4, 7}, {1, 3}, {1, 1}};
    const Array<float2> points_clip = {
        {3, 7}, {5, 5}, {1, 0}, {0, 4}, {2, 3}, {1, 5}, {3, 4}, {2, 6}, {4, 5}};
    const Array<bool> is_fill = {false, true};
    const Array<bool> is_cyclic = {false, true};

    /* TODO */
    const Array<int> points_by_curve(
        {0, int(points_subj.size()), int(points_subj.size() + points_clip.size())});
    Array<float2> points(points_subj.size() + points_clip.size());
    array_utils::copy(points_subj.as_span(),
                      points.as_mutable_span().slice(IndexRange(points_subj.size())));
    array_utils::copy(
        points_clip.as_span(),
        points.as_mutable_span().slice(IndexRange(points_subj.size(), points_clip.size())));
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(points_by_curve, is_cyclic, is_fill);

    BooleanResult result = curve_boolean_calc(
        Operation::Difference, src_curves, points, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{6, 8}, {4, 7}, {3.57143, 6.42857}},
                                                   {{3.4, 6.2}, {2.90909, 5.54545}},
                                                   {{2.5, 5}, {2.09091, 4.45455}},
                                                   {{1.6, 3.8}, {1.27273, 3.36364}}};
    expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Simple Cut 3",
                 "cut",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  {
    const Array<float2> points_subj = {{6, 7}, {4, 6}, {1, 2}, {1, 0}};
    const Array<float2> points_clip = {
        {0, 4}, {2, 2}, {7, 8}, {3, 7}, {4, 5}, {2, 6}, {3, 4}, {1, 5}, {2, 3}};
    const Array<bool> is_fill = {false, true};
    const Array<bool> is_cyclic = {false, true};

    /* TODO */
    const Array<int> points_by_curve(
        {0, int(points_subj.size()), int(points_subj.size() + points_clip.size())});
    Array<float2> points(points_subj.size() + points_clip.size());
    array_utils::copy(points_subj.as_span(),
                      points.as_mutable_span().slice(IndexRange(points_subj.size())));
    array_utils::copy(
        points_clip.as_span(),
        points.as_mutable_span().slice(IndexRange(points_subj.size(), points_clip.size())));
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(points_by_curve, is_cyclic, is_fill);

    BooleanResult result = curve_boolean_calc(
        Operation::Difference, src_curves, points, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{3.7, 5.6}, {3.45455, 5.27273}},
                                                   {{2.8, 4.4}, {2.63636, 4.18182}},
                                                   {{1.9, 3.2}, {1.81818, 3.09091}},
                                                   {{1.42857, 2.57143}, {1, 2}, {1, 0}}};
    expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Simple Cut 4",
                 "cut",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  {
    const Array<float2> points_subj = {{6, 5}, {4, 5}, {1, 2}, {1, 0}};
    const Array<float2> points_clip = {{1, 4}, {3, 1}, {5, 3}, {2, 5}, {3, 3}};
    const Array<bool> is_fill = {false, true};
    const Array<bool> is_cyclic = {true, true};

    /* TODO */
    const Array<int> points_by_curve(
        {0, int(points_subj.size()), int(points_subj.size() + points_clip.size())});
    Array<float2> points(points_subj.size() + points_clip.size());
    array_utils::copy(points_subj.as_span(),
                      points.as_mutable_span().slice(IndexRange(points_subj.size())));
    array_utils::copy(
        points_clip.as_span(),
        points.as_mutable_span().slice(IndexRange(points_subj.size(), points_clip.size())));
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(points_by_curve, is_cyclic, is_fill);

    BooleanResult result = curve_boolean_calc(
        Operation::Difference, src_curves, points, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{4.4, 3.4}, {6, 5}, {4, 5}, {3.2, 4.2}},
                                                   {{2.66667, 3.66667}, {2.33333, 3.33333}},
                                                   {{1.8, 2.8}, {1, 2}, {1, 0}, {2.6, 1.6}}};
    expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Cyclical Cut",
                 "cut",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
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

  const Array<int> points_by_curve({0, 4, 8, 12, 16});
  const IndexRange clipping_shapes = IndexRange::from_begin_end(2, 4);
  const Array<bool> is_fill = {true, true, true, true};
  const Array<bool> is_cyclic = {true, true, true, true};

  const bke::CurvesGeometry src_curves = create_test_curves(points_by_curve, is_cyclic, is_fill);

  {
    BooleanResult result = curve_boolean_calc(
        Operation::Intersect, src_curves, points, clipping_shapes);

    /* TODO. */
    // const Array<Vector<float2>> expected_points = {
    //     {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}}};
    // expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Intersection",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  {
    BooleanResult result = curve_boolean_calc(
        Operation::Union, src_curves, points, clipping_shapes);

    /* TODO. */
    // const Array<Vector<float2>> expected_points = {
    //     {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}}};
    // expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Union",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  {
    BooleanResult result = curve_boolean_calc(
        Operation::Difference, src_curves, points, clipping_shapes);

    /* TODO. */
    // const Array<Vector<float2>> expected_points = {
    //     {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}}};
    // expect_boolean_result_coord(points_subj, points_clip, result, expected_points);

    draw_results("Difference",
                 "polygon",
                 points,
                 OffsetIndices<int>(points_by_curve),
                 clipping_shapes,
                 is_cyclic,
                 result);
  }
  draw_divider_end();
}

}  // namespace blender::geometry::tests
