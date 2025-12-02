/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cmpnodes
 */

#include "BLI_array.hh"
#include "BLI_hash.hh"
#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"

#include "UI_interface.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_temporal_denoise_cc {

static const EnumPropertyItem quality_items[] = {
    {0, "HIGH", 0, "High", "High quality temporal denoising with full precision"},
    {1, "BALANCED", 0, "Balanced", "Balanced quality and performance"},
    {2, "FAST", 0, "Fast", "Fast temporal denoising with reduced precision"},
    {0, nullptr, 0, nullptr, nullptr}};

static void cmp_node_temporal_denoise_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();

  b.add_input<decl::Color>("Image")
      .hide_value()
      .structure_type(StructureType::Dynamic);

  b.add_input<decl::Vector>("Speed")
      .dimensions(4)
      .default_value({0.0f, 0.0f, 0.0f})
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_VELOCITY)
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .description("Motion vector pass (xy=previous frame, zw=next frame)");

  b.add_input<decl::Color>("Albedo")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Vector>("Normal")
      .default_value({0.0f, 0.0f, 0.0f})
      .min(-1.0f)
      .max(1.0f)
      .hide_value()
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Depth")
      .default_value(0.0f)
      .min(0.0f)
      .structure_type(StructureType::Dynamic);

  b.add_input<decl::Int>("Frames")
      .default_value(5)
      .min(1)
      .max(10)
      .description("Maximum frames for temporal accumulation (auto-adapted to motion)");

  b.add_input<decl::Float>("Amplitude")
      .default_value(0.8f)
      .min(0.0f)
      .max(1.0f)
      .description("Denoising strength (0=original, 1=full denoising)");

  b.add_input<decl::Menu>("Quality")
      .default_value(MenuValue(0))
      .static_items(quality_items)
      .optional_label()
      .description("High=best quality, Balanced=optimized, Fast=preview");

  PanelDeclarationBuilder &advanced_panel = b.add_panel("Advanced").default_closed(true);
  advanced_panel.add_input<decl::Float>("Variance Gamma")
      .default_value(1.5f)
      .min(0.5f)
      .max(3.0f)
      .description("AABB width: lower=less ghosting/more flicker, higher=more stable/slight ghost");
  advanced_panel.add_input<decl::Float>("Temporal Weight")
      .default_value(1.0f)
      .min(0.1f)
      .max(3.0f)
      .description("Temporal accumulation: lower=more stable, higher=more responsive");

  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic).align_with_previous();
}

using namespace blender::compositor;

/* Per-node history state. For now we keep it simple: just remember last frame number.
 * Pixel history buffering is implemented inside the operation via static cache keyed by node. */

struct HistoryKey {
  const Scene *scene;
  const bNodeTree *tree;
  const bNode *node;

  uint64_t hash() const
  {
    return blender::get_default_hash(scene, tree, node);
  }

  friend bool operator==(const HistoryKey &a, const HistoryKey &b)
  {
    return a.scene == b.scene && a.tree == b.tree && a.node == b.node;
  }
};

struct HistoryEntry {
  int last_frame = 0;
  int stored_frames = 0;
  int capacity_frames = 0;
  int2 size = int2(0);
  blender::Array<float4> buffer;
  blender::Array<float4> albedo_buffer;
  blender::Array<float4> normal_buffer;
  blender::Array<float> depth_buffer;
  blender::Array<float4> motion_buffer;  /* Motion vectors (xy=previous, zw=next) */
  int head = -1;
};

static blender::Map<HistoryKey, HistoryEntry> history_map;

