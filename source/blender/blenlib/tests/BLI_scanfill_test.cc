/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_listbase_iterator.hh"
#include "BLI_scanfill.h"
#include "BLI_serialize.hh"
#include "BLI_vector.hh"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace blender::tests {

/**
 * Check if an environment variable is set to a truthy value.
 * Returns true if set, not empty, and not "0".
 */
static bool env_var_as_bool(const char *name)
{
  const char *env = std::getenv(name);
  if (env == nullptr || env[0] == '\0') {
    return false;
  }
  if (env[0] == '0' && env[1] == '\0') {
    return false;
  }
  return true;
}

/**
 * Check if legacy scanfill method should be used.
 * Set USE_SCANFILL_LEGACY=1 environment variable to enable.
 */
static bool use_scanfill_legacy()
{
  return env_var_as_bool("USE_SCANFILL_LEGACY");
}

/**
 * Check if Skia triangulator should be used.
 * Set USE_SKIA_TRIANGULATOR=1 environment variable to enable.
 * Only effective when built with WITH_SKIA_TRIANGULATOR.
 */
static bool use_skia_triangulator()
{
  return env_var_as_bool("USE_SKIA_TRIANGULATOR");
}

/* -------------------------------------------------------------------- */
/** \name Transform Permutations
 * \{ */

enum class XForm {
  NONE = 0,
  ROTATE_90 = 1,
  ROTATE_180 = 2,
  ROTATE_270 = 3,
  ROTATE_45 = 4,
  ROTATE_22_5 = 5,
  ROTATE_11_25 = 6,
  FLIP_X = 7,
  FLIP_Y = 8,
  NUM = 9,
};

static const char *xform_name(XForm xf)
{
  switch (xf) {
    case XForm::NONE:
      return "";
    case XForm::ROTATE_90:
      return "rotate_90";
    case XForm::ROTATE_180:
      return "rotate_180";
    case XForm::ROTATE_270:
      return "rotate_270";
    case XForm::ROTATE_45:
      return "rotate_45";
    case XForm::ROTATE_22_5:
      return "rotate_22_5";
    case XForm::ROTATE_11_25:
      return "rotate_11_25";
    case XForm::FLIP_X:
      return "flip_x";
    case XForm::FLIP_Y:
      return "flip_y";
    default:
      return "";
  }
}

/**
 * Transform vertices by rotation angle in radians.
 */
static void xform_verts_by_angle(float angle, Vector<std::array<float, 2>> &verts)
{
  const float cos_a = std::cos(angle);
  const float sin_a = std::sin(angle);
  for (auto &v : verts) {
    float x = v[0] * cos_a - v[1] * sin_a;
    float y = v[0] * sin_a + v[1] * cos_a;
    v[0] = x;
    v[1] = y;
  }
}

/**
 * Apply transform to vertices.
 */
