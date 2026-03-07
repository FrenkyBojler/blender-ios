/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Paint a color made from hash of node pointer. */
// #define DEBUG_PIXEL_NODES

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_object_types.h"
#include "DNA_userdef_types.h"

#include "ED_paint.hh"

#include "BLI_bit_vector.hh"
#include "BLI_listbase.h"
#include "BLI_math_color_blend.h"
#include "BLI_math_geom.h"
#ifdef DEBUG_PIXEL_NODES
#  include "BLI_hash.h"
#endif

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "BKE_brush.hh"
#include "BKE_image_wrappers.hh"
#include "BKE_object_types.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_bvh_pixels.hh"

#include "mesh_brush_common.hh"
#include "sculpt_automask.hh"
#include "sculpt_intern.hh"

namespace blender {

namespace ed::sculpt_paint::paint::image {

using namespace blender::bke::pbvh::pixels;
using namespace blender::bke::image;

struct ImageData {
  Image *image = nullptr;
  ImageUser *image_user = nullptr;

  ~ImageData() = default;

  static bool init_active_image(Object &ob,
                                ImageData *r_image_data,
                                PaintModeSettings &paint_mode_settings)
  {
    return BKE_paint_canvas_image_get(
        &paint_mode_settings, &ob, &r_image_data->image, &r_image_data->image_user);
  }
};

static float3 calc_pixel_position(const Span<float3> vert_positions,
                                  const Span<int3> vert_tris,
                                  const int tri_index,
                                  const float2 &barycentric_weight)
{
  const int3 &verts = vert_tris[tri_index];
  const float3 weights(barycentric_weight.x,
                       barycentric_weight.y,
                       1.0f - barycentric_weight.x - barycentric_weight.y);
  float3 result;
  interp_v3_v3v3v3(result,
                   vert_positions[verts[0]],
                   vert_positions[verts[1]],
                   vert_positions[verts[2]],
                   weights);
  return result;
}

static void calc_pixel_row_positions(const Span<float3> vert_positions,
                                     const Span<int3> vert_tris,
                                     const Span<UVPrimitivePaintInput> uv_primitives,
                                     const PackedPixelRow &pixel_row,
                                     const MutableSpan<float3> positions)
{
  const float3 start = calc_pixel_position(vert_positions,
                                           vert_tris,
                                           uv_primitives[pixel_row.uv_primitive_index].tri_index,
                                           pixel_row.start_barycentric_coord);
  const float3 next = calc_pixel_position(
      vert_positions,
      vert_tris,
      uv_primitives[pixel_row.uv_primitive_index].tri_index,
      pixel_row.start_barycentric_coord +
          uv_primitives[pixel_row.uv_primitive_index].delta_barycentric_coord_u);
  const float3 delta = next - start;
  for (const int i : IndexRange(pixel_row.num_pixels)) {
    positions[i] = start + delta * i;
  }
}

static BitVector<> init_uv_primitives_brush_test(SculptSession &ss,
                                                 const Span<int3> vert_tris,
                                                 const Span<UVPrimitivePaintInput> uv_primitives,
                                                 const Span<float3> positions)
{
  const float3 location = ss.cache ? ss.cache->location_symm : ss.cursor_location;
  const float radius = ss.cache ? ss.cache->radius : ss.cursor_radius;
  const Bounds<float3> brush_bounds(location - radius, location + radius);

  BitVector<> brush_test(uv_primitives.size());
  for (const int i : uv_primitives.index_range()) {
    const UVPrimitivePaintInput &paint_input = uv_primitives[i];
    const int3 verts = vert_tris[paint_input.tri_index];

    Bounds<float3> tri_bounds(positions[verts[0]]);
    math::min_max(positions[verts[1]], tri_bounds.min, tri_bounds.max);
    math::min_max(positions[verts[2]], tri_bounds.min, tri_bounds.max);

    brush_test[i].set(
        isect_aabb_aabb_v3(brush_bounds.min, brush_bounds.max, tri_bounds.min, tri_bounds.max));
  }
  return brush_test;
}

static float4 init_brush_color(const float3 &brush_color, const ImageData &image_data)
{
  ImageUser image_user = *image_data.image_user;
  ImBuf *image_buffer = BKE_image_acquire_ibuf(image_data.image, &image_user, nullptr);

  const char *to_colorspace = image_buffer->float_buffer.data ?
                                  IMB_colormanagement_get_float_colorspace(image_buffer) :
                                  IMB_colormanagement_get_rect_colorspace(image_buffer);

  BKE_image_release_ibuf(image_data.image, image_buffer, nullptr);

  float4 converted_color(brush_color, 1.0f);

  const char *from_colorspace = IMB_colormanagement_role_colorspace_name_get(
      COLOR_ROLE_SCENE_LINEAR);
  ColormanageProcessor *cm_processor = IMB_colormanagement_colorspace_processor_new(
      from_colorspace, to_colorspace);
  IMB_colormanagement_processor_apply_v4(cm_processor, converted_color);
  IMB_colormanagement_processor_free(cm_processor);

  return converted_color;
}

static bool paint_row_float(const Brush &brush,
                            const float4 &brush_color,
                            const PackedPixelRow &pixel_row,
                            const Span<float> factors,
                            ImBuf *image_buffer)
{
  int offset = int(pixel_row.start_image_coordinate.y) * image_buffer->x +
               int(pixel_row.start_image_coordinate.x);
  bool pixels_painted = false;
  for (int x = 0; x < pixel_row.num_pixels; x++) {
    float4 color = &image_buffer->float_buffer.data[offset * 4];
    float4 paint_color = brush_color * factors[x];
    float4 buffer_color;

#ifdef DEBUG_PIXEL_NODES
    if ((pixel_row.start_image_coordinate.y >> 3) & 1) {
      paint_color[0] *= 0.5f;
      paint_color[1] *= 0.5f;
      paint_color[2] *= 0.5f;
    }
#endif

    blend_color_mix_float(buffer_color, color, paint_color);
    buffer_color *= brush.alpha;
    IMB_blend_color_float(color, color, buffer_color, static_cast<IMB_BlendMode>(brush.blend));
    copy_v4_v4(&image_buffer->float_buffer.data[offset * 4], color);
    pixels_painted = true;

    offset++;
  }
  return pixels_painted;
}

static bool paint_row_byte(const Brush &brush,
                           const float4 &brush_color,
                           const PackedPixelRow &pixel_row,
                           const Span<float> factors,
                           ImBuf *image_buffer)
{
  int offset = int(pixel_row.start_image_coordinate.y) * image_buffer->x +
               int(pixel_row.start_image_coordinate.x);
  bool pixels_painted = false;
  for (int x = 0; x < pixel_row.num_pixels; x++) {
    float4 color;
    rgba_uchar_to_float(color,
                        static_cast<const uchar *>(static_cast<const void *>(
                            &(image_buffer->byte_buffer.data[4 * offset]))));
    float4 paint_color = brush_color * factors[x];
    float4 buffer_color;

#ifdef DEBUG_PIXEL_NODES
    if ((pixel_row.start_image_coordinate.y >> 3) & 1) {
      paint_color[0] *= 0.5f;
      paint_color[1] *= 0.5f;
      paint_color[2] *= 0.5f;
    }
#endif

    blend_color_mix_float(buffer_color, color, paint_color);
    buffer_color *= brush.alpha;
    IMB_blend_color_float(color, color, buffer_color, static_cast<IMB_BlendMode>(brush.blend));
    rgba_float_to_uchar(
        static_cast<uchar *>(static_cast<void *>(&image_buffer->byte_buffer.data[4 * offset])),
        color);
    pixels_painted = true;

    offset++;
  }
  return pixels_painted;
}

static void do_paint_pixels(const Depsgraph &depsgraph,
                            Object &object,
                            const Paint &paint,
                            const Brush &brush,
                            ImageData image_data,
                            bke::pbvh::Node &node)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  PBVHData &pbvh_data = bke::pbvh::pixels::data_get(pbvh);
  NodeData &node_data = bke::pbvh::pixels::node_data_get(node);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);

