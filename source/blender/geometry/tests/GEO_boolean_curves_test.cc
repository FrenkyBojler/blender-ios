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
    constexpr int max_draw_width = 500;
    constexpr int max_draw_height = 350;

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
                         const Vector<IndexMask> &shapes,
                         const IndexMask &shapes_mask,
                         const OffsetIndices<int> points_by_curve,
                         const VArraySpan<bool> &cyclic,
                         const FillRule fill_rule,
                         const SVGMapping &mapping)
{
  shapes_mask.foreach_index([&](const int64_t shape_id) {
    f << "<path class = \"" << class_name << "\" d = \"";

    const IndexMask &shape = shapes[shape_id];
    shape.foreach_index([&](const int64_t curve_i, const int64_t pos) {
      if (pos != 0) {
        f << " ";
      }

      const IndexRange points_ids = points_by_curve[curve_i];
      f << "M ";
      for (const int i : points_ids.index_range()) {
        const float2 &point = points[points_ids[i]];

        if (i == 1) {
          f << " L ";
        }
        else if (i != 0) {
          f << ", ";
        }
        f << mapping.SX(point[0]) << "," << mapping.SY(point[1]);
      }
      if (cyclic[curve_i]) {
        f << " Z";
      }
    });

    f << "\"";
    if (fill_rule == FillRule::EvenOdd) {
      f << " fill-rule=\"evenodd\"";
    }
    else if (fill_rule == FillRule::NonZero) {
      f << " fill-rule=\"nonzero\"";
    }
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
                  const IndexMask &clipping_shapes,
                  const CurveBooleanOpParameters op_params)
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
  const VArray<int> src_shape_ids = *src_curves.attributes().lookup<int>("shape_id",
                                                                         bke::AttrDomain::Curve);
  const VArray<int> dst_shape_ids = *dst_curves.attributes().lookup<int>("shape_id",
                                                                         bke::AttrDomain::Curve);

  IndexMaskMemory memory;
  VectorSet<int> src_shape_indexing;
  VectorSet<int> dst_shape_indexing;
  const Vector<IndexMask> src_shapes = IndexMask::from_group_ids(
      src_shape_ids, memory, src_shape_indexing);
  const Vector<IndexMask> dst_shapes = IndexMask::from_group_ids(
      dst_shape_ids, memory, dst_shape_indexing);
  const IndexMask subject_shapes = clipping_shapes.complement(src_shapes.index_range(), memory);

  BLI_assert(src_points.is_span());
  const SVGMapping mapping = SVGMapping(*bounds::min_max(src_points.get_internal_span()));

  f << "<div>\n";
  f << "<svg width=\"" << mapping.view_width << "\" height=\"" << mapping.view_height << "\">\n";

  SVG_add_path(f,
               type + "-A",
               src_points,
               src_shapes,
               subject_shapes,
               src_points_by_curve,
               src_cyclic,
               op_params.subject_rule,
               mapping);
  SVG_add_path(f,
               type + "-B",
               src_points,
               src_shapes,
               clipping_shapes,
               src_points_by_curve,
               src_cyclic,
               op_params.clipping_rule,
               mapping);
  SVG_add_path(f,
               type + "-C",
               dst_points,
               dst_shapes,
               dst_shapes.index_range(),
               dst_points_by_curve,
               dst_cyclic,
               op_params.output_rule,
               mapping);

  f << "</svg>\n";
  f << "<h2>" << label << "</h2>\n";
  f << "</div>\n";
}

static bke::CurvesGeometry create_test_curves(const Span<int> offsets,
                                              const Span<float2> points,
                                              const Span<int> shape_ids,
                                              const Span<bool> cyclic,
                                              const Span<bool> fills)
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

  bke::SpanAttributeWriter<int> shape_id_writer = attributes.lookup_or_add_for_write_span<int>(
      "shape_id", bke::AttrDomain::Curve);
  shape_id_writer.span.copy_from(shape_ids);
  shape_id_writer.finish();

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
  EXPECT_EQ(dst_curves.curves_num(), expected_points.size());
  if (dst_curves.curves_num() != expected_points.size()) {
    return;
  }

  int total_size = 0;
  for (const int i : expected_points.index_range()) {
    total_size += expected_points[i].size();
  }

  const VArray<float2> dst_points = *dst_curves.attributes().lookup<float2>(
      ".positions_2d", bke::AttrDomain::Point);

  EXPECT_EQ(dst_points.size(), total_size);
  if (dst_points.size() != total_size) {
    return;
  }

  Array<float2> src_points(total_size);
  int i = 0;
  for (const int j : expected_points.index_range()) {
    for (const int k : expected_points[j].index_range()) {
      src_points[i] = expected_points[j][k];
      i++;
    }
  }

  const OffsetIndices<int> dst_points_by_curve = dst_curves.points_by_curve();

  /* Check if the all the points match. */
  auto do_points_match = [&](const IndexRange &points_i, const int curve_j) {
    for (const int point_i : points_i) {
      int src_point_i = -1;
      for (const int point_j : expected_points[curve_j].index_range()) {
        if (math::is_equal(dst_points[point_i], expected_points[curve_j][point_j], 1e-4f)) {
          src_point_i = point_j;
        }
      }
      /* No point could be found. */
      if (src_point_i == -1) {
        return false;
      }
    }

    return true;
  };

  Array<int> dst_to_src_curves(dst_curves.curves_num(), -1);
  /* Loop through all curves trying to find which other curve matches. */
  for (const int curve_i : dst_curves.curves_range()) {
    const IndexRange points_i = dst_points_by_curve[curve_i];
    for (const int curve_j : expected_points.index_range()) {
      if (do_points_match(points_i, curve_j)) {
        /* Only one curve should match. */
        EXPECT_EQ(dst_to_src_curves[curve_i], -1);

        dst_to_src_curves[curve_i] = curve_j;
      }
    }

    /* Some curve should always be found. */
    EXPECT_NE(dst_to_src_curves[curve_i], -1);
    if (dst_to_src_curves[curve_i] == -1) {
      return;
    }
  }
}

