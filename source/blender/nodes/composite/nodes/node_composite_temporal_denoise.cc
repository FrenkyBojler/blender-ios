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

static const EnumPropertyItem motion_estimation_items[] = {
    {0, "NONE", 0, "None", "No motion estimation (faster but may blur motion)"},
    {1, "FASTER", 0, "Faster", "Fast motion estimation (default, good balance)"},
    {2, "BETTER", 0, "Better", "Accurate motion estimation (slower, best quality)"},
    {0, nullptr, 0, nullptr, nullptr}};

static const EnumPropertyItem motion_range_items[] = {
    {0, "SMALL", 0, "Small", "Slow motion (more denoising in static areas)"},
    {1, "MEDIUM", 0, "Medium", "Medium motion (balanced)"},
    {2, "LARGE", 0, "Large", "Fast motion (less denoising, sharper motion)"},
    {0, nullptr, 0, nullptr, nullptr}};

static void cmp_node_temporal_denoise_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();

  b.add_input<decl::Color>("Image")
      .hide_value()
      .structure_type(StructureType::Dynamic);

  b.add_input<decl::Vector>("Motion")
      .dimensions(4)
      .default_value({0.0f, 0.0f, 0.0f})
      .subtype(PROP_VELOCITY)
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .description("Motion vector pass (xy=previous, zw=next). Optional but recommended");

  b.add_input<decl::Float>("Luma")
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .description("Luminance denoising strength (brightness)");

  b.add_input<decl::Float>("Chroma")
      .default_value(0.7f)
      .min(0.0f)
      .max(2.0f)
      .description("Chrominance denoising strength (color). Can be higher than Luma");

  b.add_input<decl::Int>("Frames")
      .default_value(3)
      .min(1)
      .max(5)
      .description("Number of frames to blend (1-5). Higher = stronger denoising");

  b.add_input<decl::Float>("Motion Threshold")
      .default_value(10.0f)
      .min(0.0f)
      .max(100.0f)
      .description("Exclude pixels above this motion (pixels). Lower = sharper motion");

  b.add_input<decl::Menu>("Motion Estimation")
      .default_value(MenuValue(1))
      .static_items(motion_estimation_items)
      .description("Motion detection quality: None (fastest), Faster (default), Better (best)");

  b.add_input<decl::Menu>("Motion Range")
      .default_value(MenuValue(1))
      .static_items(motion_range_items)
      .description("Expected motion speed: Small (slow), Medium (default), Large (fast)");

  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
}

using namespace blender::compositor;

/* Simple history buffer */
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
  blender::Array<float4> buffer;        /* RGB history */
  blender::Array<float4> motion_buffer; /* Motion vectors */
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
    const int64_t buffer_size = pixel_count * history_frames;
    entry.buffer.reinitialize(buffer_size);
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