  BitVector<> brush_test = init_uv_primitives_brush_test(
      ss, pbvh_data.vert_tris, node_data.uv_primitives, positions);

  float4 brush_color = init_brush_color(ss.cache->invert ?
                                            BKE_brush_secondary_color_get(&paint, &brush) :
                                            BKE_brush_color_get(&paint, &brush),
                                        image_data);

#ifdef DEBUG_PIXEL_NODES
  float3 debug_color;
  uint hash = BLI_hash_int(POINTER_AS_UINT(&node));

  debug_color[0] = float(hash & 255) / 255.0f;
  debug_color[1] = float((hash >> 8) & 255) / 255.0f;
  debug_color[2] = float((hash >> 16) & 255) / 255.0f;
  brush_color = init_brush_color(debug_color, image_data);
#endif

  Vector<float3> pixel_positions;
  Vector<float> factors;
  Vector<float> distances;

  ImageUser image_user = *image_data.image_user;
  bool pixels_updated = false;
  for (UDIMTilePixels &tile_data : node_data.tiles) {
    for (ImageTile &tile : image_data.image->tiles) {
      ImageTileWrapper image_tile(&tile);
      if (image_tile.get_tile_number() == tile_data.tile_number) {
        image_user.tile = image_tile.get_tile_number();

        ImBuf *image_buffer = BKE_image_acquire_ibuf(image_data.image, &image_user, nullptr);
        if (image_buffer == nullptr) {
          continue;
        }

        for (const PackedPixelRow &pixel_row : tile_data.pixel_rows) {
          if (!brush_test[pixel_row.uv_primitive_index]) {
            continue;
          }

          pixel_positions.resize(pixel_row.num_pixels);
          calc_pixel_row_positions(
              positions, pbvh_data.vert_tris, node_data.uv_primitives, pixel_row, pixel_positions);

          factors.resize(pixel_positions.size());
          factors.fill(1.0f);

          distances.resize(pixel_positions.size());
          calc_brush_distances(
              ss, pixel_positions, eBrushFalloffShape(brush.falloff_shape), distances);
          filter_distances_with_radius(cache.radius, distances, factors);
          apply_hardness_to_distances(cache, distances);
          calc_brush_strength_factors(cache, brush, distances, factors);
          calc_brush_texture_factors(ss, brush, pixel_positions, factors);
          scale_factors(factors, cache.bstrength);

          bool pixels_painted = false;
          if (image_buffer->float_buffer.data != nullptr) {
            pixels_painted = paint_row_float(brush, brush_color, pixel_row, factors, image_buffer);
          }
          else {
            pixels_painted = paint_row_byte(brush, brush_color, pixel_row, factors, image_buffer);
          }

          if (pixels_painted) {
            tile_data.mark_dirty(pixel_row);
          }
        }

        BKE_image_release_ibuf(image_data.image, image_buffer, nullptr);
        pixels_updated |= tile_data.flags.dirty;
        break;
      }
    }
  }