static HistoryEntry &ensure_history_entry(Context &context,
                                          const bNode &bnode,
                                          const int2 &size,
                                          const int current_frame,
                                          const int history_frames,
                                          bool &r_has_history)
{
  HistoryKey key{&context.get_scene(), &context.get_node_tree(), &bnode};
  HistoryEntry &entry = history_map.lookup_or_add_default(key);

  const bool size_changed = (entry.size != size);
  const bool frame_jump = (entry.last_frame != 0 &&
                           (current_frame < entry.last_frame ||
                            current_frame > entry.last_frame + 1));
  const bool capacity_changed = (entry.capacity_frames != history_frames);

  r_has_history = (!size_changed && !frame_jump && !capacity_changed && entry.stored_frames > 0);

  if (size_changed || capacity_changed) {
    entry.size = size;
    entry.capacity_frames = history_frames;
    const int64_t pixel_count = int64_t(size.x) * size.y;
    const int frames_non_negative = history_frames > 0 ? history_frames : 0;
    const int64_t buffer_size = pixel_count * frames_non_negative;
    entry.buffer.reinitialize(buffer_size);
    entry.albedo_buffer.reinitialize(buffer_size);
    entry.normal_buffer.reinitialize(buffer_size);
    entry.depth_buffer.reinitialize(buffer_size);
    entry.motion_buffer.reinitialize(buffer_size);
    entry.stored_frames = 0;
    entry.head = -1;
  }
  if (frame_jump) {
    entry.stored_frames = 0;
    entry.head = -1;
  }

  entry.last_frame = current_frame;
  return entry;
}

/* -------------------------------------------------------------------
 * Color Space Conversion: RGB ↔ YCoCg
 * YCoCg separates luma from chroma for better temporal filtering
 * and reduces color artifacts during variance clipping.
 * ------------------------------------------------------------------- */

static inline float3 rgb_to_ycocg(const float3 &rgb)
{
  const float Y = 0.25f * rgb.x + 0.5f * rgb.y + 0.25f * rgb.z;
  const float Co = 0.5f * rgb.x - 0.5f * rgb.z;
  const float Cg = -0.25f * rgb.x + 0.5f * rgb.y - 0.25f * rgb.z;
  return float3(Y, Co, Cg);
}

static inline float3 ycocg_to_rgb(const float3 &ycocg)
{
  const float tmp = ycocg.x - ycocg.z;
  const float r = tmp + ycocg.y;
  const float g = ycocg.x + ycocg.z;
  const float b = tmp - ycocg.y;
  return float3(r, g, b);
}

/* -------------------------------------------------------------------
 * Variance Clipping (AABB Method)
 * Computes min/max bounds from neighborhood variance to reject
 * outlier historical colors, preventing ghosting artifacts.
 * ------------------------------------------------------------------- */

struct NeighborhoodStats {
  float3 mean;
  float3 variance;
  float3 aabb_min;
  float3 aabb_max;
};

static NeighborhoodStats compute_neighborhood_aabb(const Result &input,
                                                    const int2 &texel,
                                                    const int2 &size,
                                                    const float gamma)
{
  /* 5-tap cross pattern for efficiency (center + 4 neighbors) */
  const int2 offsets[5] = {
      int2(0, 0),   /* center */
      int2(-1, 0),  /* left */
      int2(1, 0),   /* right */
      int2(0, -1),  /* top */
      int2(0, 1)    /* bottom */
  };

  float3 m1 = float3(0.0f);  /* mean */
  float3 m2 = float3(0.0f);  /* second moment (for variance) */

  for (int i = 0; i < 5; i++) {
    const int2 sample_pos = texel + offsets[i];
    
    /* Clamp to image bounds */
    const int2 clamped_pos = math::clamp(sample_pos, int2(0), size - int2(1));
    
    const float4 color = input.load_pixel<float4>(clamped_pos);
    const float3 rgb = float3(color.x, color.y, color.z);
    const float3 ycocg = rgb_to_ycocg(rgb);
    
    m1 += ycocg;
    m2 += ycocg * ycocg;
  }

  m1 /= 5.0f;
  m2 /= 5.0f;

  /* Standard deviation: sqrt(E[X²] - E[X]²) */
  const float3 variance = math::max(float3(0.0f), m2 - m1 * m1);
  const float3 sigma = math::sqrt(variance);

  /* AABB bounds: mean ± gamma * sigma */
  NeighborhoodStats stats;
  stats.mean = m1;
  stats.variance = variance;
  stats.aabb_min = m1 - gamma * sigma;
  stats.aabb_max = m1 + gamma * sigma;

  return stats;
}

/* Clamp color to AABB bounds */
static inline float3 clip_aabb(const float3 &color,
                               const float3 &aabb_min,
                               const float3 &aabb_max)
{
  return math::clamp(color, aabb_min, aabb_max);
}