TEST(boolean_curves, Squares)
{
  draw_divider_start("Squares");

  const Array<float2> points = {{0, 0}, {2, 0}, {2, 2}, {0, 2}, {1, 1}, {3, 1}, {3, 3}, {1, 3}};
  const Array<int> points_by_curve = {0, 4, 8};
  const Array<bool> is_fill = {true, true};
  const Array<bool> is_cyclic = {true, true};
  const Array<int> shape_ids = {0, 1};
  const IndexRange clipping_shapes = IndexRange(1, 1);

  const bke::CurvesGeometry src_curves = create_test_curves(
      points_by_curve, points, shape_ids, is_cyclic, is_fill);

  geometry::boolean::CurveBooleanOpParameters op_params;
  op_params.subject_rule = FillRule::EvenOdd;
  op_params.clipping_rule = FillRule::EvenOdd;
  op_params.output_rule = FillRule::EvenOdd;

  {
    op_params.boolean_mode = Operation::Intersect;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{2, 2}, {1, 2}, {1, 1}, {2, 1}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Intersection", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Union;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 3}, {3, 3}, {3, 1}, {2, 1}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Union", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Difference;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{2, 0}, {0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Difference", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
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
  const Array<int> shape_ids = {0, 1};
  const IndexRange clipping_shapes = IndexRange(1, 1);

  const bke::CurvesGeometry src_curves = create_test_curves(
      points_by_curve, points, shape_ids, is_cyclic, is_fill);

  geometry::boolean::CurveBooleanOpParameters op_params;
  op_params.subject_rule = FillRule::EvenOdd;
  op_params.clipping_rule = FillRule::EvenOdd;
  op_params.output_rule = FillRule::EvenOdd;

  {
    op_params.boolean_mode = Operation::Intersect;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{5, 3}, {6, 4}, {6, 3}},
                                                   {{2, 3}, {2, 4}, {3, 3}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Intersection", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Union;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{8, 3}, {8, 6}, {0, 6}, {0, 3}, {2, 3}, {2, 0}, {6, 0}, {6, 3}},
        {{3, 3}, {4, 2}, {5, 3}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Union", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Difference;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{2, 3}, {0, 3}, {0, 6}, {8, 6}, {8, 3}, {6, 3}, {6, 4}, {5, 3}, {3, 3}, {2, 4}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Difference", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Union;
    op_params.output_rule = FillRule::NoHoles;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{8, 3}, {8, 6}, {0, 6}, {0, 3}, {2, 3}, {2, 0}, {6, 0}, {6, 3}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results(
        "Union Without Holes", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
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
  const Array<int> shape_ids = {0, 1};
  const IndexRange clipping_shapes = IndexRange(1, 1);

  const bke::CurvesGeometry src_curves = create_test_curves(
      points_by_curve, points, shape_ids, is_cyclic, is_fill);

  geometry::boolean::CurveBooleanOpParameters op_params;
  op_params.subject_rule = FillRule::EvenOdd;
  op_params.clipping_rule = FillRule::EvenOdd;
  op_params.output_rule = FillRule::EvenOdd;

  {
    op_params.boolean_mode = Operation::Intersect;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{12.3455, 1.47273}, {12.2, 1.8}, {12.4851, 1.67327}, {12.5663, 1.40964}},
        {{6.71134, 3.08247}, {6.95349, 4.13178}, {7.32258, 3.96774}, {7, 3}},
        {{9.30137, 8.32192}, {9.45361, 7.97938}, {10.4135, 8.40602}, {10.3267, 8.68812}},
        {{7.79641, 7.78443}, {7.65714, 7.18095}, {8.52174, 7.56522}, {8.7027, 8.10811}},
        {{10.3333, 6}, {10.5059, 5.61176}, {11.2479, 5.69421}, {11.1538, 6}},
        {{7.38462, 6}, {7.21053, 5.24561}, {7.76923, 5.30769}, {8, 6}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Intersection", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Union;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

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

    draw_results("Union", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Difference;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

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

    draw_results("Difference", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Union;
    op_params.output_rule = FillRule::NoHoles;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{14, 1},
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
                                                    {12.5663, 1.40964}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results(
        "Union Without Holes", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
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
      {0, 5}, {0, 0}, {7, 0}, {7, 5}, {2, 3}, {0, 7}, {3, 7}, {6, 3}, {7, 6}, {3, 3}, {2, 6}};
  const Array<int> points_by_curve = {0, 4, 11};
  const Array<bool> is_fill = {true, true};
  const Array<bool> is_cyclic = {true, true};
  const Array<int> shape_ids = {0, 1};
  const IndexRange clipping_shapes = IndexRange(1, 1);

  const bke::CurvesGeometry src_curves = create_test_curves(
      points_by_curve, points, shape_ids, is_cyclic, is_fill);

  geometry::boolean::CurveBooleanOpParameters op_params;
  op_params.subject_rule = FillRule::EvenOdd;
  op_params.clipping_rule = FillRule::EvenOdd;
  op_params.output_rule = FillRule::EvenOdd;

  {
    op_params.boolean_mode = Operation::Intersect;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{6.66667, 5}, {5.66667, 5}, {3, 3}, {2.33333, 5}, {4.5, 5}, {6, 3}},
        {{2, 5}, {1, 5}, {2, 3}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Intersection", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Union;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{1, 5},
                                                    {0, 5},
                                                    {0, 0},
                                                    {7, 0},
                                                    {7, 5},
                                                    {6.66667, 5},
                                                    {7, 6},
                                                    {5.66667, 5},
                                                    {4.5, 5},
                                                    {3, 7},
                                                    {0, 7}},
                                                   {{2.33333, 5}, {2, 5}, {2, 6}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Union", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Difference;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{1, 5},
                                                    {0, 5},
                                                    {0, 0},
                                                    {7, 0},
                                                    {7, 5},
                                                    {6.66667, 5},
                                                    {6, 3},
                                                    {4.5, 5},
                                                    {5.66667, 5},
                                                    {3, 3},
                                                    {2.33333, 5},
                                                    {2, 5},
                                                    {2, 3}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Difference", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }

  draw_divider_end();
}

TEST(boolean_curves, Simple_Cuts)
{
  draw_divider_start("Cuts");

  geometry::boolean::CurveBooleanOpParameters op_params;
  op_params.subject_rule = FillRule::EvenOdd;
  op_params.clipping_rule = FillRule::EvenOdd;
  op_params.output_rule = FillRule::EvenOdd;
  op_params.boolean_mode = Operation::Difference;

  {
    const Array<float2> points = {
        {5, 7}, {3, 6}, {0, 2}, {0, 0}, {1, 6}, {3, 4}, {3, 1}, {0, 4}, {2, 3}};
    const Array<int> points_by_curve = {0, 4, 9};
    const Array<bool> is_fill = {false, true};
    const Array<bool> is_cyclic = {false, true};
    const Array<int> shape_ids = {0, 1};
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(
        points_by_curve, points, shape_ids, is_cyclic, is_fill);

    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{5, 7}, {3, 6}, {2.14286, 4.85714}},
                                                   {{1.61538, 4.15385}, {1.09091, 3.45455}},
                                                   {{0.857143, 3.14286}, {0, 2}, {0, 0}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Simple Cut 1", "cut", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    const Array<float2> points = {{5, 5}, {3, 5}, {1, 3}, {1, 1}, {5, 6}, {6, 5}, {1, 0}, {0, 1}};
    const Array<int> points_by_curve = {0, 4, 8};
    const Array<bool> is_fill = {false, true};
    const Array<bool> is_cyclic = {false, true};
    const Array<int> shape_ids = {0, 1};
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(
        points_by_curve, points, shape_ids, is_cyclic, is_fill);

    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{4, 5}, {3, 5}, {1, 3}, {1, 2}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Simple Cut 2", "cut", src_curves, dst_curves, clipping_shapes, op_params);
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
    const Array<int> shape_ids = {0, 1};
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(
        points_by_curve, points, shape_ids, is_cyclic, is_fill);

    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{6, 8}, {4, 7}, {3.57143, 6.42857}},
                                                   {{3.4, 6.2}, {2.90909, 5.54545}},
                                                   {{2.5, 5}, {2.09091, 4.45455}},
                                                   {{1.6, 3.8}, {1.27273, 3.36364}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Simple Cut 3", "cut", src_curves, dst_curves, clipping_shapes, op_params);
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
    const Array<int> shape_ids = {0, 1};
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(
        points_by_curve, points, shape_ids, is_cyclic, is_fill);

    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{3.7, 5.6}, {3.45455, 5.27273}},
                                                   {{2.8, 4.4}, {2.63636, 4.18182}},
                                                   {{1.9, 3.2}, {1.81818, 3.09091}},
                                                   {{1.42857, 2.57143}, {1, 2}, {1, 0}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Simple Cut 4", "cut", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    const Array<float2> points = {
        {6, 5}, {4, 5}, {1, 2}, {1, 0}, {1, 4}, {3, 1}, {5, 3}, {2, 5}, {3, 3}};
    const Array<int> points_by_curve = {0, 4, 9};
    const Array<bool> is_fill = {false, true};
    const Array<bool> is_cyclic = {true, true};
    const Array<int> shape_ids = {0, 1};
    const IndexRange clipping_shapes = IndexRange(1, 1);

    const bke::CurvesGeometry src_curves = create_test_curves(
        points_by_curve, points, shape_ids, is_cyclic, is_fill);

    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{4.4, 3.4}, {6, 5}, {4, 5}, {3.2, 4.2}},
                                                   {{2.66667, 3.66667}, {2.33333, 3.33333}},
                                                   {{1.8, 2.8}, {1, 2}, {1, 0}, {2.6, 1.6}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Cyclical Cut", "cut", src_curves, dst_curves, clipping_shapes, op_params);
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
  const Array<bool> is_fill = {true, true, true, true};
  const Array<bool> is_cyclic = {true, true, true, true};
  const Array<int> shape_ids = {0, 0, 1, 1};
  const IndexRange clipping_shapes = IndexRange::from_begin_end(1, 2);

  const bke::CurvesGeometry src_curves = create_test_curves(
      points_by_curve, points, shape_ids, is_cyclic, is_fill);

  geometry::boolean::CurveBooleanOpParameters op_params;
  op_params.subject_rule = FillRule::EvenOdd;
  op_params.clipping_rule = FillRule::EvenOdd;
  op_params.output_rule = FillRule::EvenOdd;

  {
    op_params.boolean_mode = Operation::Intersect;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{2, 5}, {3, 5}, {3, 4}, {2, 4}},
                                                   {{5, 3}, {5, 2}, {4, 2}, {4, 3}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Intersection", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Union;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{5, 2}, {5, 0}, {0, 0}, {0, 5}, {2, 5}, {2, 7}, {7, 7}, {7, 2}},
        {{3, 5}, {5, 5}, {5, 3}, {6, 3}, {6, 6}, {3, 6}},
        {{4, 2}, {4, 1}, {1, 1}, {1, 4}, {2, 4}, {2, 2}},
        {{3, 4}, {4, 4}, {4, 3}, {3, 3}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Union", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Difference;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{5, 2}, {5, 0}, {0, 0}, {0, 5}, {2, 5}, {2, 4}, {1, 4}, {1, 1}, {4, 1}, {4, 2}},
        {{3, 5}, {5, 5}, {5, 3}, {4, 3}, {4, 4}, {3, 4}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Difference", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
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
  const Array<int> shape_ids = {0, 1, 2};

  const bke::CurvesGeometry src_curves = create_test_curves(
      points_by_curve, points, shape_ids, is_cyclic, is_fill);

  geometry::boolean::CurveBooleanOpParameters op_params;
  op_params.subject_rule = FillRule::EvenOdd;
  op_params.clipping_rule = FillRule::EvenOdd;
  op_params.output_rule = FillRule::EvenOdd;

  /**
   * Multiple separate but intersecting subject shapes.
   * The two subject shapes should be affected by the clipping shape, but not join into one.
   */
  {
    op_params.boolean_mode = Operation::Intersect;
    const IndexRange clipping_shapes = IndexRange::from_begin_end(2, 3);
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {{{3, 7}, {5, 7}, {5, 3}, {3, 3}},
                                                   {{3, 5}, {7, 5}, {7, 3}, {3, 3}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results(
        "2 Subjects Intersection", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Difference;
    const IndexRange clipping_shapes = IndexRange::from_begin_end(2, 3);
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{5, 3}, {5, 2}, {0, 2}, {0, 7}, {3, 7}, {3, 3}},
        {{7, 3}, {7, 0}, {2, 0}, {2, 5}, {3, 5}, {3, 3}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results(
        "2 Subjects Difference", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }

  /**
   * Multiple separate but intersecting clipping shapes.
   * The subject shape should be affected as if the two clipping shapes were union.
   */
  {
    op_params.boolean_mode = Operation::Intersect;
    const IndexRange clipping_shapes = IndexRange::from_begin_end(0, 2);
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{5, 3}, {3, 3}, {3, 5}, {3, 7}, {5, 7}, {5, 5}, {7, 5}, {7, 3}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results(
        "2 Clipping Intersection", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Difference;
    const IndexRange clipping_shapes = IndexRange::from_begin_end(0, 2);
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{3, 7}, {3, 8}, {8, 8}, {8, 3}, {7, 3}, {7, 5}, {5, 5}, {5, 7}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results(
        "2 Clipping Difference", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  draw_divider_end();
}

TEST(boolean_curves, Four_Shapes)
{
  draw_divider_start("Four Shapes");

  const Array<float2> points = {
      {0, 2},
      {0, 7},
      {5, 7},
      {5, 2},

      {2, 0},
      {2, 5},
      {7, 5},
      {7, 0},

      {1, 3},
      {1, 8},
      {6, 8},
      {6, 3},

      {3, 1},
      {3, 6},
      {8, 6},
      {8, 1},
  };
  const Array<int> points_by_curve = {0, 4, 8, 12, 16};
  const Array<bool> is_fill = {true, true, true, true};
  const Array<bool> is_cyclic = {true, true, true, true};
  const Array<int> shape_ids = {0, 1, 2, 3};
  const IndexRange clipping_shapes = IndexRange::from_begin_end(2, 4);

  const bke::CurvesGeometry src_curves = create_test_curves(
      points_by_curve, points, shape_ids, is_cyclic, is_fill);

  geometry::boolean::CurveBooleanOpParameters op_params;
  op_params.subject_rule = FillRule::EvenOdd;
  op_params.clipping_rule = FillRule::EvenOdd;
  op_params.output_rule = FillRule::EvenOdd;

  {
    op_params.boolean_mode = Operation::Intersect;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    /* TODO(@casey-bianco-davis): Remove the unneeded clipping/clipping points. */
    const Array<Vector<float2>> expected_points = {
        {{1, 7}, {5, 7}, {5, 6}, {5, 3}, {5, 2}, {3, 2}, {3, 3}, {1, 3}},
        {{2, 3}, {2, 5}, {3, 5}, {6, 5}, {7, 5}, {7, 1}, {3, 1}, {3, 3}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Intersection", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }
  {
    op_params.boolean_mode = Operation::Difference;
    const bke::CurvesGeometry dst_curves = curve_boolean(op_params, src_curves, clipping_shapes);

    const Array<Vector<float2>> expected_points = {
        {{3, 2}, {0, 2}, {0, 7}, {1, 7}, {1, 3}, {3, 3}},
        {{7, 1}, {7, 0}, {2, 0}, {2, 3}, {3, 3}, {3, 1}}};
    expect_boolean_result_coord(dst_curves, expected_points);

    draw_results("Difference", "polygon", src_curves, dst_curves, clipping_shapes, op_params);
  }

  draw_divider_end();
}

}  // namespace blender::geometry::tests