/* RGB to Luma (Rec. 709) */
static inline float rgb_to_luma(const float3 &rgb)
{
  return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
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
      output.share_data(input);
      return;
    }

    const int2 size = input.domain().data_size;
    Result input_cpu = context().use_gpu() ? input.download_to_cpu() : input;

    Result &motion_input = get_input("Motion");
    const bool has_motion = !motion_input.is_single_value();
    Result motion_cpu = (has_motion && context().use_gpu()) ? motion_input.download_to_cpu() :
                                                                motion_input;

    output.set_type(input_cpu.type());
    output.set_precision(input_cpu.precision());
    output.allocate_texture(input_cpu.domain(), false, ResultStorageType::CPU);

    const float luma_strength = math::clamp(
        this->get_input("Luma").get_single_value_default(0.5f), 0.0f, 1.0f);
    const float chroma_strength = math::clamp(
        this->get_input("Chroma").get_single_value_default(0.7f), 0.0f, 2.0f);
    const int frames = math::clamp(
        this->get_input("Frames").get_single_value_default(3), 1, 5);
    const float base_motion_threshold = math::clamp(
        this->get_input("Motion Threshold").get_single_value_default(10.0f), 0.0f, 100.0f);

    /* Motion Estimation: 0=None, 1=Faster, 2=Better */
    const MenuValue motion_est_menu = this->get_input("Motion Estimation")
                                           .get_single_value_default(MenuValue(1));
    const int motion_estimation = motion_est_menu.value;

    /* Motion Range: 0=Small, 1=Medium, 2=Large */
    const MenuValue motion_range_menu = this->get_input("Motion Range")
                                             .get_single_value_default(MenuValue(1));
    const int motion_range = motion_range_menu.value;

    /* Apply motion range to threshold: Small=stricter, Large=more permissive */
    const float range_multiplier = (motion_range == 0) ? 0.5f :   /* Small: stricter */
                                    (motion_range == 2) ? 2.0f :   /* Large: more permissive */
                                                          1.0f;    /* Medium: default */
    const float motion_threshold = base_motion_threshold * range_multiplier;

    /* Motion Estimation=None disables motion compensation entirely */
    const bool use_motion_compensation = (motion_estimation != 0 && has_motion);

    const int history_frames = frames - 1;  /* Current frame + N-1 history */
    const int current_frame = context().get_frame_number();

    bool has_history = false;
    HistoryEntry &entry = ensure_history_entry(
        context(), this->bnode(), size, current_frame, history_frames, has_history);

    const int64_t pixel_count = int64_t(size.x) * size.y;
    const int prev_head = entry.head;
    const int prev_stored = entry.stored_frames;
    const bool use_history = (has_history && prev_stored > 0);

    int write_frame_index = 0;
    if (history_frames > 0) {
      write_frame_index = (prev_head < 0) ? 0 : ((prev_head + 1) % history_frames);
    }

    /* ===== DEBUG OUTPUT ===== */
    printf("\n========== TEMPORAL DENOISE DEBUG ==========\n");
    printf("Frame: %d | Resolution: %dx%d | Pixels: %lld\n", 
           current_frame, size.x, size.y, pixel_count);
    printf("--- Parameters ---\n");
    printf("  Luma Strength: %.2f\n", luma_strength);
    printf("  Chroma Strength: %.2f\n", chroma_strength);
    printf("  Frames: %d (history: %d)\n", frames, history_frames);
    printf("  Base Motion Threshold: %.1f\n", base_motion_threshold);
    printf("  Motion Estimation: %s (%d)\n", 
           motion_estimation == 0 ? "NONE" : motion_estimation == 1 ? "FASTER" : "BETTER",
           motion_estimation);
    printf("  Motion Range: %s (%d) | Multiplier: %.1fx\n",
           motion_range == 0 ? "SMALL" : motion_range == 1 ? "MEDIUM" : "LARGE",
           motion_range, range_multiplier);
    printf("  Effective Motion Threshold: %.1f\n", motion_threshold);
    printf("--- Motion ---\n");
    printf("  Has Motion Vectors: %s\n", has_motion ? "YES" : "NO");
    printf("  Use Motion Compensation: %s\n", use_motion_compensation ? "YES" : "NO");
    printf("--- History ---\n");
    printf("  Has History: %s\n", has_history ? "YES" : "NO");
    printf("  Stored Frames: %d / %d capacity\n", prev_stored, entry.capacity_frames);
    printf("  Previous Head: %d | Write Index: %d\n", prev_head, write_frame_index);
    printf("  Using History: %s\n", use_history ? "YES" : "NO");
    printf("============================================\n\n");

    /* Statistics counters for debug */
    std::atomic<int64_t> pixels_with_motion_comp(0);
    std::atomic<int64_t> pixels_direct_average(0);
    std::atomic<int64_t> samples_excluded_bounds(0);
    std::atomic<int64_t> samples_excluded_threshold(0);
    std::atomic<int> debug_motion_count(0);  /* For debugging motion compensation */

    /* DaVinci-style simple averaging */
    parallel_for(size, [&](const int2 texel) {
      const float4 current_rgba = input_cpu.load_pixel<float4>(texel);
      const float3 current_rgb = float3(current_rgba.x, current_rgba.y, current_rgba.z);
      const float current_luma = rgb_to_luma(current_rgb);
      const float3 current_chroma = current_rgb - float3(current_luma);

      /* Load motion vector once (needed for both history lookup and storage) */
      const float4 motion_vec = has_motion ? motion_cpu.load_pixel<float4>(texel) : float4(0.0f);
      /* Blender motion vectors are typically in pixels (Speed pass). */
      const float2 motion = float2(motion_vec.x, motion_vec.y);
      bool used_motion_comp = false;

      /* Separate luma/chroma and apply different strengths */
      /* DaVinci style: accumulate luma and chroma SEPARATELY, not RGB together */
      /* Temporal weighting: closer frames contribute MORE than distant frames */
      float accumulated_luma = current_luma;
      float3 accumulated_chroma = current_chroma;
      float total_luma_weight = 1.0f;
      float total_chroma_weight = 1.0f;

      if (use_history && prev_head >= 0) {
        const int x = texel.x;
        const int y = texel.y;
        const int width = size.x;
        const int height = size.y;

        for (int i = 0; i < prev_stored && i < history_frames; i++) {
          /* Safe circular buffer indexing (handles negative modulo) */
          const int frame_idx = ((prev_head - i) % history_frames + history_frames) % history_frames;
          float3 hist_rgb;
          bool sample_valid = false;

          if (use_motion_compensation && i == 0) {
            /* Motion compensation path (Faster or Better modes) */
            const float2 reproj = float2(x, y) + motion;

            /* Bilinear sample - compute base pixel FIRST */
            const int2 p0 = int2(math::floor(reproj.x), math::floor(reproj.y));

            /* CRITICAL: Check bounds on p0. 
             * We can clamp the +1 pixel, so as long as p0 is within [0, width-1], we are good.
             * Even better, we can support sampling slightly off-screen if we wanted, but for now
             * let's just ensure p0 is strictly inside the image.
             */
            if (p0.x < 0 || p0.x >= width || p0.y < 0 || p0.y >= height) {
              samples_excluded_bounds++;
              continue;  /* Out of bounds */
            }

            /* Check motion magnitude against threshold (in pixels). */
            const float motion_mag = math::length(motion);
            if (motion_mag > motion_threshold) {
              samples_excluded_threshold++;
              continue;  /* Motion too high, exclude this pixel */
            }

            /* Bilinear interpolation weights */
            const float2 frac = reproj - float2(p0);
            const float w00 = (1.0f - frac.x) * (1.0f - frac.y);
            const float w10 = frac.x * (1.0f - frac.y);
            const float w01 = (1.0f - frac.x) * frac.y;
            const float w11 = frac.x * frac.y;

            /* CRITICAL: Clamp coordinates to prevent wrapping at line boundaries */
            const int x0 = math::min(p0.x, width - 1);
            const int x1 = math::min(p0.x + 1, width - 1);
            const int y0 = math::min(p0.y, height - 1);
            const int y1 = math::min(p0.y + 1, height - 1);

            const int64_t base_idx = int64_t(frame_idx) * pixel_count;
            const int64_t idx00 = base_idx + int64_t(y0) * width + x0;
            const int64_t idx10 = base_idx + int64_t(y0) * width + x1;
            const int64_t idx01 = base_idx + int64_t(y1) * width + x0;
            const int64_t idx11 = base_idx + int64_t(y1) * width + x1;

            const float4 h0 = entry.buffer[idx00];
            const float4 h1 = entry.buffer[idx10];
            const float4 h2 = entry.buffer[idx01];
            const float4 h3 = entry.buffer[idx11];

            const float4 hist_rgba = h0 * w00 + h1 * w10 + h2 * w01 + h3 * w11;
            hist_rgb = float3(hist_rgba.x, hist_rgba.y, hist_rgba.z);
            sample_valid = true;
            used_motion_comp = true;

            /* DEBUG: Print first few motion compensation samples */
            static std::atomic<int> debug_count(0);
            if (debug_count.load() < 5 && current_frame == 2) {
              const int64_t total_buffer_size = int64_t(history_frames) * pixel_count;
              printf("  [DEBUG Pixel %d,%d Frame %d History i=%d]\n", x, y, current_frame, i);
              printf("    Motion: (%.2f, %.2f)\n", motion.x, motion.y);
              printf("    Reproj: (%.2f, %.2f) -> p0: (%d, %d)\n", reproj.x, reproj.y, p0.x, p0.y);
              printf("    Clamped: x0=%d x1=%d y0=%d y1=%d\n", x0, x1, y0, y1);
              printf("    Indices: %lld %lld %lld %lld (max: %lld)\n", idx00, idx10, idx01, idx11, total_buffer_size - 1);
              printf("    Weights: %.3f %.3f %.3f %.3f\n", w00, w10, w01, w11);
              debug_count++;
            }
          }
          else {
            /* No motion compensation - direct averaging */
            const int64_t idx = int64_t(frame_idx) * pixel_count + int64_t(y) * width + x;
            const float4 hist_rgba = entry.buffer[idx];
            hist_rgb = float3(hist_rgba.x, hist_rgba.y, hist_rgba.z);
            sample_valid = true;
          }

          if (sample_valid) {
            /* Separate this history sample into luma/chroma */
            const float hist_luma = rgb_to_luma(hist_rgb);
            const float3 hist_chroma = hist_rgb - float3(hist_luma);

            /* Temporal weighting: i=0 (most recent) has weight=1.0, decays for older frames */
            const float temporal_weight = 1.0f / (1.0f + float(i) * 0.5f);
            
            /* Accumulate with temporal weighting */
            accumulated_luma += hist_luma * temporal_weight;
            accumulated_chroma += hist_chroma * temporal_weight;
            total_luma_weight += temporal_weight;
            total_chroma_weight += temporal_weight;
          }
        }
      }

      /* Track which path was used for this pixel */
      if (used_motion_comp) {
        pixels_with_motion_comp++;
      }
      else if (use_history && prev_head >= 0) {
        pixels_direct_average++;
      }

      /* Average luma and chroma using temporal weights */
      accumulated_luma /= total_luma_weight;
      accumulated_chroma /= total_chroma_weight;

      /* Apply denoising strength separately to luma and chroma */
      const float final_luma = math::interpolate(current_luma, accumulated_luma, luma_strength);
      const float3 final_chroma = math::interpolate(
          current_chroma, accumulated_chroma, chroma_strength);

      const float3 final_rgb = float3(final_luma) + final_chroma;
      const float4 final_rgba = float4(final_rgb.x, final_rgb.y, final_rgb.z, current_rgba.w);

      output.store_pixel(texel, Color(final_rgba));

      /* Store current frame to history */
      if (history_frames > 0) {
        const int64_t buffer_idx = int64_t(write_frame_index) * pixel_count +
                                   int64_t(texel.y) * size.x + texel.x;
        entry.buffer[buffer_idx] = current_rgba;
        if (has_motion) {
          entry.motion_buffer[buffer_idx] = motion_vec;
        }
      }
    });

    if (history_frames > 0) {
      entry.head = write_frame_index;
      entry.stored_frames = math::min(prev_stored + 1, history_frames);
    }

    /* ===== DEBUG OUTPUT - COMPLETION ===== */
    printf("========== PROCESSING COMPLETE ==========\n");
    printf("  Updated History Head: %d\n", entry.head);
    printf("  Updated Stored Frames: %d / %d\n", entry.stored_frames, history_frames);
    printf("  Next frame will have %d history frames available\n", entry.stored_frames);
    printf("--- Processing Statistics ---\n");
    printf("  Total Pixels: %lld\n", pixel_count);
    printf("  Pixels with Motion Compensation: %lld (%.1f%%)\n", 
           pixels_with_motion_comp.load(),
           100.0f * pixels_with_motion_comp.load() / pixel_count);
    printf("  Pixels with Direct Averaging: %lld (%.1f%%)\n",
           pixels_direct_average.load(),
           100.0f * pixels_direct_average.load() / pixel_count);
    printf("  Samples Excluded (Out of Bounds): %lld\n", samples_excluded_bounds.load());
    printf("  Samples Excluded (Motion Threshold): %lld\n", samples_excluded_threshold.load());
    printf("==========================================\n\n");

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