  node_data.flags.dirty |= pixels_updated;
}

static void undo_region_tiles(
    ImBuf *ibuf, int x, int y, int w, int h, int *tx, int *ty, int *tw, int *th)
{
  int srcx = 0, srcy = 0;
  IMB_rectclip(ibuf, nullptr, &x, &y, &srcx, &srcy, &w, &h);
  *tw = ((x + w - 1) >> ED_IMAGE_UNDO_TILE_BITS);
  *th = ((y + h - 1) >> ED_IMAGE_UNDO_TILE_BITS);
  *tx = (x >> ED_IMAGE_UNDO_TILE_BITS);
  *ty = (y >> ED_IMAGE_UNDO_TILE_BITS);
}

static void push_undo(const NodeData &node_data,
                      Image &image,
                      ImageUser &image_user,
                      const image::ImageTileWrapper &image_tile,
                      ImBuf &image_buffer,
                      ImBuf **tmpibuf)
{
  for (const UDIMTileUndo &tile_undo : node_data.undo_regions) {
    if (tile_undo.tile_number != image_tile.get_tile_number()) {
      continue;
    }
    int tilex, tiley, tilew, tileh;
    PaintTileMap *undo_tiles = ED_image_paint_tile_map_get();
    undo_region_tiles(&image_buffer,
                      tile_undo.region.xmin,
                      tile_undo.region.ymin,
                      BLI_rcti_size_x(&tile_undo.region),
                      BLI_rcti_size_y(&tile_undo.region),
                      &tilex,
                      &tiley,
                      &tilew,
                      &tileh);
    for (int ty = tiley; ty <= tileh; ty++) {
      for (int tx = tilex; tx <= tilew; tx++) {
        ED_image_paint_tile_push(undo_tiles,
                                 &image,
                                 &image_buffer,
                                 tmpibuf,
                                 &image_user,
                                 tx,
                                 ty,
                                 nullptr,
                                 nullptr,
                                 true,
                                 true);
      }
    }
  }
}