static void xform_verts(XForm xf, Vector<std::array<float, 2>> &verts)
{
  switch (xf) {
    case XForm::NONE:
      break;
    case XForm::ROTATE_90:
      for (auto &v : verts) {
        float x = v[1];
        float y = -v[0];
        v[0] = x;
        v[1] = y;
      }
      break;
    case XForm::ROTATE_180:
      for (auto &v : verts) {
        v[0] = -v[0];
        v[1] = -v[1];
      }
      break;
    case XForm::ROTATE_270:
      for (auto &v : verts) {
        float x = -v[1];
        float y = v[0];
        v[0] = x;
        v[1] = y;
      }
      break;
    case XForm::ROTATE_45:
      xform_verts_by_angle(float(M_PI) / 4.0f, verts);
      break;
    case XForm::ROTATE_22_5:
      xform_verts_by_angle(float(M_PI) / 8.0f, verts);
      break;
    case XForm::ROTATE_11_25:
      xform_verts_by_angle(float(M_PI) / 16.0f, verts);
      break;
    case XForm::FLIP_X:
      for (auto &v : verts) {
        v[0] = -v[0];
      }
      break;
    case XForm::FLIP_Y:
      for (auto &v : verts) {
        v[1] = -v[1];
      }
      break;
    default:
      break;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Disabled Test Combinations
 *
 * Tests that are known to fail with specific transforms.
 * Format: test name with optional transform suffix (e.g. "test_name_rotate_90").
 * \{ */

static const char *disabled_tests[] = {
    /* Non-degenerate test failures. */
    "poly_fill_axis_align_steps_whole_subdiv_2x_03_rotate_45",
    /* TODO: `poly_fill_axis_aligned_sweepline_bug_01` - investigate the failure. */
    "poly_fill_axis_aligned_sweepline_bug_01",
    "poly_fill_axis_aligned_sweepline_bug_01_rotate_90",
    "poly_fill_axis_aligned_sweepline_bug_01_rotate_180",
    "poly_fill_axis_aligned_sweepline_bug_01_rotate_270",
    "poly_fill_axis_aligned_sweepline_bug_01_flip_x",
    "poly_fill_axis_aligned_sweepline_bug_01_flip_y",
};

static bool is_test_disabled(const char *filename, XForm xform)
{
  /* Build the full test name with transform suffix. */
  const char *xf_name = xform_name(xform);
  std::string test_name = filename;
  if (xf_name[0] != '\0') {
    test_name += "_";
    test_name += xf_name;
  }

  for (const char *disabled : disabled_tests) {
    if (test_name == disabled) {
      return true;
    }
  }
  return false;
}

/** \} */

/**
 * Check if SVG debug output is enabled via environment variable.
 * Set USE_DEBUG_SVG_OUTPUT to a non-empty value (not "0") to enable.
 */
static bool use_debug_svg_output()
{
  return env_var_as_bool("USE_DEBUG_SVG_OUTPUT");
}

/* -------------------------------------------------------------------- */
/** \name Test Data Structure
 * \{ */

struct TestData {
  const char *filename;
  int tris;
  double area;
  int area_places = 7;
  bool degenerate = false;
  bool adjacent = false;
};

/**
 * Combines test data with a transform permutation.
 */
struct TestDataWithXForm {
  TestData data;
  XForm xform;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Test Data Definitions
 * \{ */

static const TestData tests_data[] = {
    {"poly_fill_adjacent_squares_01", 4, 1.0, 7, false, true},
    {"poly_fill_adjacent_squares_02", 8, 1.0, 7, false, true},
    {"poly_fill_adjacent_squares_03", 32, 4.0, 7, false, true},
    {"poly_fill_adjacent_tris_01", 2, 0.5, 7, false, true},
    {"poly_fill_adjacent_tris_02", 4, 0.5, 7, false, true},
    {"poly_fill_adjacent_tris_03", 32, 4.0, 7, false, true},
    {"poly_fill_adjacent_tris_04", 20, 4.0, 7, false, true},
    {"poly_fill_align_holes_02", 26, 1.68},
    {"poly_fill_align_holes_03", 74, 1.76},
    {"poly_fill_align_holes_aabb_01", 14, 1.8},
    {"poly_fill_align_holes_aabb_02", 26, 1.64},
    {"poly_fill_align_holes_aabb_03", 74, 1.72},
    {"poly_fill_align_holes_aabb_04", 12, 2.18},
    {"poly_fill_align_holes_aabb_05", 42, 1.28},
    {"poly_fill_align_holes_aabb_06", 98, 3.68},
    {"poly_fill_align_holes_aabb_alt_01", 14, 1.68},
    {"poly_fill_align_holes_aabb_alt_02", 26, 1.64},
    {"poly_fill_align_holes_aabb_alt_03", 74, 1.52},
#if 0
    /* NOTE(@ideasman42): Known failing case. The problem is that touching "holes"
     * are not supported, supporting them is possible but would require pre-processing
     * the polygon so two touching shapes would form a larger shape instead of 
     * two isolated shapes. */
    {"poly_fill_align_holes_aabb_overlap_01", 20, 1.2, 7, true},
#endif
    {"poly_fill_align_y_events_01", 38, 0.787547779338},
    {"poly_fill_axis_align_max_01", 36, 3.560722616582},
    {"poly_fill_axis_align_min_01", 36, 3.56072279169},
    {"poly_fill_axis_align_steps_whole_subdiv_2x_02", 214, 1.4},
    {"poly_fill_axis_align_steps_whole_subdiv_2x_03", 256, 1.14},
    {"poly_fill_axis_aligned_sweepline_bug_01", 14, 1.1125182102151905},
    {"poly_fill_circle_bent_2x_start", 30, 1.1845165512325},
    {"poly_fill_circle_non_aligned", 30, 3.121443994996},
    {"poly_fill_circle_non_aligned_x2_diag", 60, 0.9988627689815},
    {"poly_fill_circle_non_aligned_x2_horiz", 60, 0.998863116364},
    {"poly_fill_circle_non_aligned_x2_vert", 60, 0.9988630492425},
    {"poly_fill_collapse_test_01", 324, 0.4561171421215},
    {"poly_fill_complex_test_02", 62, 1.01955},
    {"poly_fill_ear_clip_detail_01", 401, 1.833274865},
    {"poly_fill_ear_clip_detail_co_linear_01", 130, 1.832945705},
    {"poly_fill_hole_complex", 160, 1.77798808872},
    {"poly_fill_hole_complex_subdiv", 1280, 1.77798620261},
    {"poly_fill_hole_simple", 8, 1.5},
    {"poly_fill_hole_simple_rotate", 8, 1.50000041665},
    {"poly_fill_hole_simple_subdiv", 16, 1.875},
    {"poly_fill_hole_simple_subdiv_rotate", 16, 1.875000903496},
    {"poly_fill_overlap_coord_2x_ymax_01", 6, 0.04},
    {"poly_fill_overlap_coord_2x_ymin_01", 6, 0.04},
    {"poly_fill_overlap_coord_quad_corners_01", 8, 1.44},
    {"poly_fill_overlap_coord_quad_corners_rotate_01", 8, 0.72},
    {"poly_fill_overlap_coord_3x_ymax_01", 9, 0.06},
    {"poly_fill_overlap_coord_3x_ymin_01", 9, 0.06},
    {"poly_fill_overlap_coord_8x_ymax_01", 24, 0.16},
    {"poly_fill_overlap_coord_internal_circle_01", 50, 2.53},
    {"poly_fill_overlap_test_01", 11, 0.297994620954},
    {"poly_fill_overlap_test_subdiv_01", 32, 0.309955273155},
    {"poly_fill_primitive_square_01", 2, 1.0},
    {"poly_fill_primitive_triangle_01", 1, 1.0},
    {"poly_fill_primitive_triangle_02", 1, 0.5},
    {"poly_fill_tilted_staircase_01", 6, 0.6875},
    {"poly_fill_stress_test_sweepline_01", 6, 1.23},
#if 1 /* A valid test but quite slow. */
    {"poly_fill_stress_test_sweepline_02", 95211, 0.6013724715115001},
#endif

    /* Degenerate tests - only verify no crash. */
    {"poly_fill_degenerate_bowtie_01", -1, -1.0, 7, true},
    {"poly_fill_degenerate_bowtie_02", -1, -1.0, 7, true},
    {"poly_fill_degenerate_square_is_edge_01", -1, -1.0, 7, true},
    {"poly_fill_degenerate_square_is_edge_02", -1, -1.0, 7, true},
    {"poly_fill_degenerate_square_is_point_01", -1, -1.0, 7, true},
    {"poly_fill_degenerate_triangle_is_edge_01", -1, -1.0, 7, true},
    {"poly_fill_degenerate_triangle_is_point_01", -1, -1.0, 7, true},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Utility Functions
 * \{ */

/**
 * Calculate signed area of a 2D triangle.
 */
static double tri_area_2d(const double v0[2], const double v1[2], const double v2[2])
{
  double d2[2] = {v1[0] - v0[0], v1[1] - v0[1]};
  double d3[2] = {v2[0] - v0[0], v2[1] - v0[1]};
  return std::abs((d2[0] * d3[1]) - (d3[0] * d2[1])) / 2.0;
}

/**
 * Get the path to the test data directory.
 */
static std::string get_test_data_dir()
{
  /* The test data is located relative to the source file. */
  return std::string(STRINGIFY(TEST_DATA_DIR));
}

/**
 * Load polygon data from a JSON file.
 *
 * Returns true on success, populating verts and edges vectors.
 */
static bool load_polygon_json(const std::string &filepath,
                              Vector<std::array<float, 2>> &verts,
                              Vector<std::array<int, 2>> &edges)
{
  std::ifstream ifs(filepath);
  if (!ifs.is_open()) {
    return false;
  }

  io::serialize::JsonFormatter json;
  std::unique_ptr<io::serialize::Value> root = json.deserialize(ifs);
  if (!root) {
    return false;
  }

  const io::serialize::DictionaryValue *dict = root->as_dictionary_value();
  if (!dict) {
    return false;
  }

  /* Load vertices. */
  const io::serialize::ArrayValue *verts_array = dict->lookup_array("verts");
  if (!verts_array) {
    return false;
  }

  for (const std::shared_ptr<io::serialize::Value> &vert_value : verts_array->elements()) {
    const io::serialize::ArrayValue *vert_array = vert_value->as_array_value();
    if (!vert_array || vert_array->elements().size() < 2) {
      return false;
    }

    std::array<float, 2> co;
    for (int i = 0; i < 2; i++) {
      const io::serialize::Value *coord = vert_array->elements()[i].get();
      if (const io::serialize::DoubleValue *dval = coord->as_double_value()) {
        co[i] = float(dval->value());
      }
      else if (const io::serialize::IntValue *ival = coord->as_int_value()) {
        co[i] = float(ival->value());
      }
      else {
        return false;
      }
    }
    verts.append(co);
  }

  /* Load edges. */
  const io::serialize::ArrayValue *edges_array = dict->lookup_array("edges");
  if (!edges_array) {
    return false;
  }

  for (const std::shared_ptr<io::serialize::Value> &edge_value : edges_array->elements()) {
    const io::serialize::ArrayValue *edge_array = edge_value->as_array_value();
    if (!edge_array || edge_array->elements().size() < 2) {
      return false;
    }

    std::array<int, 2> edge;
    for (int i = 0; i < 2; i++) {
      const io::serialize::IntValue *ival = edge_array->elements()[i]->as_int_value();
      if (!ival) {
        return false;
      }
      edge[i] = int(ival->value());
    }
    edges.append(edge);
  }

  return true;
}

/**
 * Write triangulated result as SVG with semi-transparent triangles.
 *
 * \param filepath: Output SVG file path.
 * \param verts: Vertex coordinates.
 * \param faces: Triangle indices (each array is 3 vertex indices).
 * \param failed: If true, draw red background to indicate test failure.
 */
static void write_svg(const std::string &filepath,
                      const Vector<std::array<float, 2>> &verts,
                      const Vector<std::array<uint, 3>> &faces,
                      bool failed = false)
{
  if (verts.is_empty()) {
    return;
  }

  /* Calculate bounds. */
  float min_x = verts[0][0], max_x = verts[0][0];
  float min_y = verts[0][1], max_y = verts[0][1];
  for (const auto &v : verts) {
    min_x = std::min(min_x, v[0]);
    max_x = std::max(max_x, v[0]);
    min_y = std::min(min_y, v[1]);
    max_y = std::max(max_y, v[1]);
  }

  float width = max_x - min_x;
  float height = max_y - min_y;

  if (width == 0.0f || height == 0.0f) {
    return;
  }

  /* Add padding. */
  float padding = std::max(width, height) * 0.05f;
  min_x -= padding;
  min_y -= padding;
  width += padding * 2;
  height += padding * 2;

  /* Scale to reasonable SVG size. */
  float scale = 500.0f / std::max(width, height);
  float svg_width = width * scale;
  float svg_height = height * scale;

  std::ofstream ofs(filepath);
  if (!ofs.is_open()) {
    return;
  }

  /* SVG header. */
  ofs << "<svg xmlns=\"http://www.w3.org/2000/svg\" ";
  ofs << "width=\"" << svg_width << "\" height=\"" << svg_height << "\" ";
  ofs << "viewBox=\"" << min_x << " " << min_y << " " << width << " " << height << "\">\n";

  /* Red background for failed tests. */
  if (failed) {
    ofs << "<rect x=\"" << min_x << "\" y=\"" << min_y << "\" ";
    ofs << "width=\"" << width << "\" height=\"" << height << "\" ";
    ofs << "fill=\"#ffcccc\"/>\n";
  }

  /* Flip Y axis (SVG has Y down, geometry has Y up). */
  ofs << "<g transform=\"translate(0," << (min_y + max_y + padding * 2) << ") scale(1,-1)\">\n";

  /* Draw triangles with 10% alpha. */
  float stroke_width = 0.002f * std::max(width, height);
  for (const auto &tri : faces) {
    if (tri[0] >= verts.size() || tri[1] >= verts.size() || tri[2] >= verts.size()) {
      continue;
    }
    const auto &p0 = verts[tri[0]];
    const auto &p1 = verts[tri[1]];
    const auto &p2 = verts[tri[2]];
    ofs << "<polygon points=\"";
    ofs << p0[0] << "," << p0[1] << " ";
    ofs << p1[0] << "," << p1[1] << " ";
    ofs << p2[0] << "," << p2[1] << "\" ";
    ofs << "fill=\"blue\" fill-opacity=\"0.1\" ";
    ofs << "stroke=\"black\" stroke-width=\"" << stroke_width << "\"/>\n";
  }

  ofs << "</g>\n";
  ofs << "</svg>\n";
}

/**
 * Write input polygon edges as SVG (no triangulation).
 *
 * \param filepath: Output SVG file path.
 * \param verts: Vertex coordinates.
 * \param edges: Edge indices (pairs of vertex indices).
 */
static void write_svg_poly(const std::string &filepath,
                           const Vector<std::array<float, 2>> &verts,
                           const Vector<std::array<int, 2>> &edges)
{
  if (verts.is_empty() || edges.is_empty()) {
    return;
  }

  /* Calculate bounds. */
  float min_x = verts[0][0], max_x = verts[0][0];
  float min_y = verts[0][1], max_y = verts[0][1];
  for (const auto &v : verts) {
    min_x = std::min(min_x, v[0]);
    max_x = std::max(max_x, v[0]);
    min_y = std::min(min_y, v[1]);
    max_y = std::max(max_y, v[1]);
  }

  float width = max_x - min_x;
  float height = max_y - min_y;

  if (width == 0.0f || height == 0.0f) {
    return;
  }

  /* Add padding. */
  float padding = std::max(width, height) * 0.05f;
  min_x -= padding;
  min_y -= padding;
  width += padding * 2;
  height += padding * 2;

  /* Scale to reasonable SVG size. */
  float scale = 500.0f / std::max(width, height);
  float svg_width = width * scale;
  float svg_height = height * scale;

  std::ofstream ofs(filepath);
  if (!ofs.is_open()) {
    return;
  }

  /* SVG header. */
  ofs << "<svg xmlns=\"http://www.w3.org/2000/svg\" ";
  ofs << "width=\"" << svg_width << "\" height=\"" << svg_height << "\" ";
  ofs << "viewBox=\"" << min_x << " " << min_y << " " << width << " " << height << "\">\n";

  /* Flip Y axis (SVG has Y down, geometry has Y up). */
  ofs << "<g transform=\"translate(0," << (min_y + max_y + padding * 2) << ") scale(1,-1)\">\n";

  /* Draw edges. */
  float stroke_width = 0.002f * std::max(width, height);
  for (const auto &edge : edges) {
    if (edge[0] < 0 || edge[0] >= int(verts.size()) || edge[1] < 0 ||
        edge[1] >= int(verts.size()))
    {
      continue;
    }
    const auto &p0 = verts[edge[0]];
    const auto &p1 = verts[edge[1]];
    ofs << "<line x1=\"" << p0[0] << "\" y1=\"" << p0[1] << "\" ";
    ofs << "x2=\"" << p1[0] << "\" y2=\"" << p1[1] << "\" ";
    ofs << "stroke=\"black\" stroke-width=\"" << stroke_width << "\"/>\n";
  }

  ofs << "</g>\n";
  ofs << "</svg>\n";
}

/**
 * Run scanfill on polygon data and return the number of triangles and total area.
 * Optionally collects face indices for SVG output.
 *
 * \param is_degenerate: If false, pass BLI_SCANFILL_POLY_IS_NOT_DEGENERATE to enable
 *                       assertions in poly_fill that check for valid input.
 */
static bool run_scanfill(const Vector<std::array<float, 2>> &verts,
                         const Vector<std::array<int, 2>> &edges,
                         uint &out_num_tris,
                         double &out_area,
                         bool is_degenerate,
                         Vector<std::array<uint, 3>> *out_faces = nullptr)
{
  if (verts.is_empty() || edges.is_empty()) {
    out_num_tris = 0;
    out_area = 0.0;
    return true;
  }

  ScanFillContext sf_ctx;
  BLI_scanfill_begin(&sf_ctx);

  /* Create vertices. */
  Vector<ScanFillVert *> sf_verts;
  sf_verts.reserve(verts.size());
  for (const std::array<float, 2> &co : verts) {
    float co3[3] = {co[0], co[1], 0.0f};
    ScanFillVert *sv = BLI_scanfill_vert_add(&sf_ctx, co3);
    sv->keyindex = uint(sf_verts.size());
    sf_verts.append(sv);
  }

  /* Create edges. */
  for (const std::array<int, 2> &edge : edges) {
    if (edge[0] >= 0 && edge[0] < int(sf_verts.size()) && edge[1] >= 0 &&
        edge[1] < int(sf_verts.size()))
    {
      BLI_scanfill_edge_add(&sf_ctx, sf_verts[edge[0]], sf_verts[edge[1]]);
    }
  }

  /* Run scanfill. */
  const float nor[3] = {0.0f, 0.0f, 1.0f};
  int flag = BLI_SCANFILL_CALC_HOLES;
  if (!is_degenerate) {
    flag |= BLI_SCANFILL_POLY_IS_NOT_DEGENERATE;
  }
  if (use_scanfill_legacy()) {
    flag |= BLI_SCANFILL_LEGACY_METHOD;
  }
  if (use_skia_triangulator()) {
    flag |= BLI_SCANFILL_USE_SKIA_TRIANGULATOR;
  }
  out_num_tris = BLI_scanfill_calc_ex(&sf_ctx, flag, nor);

  /* Calculate total area from resulting triangles (use double for precision). */
  out_area = 0.0;
  for (const ScanFillFace &face : sf_ctx.fillfacebase) {
    double v0[2] = {double(verts[face.v1->keyindex][0]), double(verts[face.v1->keyindex][1])};
    double v1[2] = {double(verts[face.v2->keyindex][0]), double(verts[face.v2->keyindex][1])};
    double v2[2] = {double(verts[face.v3->keyindex][0]), double(verts[face.v3->keyindex][1])};
    out_area += tri_area_2d(v0, v1, v2);

    if (out_faces) {
      out_faces->append({face.v1->keyindex, face.v2->keyindex, face.v3->keyindex});
    }
  }

  BLI_scanfill_end(&sf_ctx);

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Simple Unit Tests
 * \{ */

TEST(scanfill, Square)
{
  ScanFillContext sf_ctx;
  BLI_scanfill_begin(&sf_ctx);

  const float coords[][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};

  ScanFillVert *verts[4];
  for (int i = 0; i < 4; i++) {
    verts[i] = BLI_scanfill_vert_add(&sf_ctx, coords[i]);
  }

  for (int i = 0; i < 4; i++) {
    BLI_scanfill_edge_add(&sf_ctx, verts[i], verts[(i + 1) % 4]);
  }

  uint num_tris = BLI_scanfill_calc(&sf_ctx, 0);
  EXPECT_EQ(num_tris, 2);

  BLI_scanfill_end(&sf_ctx);
}

TEST(scanfill, Triangle)
{
  ScanFillContext sf_ctx;
  BLI_scanfill_begin(&sf_ctx);

  const float coords[][3] = {{0, 0, 0}, {1, 0, 0}, {0.5f, 1, 0}};

  ScanFillVert *verts[3];
  for (int i = 0; i < 3; i++) {
    verts[i] = BLI_scanfill_vert_add(&sf_ctx, coords[i]);
  }

  for (int i = 0; i < 3; i++) {
    BLI_scanfill_edge_add(&sf_ctx, verts[i], verts[(i + 1) % 3]);
  }

  uint num_tris = BLI_scanfill_calc(&sf_ctx, 0);
  EXPECT_EQ(num_tris, 1);

  BLI_scanfill_end(&sf_ctx);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Parameterized Tests from JSON Data
 * \{ */

class ScanFillDataTest : public ::testing::TestWithParam<TestDataWithXForm> {};

TEST_P(ScanFillDataTest, FillPolygon)
{
  const TestDataWithXForm &param = GetParam();
  const TestData &t = param.data;
  const XForm xf = param.xform;

  /* Build path to JSON file. */
  std::string data_dir = get_test_data_dir();
  std::string filepath = data_dir + "/" + t.filename + ".json";

  /* Load polygon data. */
  Vector<std::array<float, 2>> verts;
  Vector<std::array<int, 2>> edges;
  bool loaded = load_polygon_json(filepath, verts, edges);

  /* Skip test if file doesn't exist. */
  if (!loaded) {
    GTEST_SKIP() << "Test data file not found: " << filepath;
  }

  /* Apply transform to vertices. */
  xform_verts(xf, verts);

  /* Run scanfill. */
  uint num_tris = 0;
  double area = 0.0;
  Vector<std::array<uint, 3>> faces;
  const bool write_svg_output = use_debug_svg_output();
  bool success = run_scanfill(
      verts, edges, num_tris, area, t.degenerate, write_svg_output ? &faces : nullptr);
  EXPECT_TRUE(success);

  if (write_svg_output) {
    /* Determine if test will fail for SVG background color. */
    bool test_failed = false;
    if (!t.degenerate) {
      /* Match tolerance adjustment used for actual test assertions. */
      int area_places = t.area_places;
      if (xf == XForm::ROTATE_45 || xf == XForm::ROTATE_22_5 || xf == XForm::ROTATE_11_25) {
        area_places = std::max(1, area_places - 1);
      }
      double tolerance = std::pow(10.0, -area_places);
      test_failed = (int(num_tris) != t.tris) || (std::abs(area - t.area) > tolerance);
    }

    /* Write SVG visualization next to the JSON file. */
    const char *xf_name = xform_name(xf);
    std::string xf_suffix = (xf_name[0] != '\0') ? std::string("_") + xf_name : "";
    const char *svg_suffix = use_skia_triangulator() ? ".skia.svg" :
                             use_scanfill_legacy()   ? ".legacy.svg" :
                                                       ".svg";
    std::string svg_path = data_dir + "/" + t.filename + xf_suffix + svg_suffix;
    write_svg(svg_path, verts, faces, test_failed);

    /* Write input polygon SVG. */
    std::string poly_svg_path = data_dir + "/" + t.filename + xf_suffix + ".poly.svg";
    write_svg_poly(poly_svg_path, verts, edges);
  }

  /* For degenerate tests, just verify it doesn't crash. */
  if (t.degenerate) {
    return;
  }

  /* Check triangle count. */
  EXPECT_EQ(int(num_tris), t.tris)
      << "Triangle count mismatch for " << t.filename << " with transform " << xform_name(xf);

  /* Check area within tolerance.
   * For transforms using trigonometric functions (non-90-degree rotations),
   * reduce precision by 1 decimal place to account for float precision loss. */
  int area_places = t.area_places;
  if (xf == XForm::ROTATE_45 || xf == XForm::ROTATE_22_5 || xf == XForm::ROTATE_11_25) {
    area_places = std::max(1, area_places - 1);
  }
  double tolerance = std::pow(10.0, -area_places);
  EXPECT_NEAR(area, t.area, tolerance)
      << "Area mismatch for " << t.filename << " with transform " << xform_name(xf);
}

/**
 * Pretty-print TestDataWithXForm for Google Test failure messages.
 * Suppresses unhelpful "40-byte object" output - actual values shown by EXPECT macros.
 */
static void PrintTo(const TestDataWithXForm & /*param*/, std::ostream *os)
{
  *os << "(see above)";
}

/* Generate test name from TestDataWithXForm. */
static std::string test_name_generator(const ::testing::TestParamInfo<TestDataWithXForm> &info)
{
  const char *xf_name = xform_name(info.param.xform);
  if (xf_name[0] != '\0') {
    return std::string(info.param.data.filename) + "_" + xf_name;
  }
  return info.param.data.filename;
}

/* Generate test permutations with all transforms. */
static std::vector<TestDataWithXForm> get_tests_with_xforms(bool adjacent)
{
  std::vector<TestDataWithXForm> result;
  for (const TestData &t : tests_data) {
    if (t.adjacent != adjacent) {
      continue;
    }
    for (int xf_i = 0; xf_i < int(XForm::NUM); xf_i++) {
      XForm xf = XForm(xf_i);
      if (is_test_disabled(t.filename, xf)) {
        continue;
      }
      result.push_back({t, xf});
    }
  }
  return result;
}

/* Register non-adjacent tests with all transform permutations. */
static std::vector<TestDataWithXForm> get_non_adjacent_tests()
{
  return get_tests_with_xforms(false);
}

INSTANTIATE_TEST_SUITE_P(PolyFill,
                         ScanFillDataTest,
                         ::testing::ValuesIn(get_non_adjacent_tests()),
                         test_name_generator);

/* Separate suite for adjacent tests (may have different behavior). */
static std::vector<TestDataWithXForm> get_adjacent_tests()
{
  return get_tests_with_xforms(true);
}

INSTANTIATE_TEST_SUITE_P(PolyFillAdjacent,
                         ScanFillDataTest,
                         ::testing::ValuesIn(get_adjacent_tests()),
                         test_name_generator);

/** \} */

}  // namespace blender::tests