/* Simple min-max neighborhood clamping (faster alternative) */
static float3 compute_neighborhood_minmax(const Result &input,
                                          const int2 &texel,
                                          const int2 &size,
                                          bool &out_valid)
{
  const int2 offsets[5] = {
      int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};

  float3 box_min = float3(1e10f);
  float3 box_max = float3(-1e10f);

  for (int i = 0; i < 5; i++) {
    const int2 sample_pos = math::clamp(texel + offsets[i], int2(0), size - int2(1));
    const float4 color = input.load_pixel<float4>(sample_pos);
    const float3 rgb = float3(color.x, color.y, color.z);
    const float3 ycocg = rgb_to_ycocg(rgb);
    
    box_min = math::min(box_min, ycocg);
    box_max = math::max(box_max, ycocg);
  }

  out_valid = true;
  return (box_min + box_max) * 0.5f;  /* Return center for reference */
}

class TemporalDenoiseOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    Result &input = get_input("Image");
    Result &output = get_result("Image");

    if (!output.should_compute()) {
      return;
    }

    if (input.is_single_value()) {
      /* Nothing temporal to do on single values. */
      output.share_data(input);
      return;
    }

    const int2 size = input.domain().data_size;

    /* Ensure we work on CPU data. */
    Result input_cpu = context().use_gpu() ? input.download_to_cpu() : input;

    Result &speed_input = get_input("Speed");
    Result &albedo_input = get_input("Albedo");
    Result &normal_input = get_input("Normal");
    Result &depth_input = get_input("Depth");

    const bool has_motion = !speed_input.is_single_value();
    const bool has_albedo = !albedo_input.is_single_value();
    const bool has_normal = !normal_input.is_single_value();
    const bool has_depth = !depth_input.is_single_value();

    Result speed_cpu = (has_motion && context().use_gpu()) ? speed_input.download_to_cpu() :
                                                               speed_input;
    Result albedo_cpu = (has_albedo && context().use_gpu()) ? albedo_input.download_to_cpu() :
                                                                   albedo_input;
    Result normal_cpu = (has_normal && context().use_gpu()) ? normal_input.download_to_cpu() :
                                                                   normal_input;
    Result depth_cpu = (has_depth && context().use_gpu()) ? depth_input.download_to_cpu() :
                                                                 depth_input;

    /* Allocate CPU output buffer. */
    output.set_type(input_cpu.type());
    output.set_precision(input_cpu.precision());
    output.allocate_texture(input_cpu.domain(), false, ResultStorageType::CPU);

    const int frames = math::clamp(
        this->get_input("Frames").get_single_value_default(5), 1, 10);
    const int history_frames = math::max(frames - 1, 0);
    const float amplitude = math::clamp(
        this->get_input("Amplitude").get_single_value_default(0.8f), 0.0f, 1.0f);
    
    const MenuValue quality_menu = this->get_input("Quality").get_single_value_default(MenuValue(0));
    const int quality = quality_menu.value;  /* 0=High, 1=Balanced, 2=Fast */

    /* Load Advanced parameters */
    const float variance_gamma = math::clamp(
        this->get_input("Variance Gamma").get_single_value_default(1.5f), 0.5f, 3.0f);
    const float temporal_weight = math::clamp(
        this->get_input("Temporal Weight").get_single_value_default(1.0f), 0.1f, 3.0f);

    /* Hardcoded thresholds - mostly unused with variance clipping */
    const float luma_threshold = 0.1f;
    const float chroma_threshold = 0.1f;
    const float motion_threshold = 0.5f;
    const float inv_luma_threshold = 10.0f;
    const float inv_chroma_threshold = 10.0f;

    const int current_frame = context().get_frame_number();

    /* NOTE: True temporal history across frames would require persistent caches keyed by node id
     * and frame number, which is outside the scope of this first implementation. For now we apply
     * a simple per-frame luma/chroma-aware blend towards the input itself, which keeps the
     * structure needed for a future true temporal extension. */

    bool has_history = false;
    HistoryEntry &entry = ensure_history_entry(
        context(), this->bnode(), size, current_frame, history_frames, has_history);

    const int64_t pixel_count = int64_t(size.x) * size.y;
    const int history_capacity = entry.capacity_frames;
    const int prev_head = entry.head;
    const int prev_stored = entry.stored_frames;
    const bool use_history = (has_history && history_capacity > 0 && prev_stored > 0);
    const int history_to_use = use_history ? math::min(prev_stored, history_capacity) : 0;

    int write_frame_index = 0;
    if (history_capacity > 0) {
      if (prev_head < 0) {
        write_frame_index = 0;
      }
      else {
        write_frame_index = (prev_head + 1) % history_capacity;
      }
    }

    /* Adaptive motion-based frame count */
    const float scene_motion_threshold = 5.0f;

    parallel_for(size, [&](const int2 texel) {
      /* =================================================================
       * STEP 1: Load Current Frame Data
       * ================================================================= */
      
      const float4 center_rgba = input_cpu.load_pixel<float4>(texel);
      const float3 center_rgb = float3(center_rgba.x, center_rgba.y, center_rgba.z);
      const float3 center_ycocg = rgb_to_ycocg(center_rgb);
      
      /* Load motion vector */
      const float4 motion_vec = has_motion ? speed_cpu.load_pixel<float4>(texel) : float4(0.0f);
      const float2 motion_prev = motion_vec.xy();
      const float scene_motion = math::length(motion_prev);
      
      /* Load auxiliary data */
      float3 albedo_center = float3(0.0f);
      float3 normal_center = float3(0.0f);
      float depth_center = 0.0f;

      if (has_albedo) {
        const float4 a = albedo_cpu.load_pixel<float4>(texel);
        albedo_center = float3(a.x, a.y, a.z);
      }
      if (has_normal) {
        normal_center = normal_cpu.load_pixel<float3>(texel);
      }
      if (has_depth) {
        depth_center = depth_cpu.load_pixel<float>(texel);
      }

      /* =================================================================
       * STEP 2: Compute Neighborhood Stats (Variance Clipping)
       * ================================================================= */
      
      NeighborhoodStats neighborhood;
      float3 box_min, box_max;
      
      if (quality != 2) {  /* High or Balanced: use variance clipping */
        neighborhood = compute_neighborhood_aabb(input_cpu, texel, size, variance_gamma);
        box_min = neighborhood.aabb_min;
        box_max = neighborhood.aabb_max;
      }
      else {  /* Fast: use simple min-max */
        bool valid;
        compute_neighborhood_minmax(input_cpu, texel, size, valid);
        /* Compute min-max manually for Fast mode */
        const int2 offsets[5] = {int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
        box_min = float3(1e10f);
        box_max = float3(-1e10f);
        for (int i = 0; i < 5; i++) {
          const int2 sp = math::clamp(texel + offsets[i], int2(0), size - int2(1));
          const float4 c = input_cpu.load_pixel<float4>(sp);
          const float3 yc = rgb_to_ycocg(float3(c.x, c.y, c.z));
          box_min = math::min(box_min, yc);
          box_max = math::max(box_max, yc);
        }
      }

      /* =================================================================
       * STEP 3: Adaptive Frame Count based on Motion
       * ================================================================= */
      
      int adaptive_frames = history_to_use;
      if (scene_motion > scene_motion_threshold) {
        adaptive_frames = math::min(history_to_use, 2);  /* High motion: fewer frames */
      }
      else if (scene_motion > 2.0f) {
        adaptive_frames = math::min(history_to_use, 4);  /* Moderate motion */
      }
      
      /* Quality-based limit */
      const int effective_history = quality == 2 ? math::min(adaptive_frames, 2) :
                                    quality == 1 ? math::min(adaptive_frames, 5) : 
                                    adaptive_frames;

      /* =================================================================
       * STEP 4: Temporal Reprojection & Exponential Moving Average
       * ================================================================= */
      
      float3 temporal_result_ycocg = center_ycocg;  /* Start with current frame */
      bool has_valid_history = false;

      if (use_history && has_motion && effective_history > 0) {
        const int x = texel.x;
        const int y = texel.y;
        const int width = size.x;
        const int height = size.y;

        /* Try to find  ONE valid historical sample (most recent) */
        for (int i = 0; i < effective_history; i++) {
          const int frame_index = (prev_head - i + history_capacity) % history_capacity;
          
          /* Reproject pixel position */
          const float2 reprojected_pos = float2(x, y) - motion_prev * float(i + 1);
          
          /* Bounds check */
          if (reprojected_pos.x < 0 || reprojected_pos.x >= width - 1 ||
              reprojected_pos.y < 0 || reprojected_pos.y >= height - 1) {
            continue;
          }

          /* Bilinear interpolation setup */
          const int2 p0 = int2(math::floor(reprojected_pos.x), math::floor(reprojected_pos.y));
          const float2 frac = reprojected_pos - float2(p0);
          const float w00 = (1.0f - frac.x) * (1.0f - frac.y);
          const float w10 = frac.x * (1.0f - frac.y);
          const float w01 = (1.0f - frac.x) * frac.y;
          const float w11 = frac.x * frac.y;

          const int64_t pixel_count_local = int64_t(width) * height;
          const int64_t idx0 = int64_t(frame_index) * pixel_count_local + int64_t(p0.y) * width + p0.x;
          const int64_t idx1 = idx0 + 1;
          const int64_t idx2 = idx0 + width;
          const int64_t idx3 = idx2 + 1;

          /* Sample historical color */
          const float4 h0 = entry.buffer[idx0];
          const float4 h1 = entry.buffer[idx1];
          const float4 h2 = entry.buffer[idx2];
          const float4 h3 = entry.buffer[idx3];
          const float4 hist_rgba = h0 * w00 + h1 * w10 + h2 * w01 + h3 * w11;
          const float3 hist_rgb = float3(hist_rgba.x, hist_rgba.y, hist_rgba.z);
          float3 hist_ycocg = rgb_to_ycocg(hist_rgb);

          /* ===============================================================
           * ROBUST REJECTION CRITERIA
           * =============================================================== */
          
          bool is_valid = true;

          /* 1. Depth discontinuity check - relaxed to reduce flickering */
          if (has_depth && entry.depth_buffer.size() > 0) {
            const float d0 = entry.depth_buffer[idx0];
            const float d1 = entry.depth_buffer[idx1];
            const float d2 = entry.depth_buffer[idx2];
            const float d3 = entry.depth_buffer[idx3];
            const float hist_depth = d0 * w00 + d1 * w10 + d2 * w01 + d3 * w11;
            
            const float depth_diff = math::abs(hist_depth - depth_center);
            const float depth_threshold_val = 0.15f * math::max(depth_center, 0.01f);  /* Relaxed */
            if (depth_diff > depth_threshold_val) {
              is_valid = false;
            }
          }

          /* 2. Normal discontinuity check - relaxed */
          if (is_valid && has_normal && entry.normal_buffer.size() > 0) {
            const float4 n0 = entry.normal_buffer[idx0];
            const float4 n1 = entry.normal_buffer[idx1];
            const float4 n2 = entry.normal_buffer[idx2];
            const float4 n3 = entry.normal_buffer[idx3];
            const float4 nh = n0 * w00 + n1 * w10 + n2 * w01 + n3 * w11;
            const float3 hist_normal = float3(nh.x, nh.y, nh.z);
            
            const float normal_dot = math::dot(hist_normal, normal_center);
            if (normal_dot < 0.85f) {  /* ~30° deviation - relaxed for stability */
              is_valid = false;
            }
          }

          /* 3. Motion vector consistency check - relaxed */
          if (is_valid && has_motion && entry.motion_buffer.size() > 0) {
            const float4 m0 = entry.motion_buffer[idx0];
            const float4 m1 = entry.motion_buffer[idx1];
            const float4 m2 = entry.motion_buffer[idx2];
            const float4 m3 = entry.motion_buffer[idx3];
            const float4 hist_motion = m0 * w00 + m1 * w10 + m2 * w01 + m3 * w11;
            
            /* Use backward motion (zw component) for validation */
            const float2 backward_motion = hist_motion.zw();
            const float motion_error = math::length(motion_prev + backward_motion);
            if (motion_error > 3.0f) {  /* pixels - relaxed for stability */
              is_valid = false;
            }
          }

          /* 4. Variance Clipping (AABB) - Reject outliers */
          if (is_valid) {
            hist_ycocg = clip_aabb(hist_ycocg, box_min, box_max);
            
            /* Additional color divergence check after clamping - relaxed */
            const float3 color_diff = math::abs(hist_ycocg - center_ycocg);
            if (color_diff.x > 0.7f || color_diff.y > 0.7f || color_diff.z > 0.7f) {  /* Relaxed */
              is_valid = false;
            }
          }

          /* ===============================================================
           * EXPONENTIAL MOVING AVERAGE - Lower alphas for more stability
           * =============================================================== */
          
          if (is_valid) {
            /* Compute weight for this frame */
            const float temporal_falloff = 1.0f / (1.0f + float(i) * 0.5f);
            
            /* Accumulate this valid sample */
            if (!has_valid_history) {
              /* First valid sample - initialize */
              temporal_result_ycocg = hist_ycocg;
              has_valid_history = true;
            }
            else {
              /* Blend with previous accumulation */
              /* Base alpha adjusted by temporal_weight parameter */
              const float base_alpha = quality == 0 ? 0.03f :   /* High - very stable */
                                       quality == 1 ? 0.07f :    /* Balanced */
                                       0.15f;                    /* Fast */
              
              const float adjusted_alpha = base_alpha * temporal_weight;
              const float blend_alpha = adjusted_alpha * temporal_falloff;
              temporal_result_ycocg = math::interpolate(temporal_result_ycocg, hist_ycocg, blend_alpha);
            }
            
            /* Don't break - accumulate multiple frames for better stability */
          }
        }
      }

      /* If no valid history or no motion vectors, keep current frame */
      if (!has_valid_history) {
        temporal_result_ycocg = center_ycocg;
      }

      /* =================================================================
       * STEP 5: Convert back to RGB and Apply Amplitude
       * ================================================================= */
      
      const float3 denoised_rgb = ycocg_to_rgb(temporal_result_ycocg);
      const float3 final_rgb = math::interpolate(center_rgb, denoised_rgb, amplitude);
      const float4 out_color = float4(final_rgb.x, final_rgb.y, final_rgb.z, center_rgba.w);

      output.store_pixel(texel, Color(out_color));

      /* =================================================================
       * STEP 6: Store to History Buffer
       * ================================================================= */
      
      if (history_capacity > 0) {
        const int x = texel.x;
        const int y = texel.y;
        const int width = size.x;
        const int64_t pixel_index = int64_t(y) * width + x;
        const int64_t buffer_index = int64_t(write_frame_index) * pixel_count + pixel_index;
        
        entry.buffer[buffer_index] = out_color;
        
        if (entry.motion_buffer.size() > 0) {
          entry.motion_buffer[buffer_index] = motion_vec;
        }
        
        if (has_albedo && entry.albedo_buffer.size() > 0) {
          entry.albedo_buffer[buffer_index] = float4(albedo_center.x,
                                                    albedo_center.y,
                                                    albedo_center.z,
                                                    0.0f);
        }
        if (has_normal && entry.normal_buffer.size() > 0) {
          entry.normal_buffer[buffer_index] = float4(normal_center.x,
                                                    normal_center.y,
                                                    normal_center.z,
                                                    0.0f);
        }
        if (has_depth && entry.depth_buffer.size() > 0) {
          entry.depth_buffer[buffer_index] = depth_center;
        }
      }
    });

    if (history_capacity > 0) {
      entry.head = write_frame_index;
      entry.stored_frames = math::min(prev_stored + 1, history_capacity);
    }

    if (context().use_gpu()) {
      Result output_gpu = output.upload_to_gpu(true);
      output.steal_data(output_gpu);
    }
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new TemporalDenoiseOperation(context, node);
}

}  // namespace blender::nodes::node_composite_temporal_denoise_cc

static void register_node_type_cmp_temporal_denoise()
{
  namespace file_ns = blender::nodes::node_composite_temporal_denoise_cc;

  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeTemporalDenoise", CMP_NODE_TEMPORAL_DENOISE);
  ntype.ui_name = "Temporal Denoise";
  ntype.ui_description = "Simple temporal denoising with controllable history length and thresholds";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.enum_name_legacy = "TEMPORAL_DENOISE";
  ntype.declare = file_ns::cmp_node_temporal_denoise_declare;
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_temporal_denoise)