static void do_push_undo_tile(Image &image, ImageUser &image_user, bke::pbvh::Node &node)
{
  NodeData &node_data = bke::pbvh::pixels::node_data_get(node);

  ImBuf *tmpibuf = nullptr;
  ImageUser local_image_user = image_user;
  for (ImageTile &tile : image.tiles) {
    image::ImageTileWrapper image_tile(&tile);
    local_image_user.tile = image_tile.get_tile_number();
    ImBuf *image_buffer = BKE_image_acquire_ibuf(&image, &local_image_user, nullptr);
    if (image_buffer == nullptr) {
      continue;
    }

    push_undo(node_data, image, image_user, image_tile, *image_buffer, &tmpibuf);
    BKE_image_release_ibuf(&image, image_buffer, nullptr);
  }
  if (tmpibuf) {
    IMB_freeImBuf(tmpibuf);
  }
}

/* -------------------------------------------------------------------- */

/** \name Fix non-manifold edge bleeding.
 * \{ */

static Vector<image::TileNumber> collect_dirty_tiles(MutableSpan<bke::pbvh::MeshNode> nodes,
                                                     const IndexMask &node_mask)
{
  Vector<image::TileNumber> dirty_tiles;
  node_mask.foreach_index(
      [&](const int i) { bke::pbvh::pixels::collect_dirty_tiles(nodes[i], dirty_tiles); });
  return dirty_tiles;
}
static void fix_non_manifold_seam_bleeding(bke::pbvh::Tree &pbvh,
                                           Image &image,
                                           ImageUser &image_user,
                                           Span<TileNumber> tile_numbers_to_fix)
{
  for (image::TileNumber tile_number : tile_numbers_to_fix) {
    bke::pbvh::pixels::copy_pixels(pbvh, image, image_user, tile_number);
  }
}

static void fix_non_manifold_seam_bleeding(Object &ob,
                                           Image &image,
                                           ImageUser &image_user,
                                           MutableSpan<bke::pbvh::MeshNode> nodes,
                                           const IndexMask &node_mask)
{
  Vector<image::TileNumber> dirty_tiles = collect_dirty_tiles(nodes, node_mask);
  fix_non_manifold_seam_bleeding(*bke::object::pbvh_get(ob), image, image_user, dirty_tiles);
}

/** \} */

}  // namespace ed::sculpt_paint::paint::image

using namespace blender::ed::sculpt_paint::paint::image;

bool SCULPT_paint_image_canvas_get(PaintModeSettings &paint_mode_settings,
                                   Object &ob,
                                   Image **r_image,
                                   ImageUser **r_image_user)
{
  *r_image = nullptr;
  *r_image_user = nullptr;

  ImageData image_data;
  if (!ImageData::init_active_image(ob, &image_data, paint_mode_settings)) {
    return false;
  }

  *r_image = image_data.image;
  *r_image_user = image_data.image_user;
  return true;
}

bool SCULPT_use_image_paint_brush(PaintModeSettings &settings, Object &ob)
{
  if (!USER_EXPERIMENTAL_TEST(&U, use_sculpt_texture_paint)) {
    return false;
  }
  if (ob.type != OB_MESH) {
    return false;
  }
  Image *image;
  ImageUser *image_user;
  return BKE_paint_canvas_image_get(&settings, &ob, &image, &image_user);
}

void SCULPT_do_paint_brush_image(const Depsgraph &depsgraph,
                                 PaintModeSettings &paint_mode_settings,
                                 const Sculpt &sd,
                                 Object &ob,
                                 const IndexMask &node_mask)
{
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);

  ImageData image_data;
  if (!ImageData::init_active_image(ob, &image_data, paint_mode_settings)) {
    return;
  }

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  node_mask.foreach_index(
      [&](const int i) { do_push_undo_tile(*image_data.image, *image_data.image_user, nodes[i]); },
      exec_mode::grain_size(1));
  node_mask.foreach_index(
      [&](const int i) { do_paint_pixels(depsgraph, ob, sd.paint, *brush, image_data, nodes[i]); },
      exec_mode::grain_size(1));

  fix_non_manifold_seam_bleeding(ob, *image_data.image, *image_data.image_user, nodes, node_mask);

  node_mask.foreach_index([&](const int i) {
    bke::pbvh::pixels::mark_image_dirty(nodes[i], *image_data.image, *image_data.image_user);
  });
}

}  // namespace blender
