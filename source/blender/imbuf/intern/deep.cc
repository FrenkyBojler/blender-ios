/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include "BLI_math_base.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include <algorithm>

namespace blender {

/* Validate pixel coordinates and deep buffer */
static bool deep_validate_pixel(const ImBuf *ibuf, int x, int y)
{
  if (!ibuf || !(ibuf->flags & IB_deep_data)) {
    return false;
  }

  if (x < 0 || x >= ibuf->x || y < 0 || y >= ibuf->y) {
    return false;
  }

  const ImBufDeepBuffer &deep = *ibuf->deep_buffer;
  if (deep.sample_counts.is_empty() || deep.depths.is_empty()) {
    return false;
  }

  return true;
}

/* Get pixel index from coordinates */
static inline int deep_pixel_index(const ImBuf *ibuf, int x, int y)
{
  return y * ibuf->x + x;
}

int IMB_deep_get_sample_count(const ImBuf *ibuf, int x, int y)
{
  if (!deep_validate_pixel(ibuf, x, y)) {
    return 0;
  }

  const int pixel_idx = deep_pixel_index(ibuf, x, y);
  return (*ibuf->deep_buffer).sample_counts[pixel_idx];
}

int IMB_deep_read_pixel_samples(
    const ImBuf *ibuf, int x, int y, float *r_depths, float *r_channels)
{
  if (!deep_validate_pixel(ibuf, x, y)) {
    return 0;
  }

  if (!r_depths || !r_channels) {
    return 0;
  }

  const ImBufDeepBuffer &deep = *ibuf->deep_buffer;
  const int pixel_idx = deep_pixel_index(ibuf, x, y);
  const int sample_count = deep.sample_counts[pixel_idx];

  if (sample_count == 0) {
    return 0;
  }

  /* Get sample offset for this pixel */
  const int sample_offset = deep.sample_offsets[pixel_idx];

  /* Copy depth values */
  const float *src_depths = &deep.depths[sample_offset];
  memcpy(r_depths, src_depths, sample_count * sizeof(float));

  /* Copy channel data */
  const int channels_per_sample = deep.channels_per_sample;
  const float *src_channels = &deep.channel_data[sample_offset * channels_per_sample];
  memcpy(r_channels, src_channels, sample_count * channels_per_sample * sizeof(float));

  return sample_count;
}

bool IMB_deep_write_pixel_samples(
    ImBuf *ibuf, int x, int y, const float *depths, const float *channels, int sample_count)
{
  if (!deep_validate_pixel(ibuf, x, y)) {
    return false;
  }

  if (!depths || !channels || sample_count < 0) {
    return false;
  }

  ImBufDeepBuffer &deep = *ibuf->deep_buffer;
  const int pixel_idx = deep_pixel_index(ibuf, x, y);

  /* Check if we have space allocated for this pixel */
  if (pixel_idx >= deep.sample_counts.size()) {
    return false;
  }

  const int old_sample_count = deep.sample_counts[pixel_idx];

  /* For now, only allow writing if sample count matches pre-allocated space.
   * Dynamic reallocation would require rebuilding the entire sample_offsets array. */
  if (sample_count != old_sample_count) {
    /* TODO: Implement dynamic reallocation if needed */
    return false;
  }

  if (sample_count == 0) {
    return true; /* Nothing to write */
  }

  /* Get sample offset for this pixel */
  const int sample_offset = deep.sample_offsets[pixel_idx];

  /* Check bounds */
  const int channels_per_sample = deep.channels_per_sample;
  if (sample_offset + sample_count > deep.depths.size()) {
    return false;
  }

  /* Write depth values */
  float *dst_depths = &deep.depths[sample_offset];
  memcpy(dst_depths, depths, sample_count * sizeof(float));

  /* Write channel data */
  float *dst_channels = &deep.channel_data[sample_offset * channels_per_sample];
  memcpy(dst_channels, channels, sample_count * channels_per_sample * sizeof(float));

  return true;
}

int IMB_deep_get_pixel_samples_ptr(
    const ImBuf *ibuf, int x, int y, const float **r_depths, const float **r_channels)
{
  if (!deep_validate_pixel(ibuf, x, y)) {
    if (r_depths) {
      *r_depths = nullptr;
    }
    if (r_channels) {
      *r_channels = nullptr;
    }
    return 0;
  }

  const ImBufDeepBuffer &deep = *ibuf->deep_buffer;
  const int pixel_idx = deep_pixel_index(ibuf, x, y);
  const int sample_count = deep.sample_counts[pixel_idx];

  if (sample_count == 0) {
    if (r_depths) {
      *r_depths = nullptr;
    }
    if (r_channels) {
      *r_channels = nullptr;
    }
    return 0;
  }

  const int sample_offset = deep.sample_offsets[pixel_idx];
  const int channels_per_sample = deep.channels_per_sample;

  if (r_depths) {
    *r_depths = &deep.depths[sample_offset];
  }

  if (r_channels) {
    *r_channels = &deep.channel_data[sample_offset * channels_per_sample];
  }

  return sample_count;
}

bool IMB_deep_append_pixel_samples(
    ImBuf *ibuf, int x, int y, const float *depths, const float *channels, int sample_count)
{
  if (!ibuf) {
    return false;
  }

  /* Validate coordinates */
  if (x < 0 || x >= ibuf->x || y < 0 || y >= ibuf->y) {
    return false;
  }

  if (!depths || !channels || sample_count < 0) {
    return false;
  }

  /* Initialize deep buffer if needed */
  if (!(ibuf->flags & IB_deep_data)) {
    ibuf->flags |= IB_deep_data;
    ibuf->deep_buffer.emplace();  // Create the ImBufDeepBuffer

    const int pixel_count = ibuf->x * ibuf->y;
    ibuf->deep_buffer->sample_counts.reinitialize(pixel_count);
    ibuf->deep_buffer->sample_offsets.reinitialize(pixel_count + 1);

    /* Assume 4 channels (RGBA) if not set */
    if (ibuf->deep_buffer->channels_per_sample == 0) {
      ibuf->deep_buffer->channels_per_sample = 4;
    }
  }

  if (sample_count == 0) {
    return true; /* Nothing to append */
  }

  ImBufDeepBuffer &deep = *ibuf->deep_buffer;
  const int pixel_idx = deep_pixel_index(ibuf, x, y);
  const int channels_per_sample = deep.channels_per_sample;

  /* Get current sample count for this pixel */
  const int old_sample_count = deep.sample_counts[pixel_idx];
  const int new_sample_count = old_sample_count + sample_count;

  /* Update sample count */
  deep.sample_counts[pixel_idx] = new_sample_count;

  /* Append depth values */
  const int old_depth_size = deep.depths.size();
  deep.depths.resize(old_depth_size + sample_count);
  memcpy(&deep.depths[old_depth_size], depths, sample_count * sizeof(float));

  /* Append channel data */
  const int old_channel_size = deep.channel_data.size();
  deep.channel_data.resize(old_channel_size + sample_count * channels_per_sample);
  memcpy(&deep.channel_data[old_channel_size],
         channels,
         sample_count * channels_per_sample * sizeof(float));

  /* Note: sample_offsets will be rebuilt in IMB_deep_finalize() */

  return true;
}

void IMB_deep_finalize(ImBuf *ibuf, bool sort_by_depth)
{
  if (!ibuf || !(ibuf->flags & IB_deep_data)) {
    return;
  }

  ImBufDeepBuffer &deep = *ibuf->deep_buffer;
  const int pixel_count = ibuf->x * ibuf->y;
  const int channels_per_sample = deep.channels_per_sample;

  if (deep.sample_counts.is_empty()) {
    return;
  }

  /* Rebuild sample_offsets array (prefix sum) */
  deep.sample_offsets.reinitialize(pixel_count + 1);
  deep.sample_offsets[0] = 0;

  for (int i = 0; i < pixel_count; i++) {
    deep.sample_offsets[i + 1] = deep.sample_offsets[i] + deep.sample_counts[i];
  }

  /* Sort samples by depth if requested */
  if (sort_by_depth && !deep.depths.is_empty()) {
    /* Create temporary arrays for sorting */
    Vector<int> indices;
    Vector<float> temp_depths;
    Vector<float> temp_channels;

    for (int pixel_idx = 0; pixel_idx < pixel_count; pixel_idx++) {
      const int sample_count = deep.sample_counts[pixel_idx];
      if (sample_count <= 1) {
        continue; /* No need to sort single or zero samples */
      }

      const int sample_offset = deep.sample_offsets[pixel_idx];

      /* Create indices for sorting */
      indices.reinitialize(sample_count);
      for (int i = 0; i < sample_count; i++) {
        indices[i] = i;
      }

      /* Sort indices by depth (front-to-back) */
      const float *pixel_depths = &deep.depths[sample_offset];
      std::sort(indices.begin(), indices.end(), [pixel_depths](int a, int b) {
        return pixel_depths[a] < pixel_depths[b];
      });

      /* Reorder depths */
      temp_depths.reinitialize(sample_count);
      for (int i = 0; i < sample_count; i++) {
        temp_depths[i] = deep.depths[sample_offset + indices[i]];
      }
      memcpy(&deep.depths[sample_offset], temp_depths.data(), sample_count * sizeof(float));

      /* Reorder channel data */
      temp_channels.reinitialize(sample_count * channels_per_sample);
      for (int i = 0; i < sample_count; i++) {
        const int src_idx = sample_offset + indices[i];
        const int dst_idx = i;
        memcpy(&temp_channels[dst_idx * channels_per_sample],
               &deep.channel_data[src_idx * channels_per_sample],
               channels_per_sample * sizeof(float));
      }
      memcpy(&deep.channel_data[sample_offset * channels_per_sample],
             temp_channels.data(),
             sample_count * channels_per_sample * sizeof(float));
    }
  }
}

ImBuf *flatten_deep_to_float(const ImBuf *deep_ibuf, const DeepFlattenOptions *options)
{
  if (!(deep_ibuf->flags & IB_deep_data)) {
    return nullptr;
  }

  const ImBufDeepBuffer &deep = *deep_ibuf->deep_buffer;
  const int width = deep_ibuf->x;
  const int height = deep_ibuf->y;

  if (deep.sample_counts.is_empty() || deep.channels_per_sample == 0) {
    return nullptr;
  }

  /* Set default options if none provided */
  DeepFlattenOptions default_opts;
  if (!options) {
    options = &default_opts;
  }

  /* Allocate output ImBuf with float buffer. */
  ImBuf *result = IMB_allocImBuf(width, height, 32, IB_float_data);
  if (!result) {
    return nullptr;
  }

  /* Copy metadata. */
  result->ftype = deep_ibuf->ftype;
  result->flags = deep_ibuf->flags & ~IB_deep_data; /* Remove deep flag. */
  result->float_buffer.colorspace = deep_ibuf->float_buffer.colorspace;

  /* Flatten deep pixels. */
  threading::parallel_for(IndexRange(height), 64, [&](IndexRange y_range) {
    for (const int64_t y : y_range) {
      for (int x = 0; x < width; x++) {
        const int pixel_idx = y * width + x;
        float *out_pixel = result->float_buffer.data + pixel_idx * 4;

        /* Initialize output to transparent black. */
        out_pixel[0] = 0.0f;
        out_pixel[1] = 0.0f;
        out_pixel[2] = 0.0f;
        out_pixel[3] = 0.0f;

        const int sample_start = deep.sample_offsets[pixel_idx];
        const int num_samples = deep.sample_counts[pixel_idx];

        if (num_samples == 0) {
          continue; /* No samples, leave as transparent. */
        }

        /* Limit number of samples to process. */
        const int samples_to_process = std::min(num_samples, options->max_samples);

        /* Track sample count for averaging mode. */
        int valid_samples = 0;

        /* Composite samples. */
        for (int s = 0; s < samples_to_process; s++) {
          const int sample_idx = sample_start + s;

          /* Depth range filtering. */
          const float depth = deep.depths[sample_idx];
          if (depth < options->depth_min || depth > options->depth_max) {
            continue;
          }

          const float *sample_channels = deep.channel_data.data() +
                                         sample_idx * deep.channels_per_sample;

          /* Extract RGBA channels. */
          const float src_r = (deep.channels_per_sample > 0) ? sample_channels[0] : 0.0f;
          const float src_g = (deep.channels_per_sample > 1) ? sample_channels[1] : 0.0f;
          const float src_b = (deep.channels_per_sample > 2) ? sample_channels[2] : 0.0f;
          const float src_a = (deep.channels_per_sample > 3) ? sample_channels[3] : 1.0f;

          valid_samples++;

          /* Apply compositing mode. */
          switch (options->mode) {
            case DEEP_FLATTEN_COMPOSITE: {
              /* Front-to-back "over" operation (assumes premultiplied alpha). */
              const float one_minus_dst_a = 1.0f - out_pixel[3];
              out_pixel[0] += src_r * one_minus_dst_a;
              out_pixel[1] += src_g * one_minus_dst_a;
              out_pixel[2] += src_b * one_minus_dst_a;
              out_pixel[3] += src_a * one_minus_dst_a;

              /* Early out if fully opaque. */
              if (out_pixel[3] >= options->alpha_threshold) {
                out_pixel[3] = 1.0f;
                goto pixel_done; /* Break out of both loops */
              }
              break;
            }

            case DEEP_FLATTEN_NEAREST: {
              /* Use first valid sample only. */
              out_pixel[0] = src_r;
              out_pixel[1] = src_g;
              out_pixel[2] = src_b;
              out_pixel[3] = src_a;
              goto pixel_done; /* Done after first sample */
            }

            case DEEP_FLATTEN_FARTHEST: {
              /* Keep overwriting (last valid sample wins). */
              out_pixel[0] = src_r;
              out_pixel[1] = src_g;
              out_pixel[2] = src_b;
              out_pixel[3] = src_a;
              break;
            }

            case DEEP_FLATTEN_AVERAGE:
            case DEEP_FLATTEN_SUM: {
              /* Accumulate values. */
              out_pixel[0] += src_r;
              out_pixel[1] += src_g;
              out_pixel[2] += src_b;
              out_pixel[3] += src_a;
              break;
            }
          }
        }

        /* Finalize based on mode. */
        if (options->mode == DEEP_FLATTEN_AVERAGE && valid_samples > 0) {
          const float inv_samples = 1.0f / valid_samples;
          out_pixel[0] *= inv_samples;
          out_pixel[1] *= inv_samples;
          out_pixel[2] *= inv_samples;
          out_pixel[3] *= inv_samples;
        }

      pixel_done:;
      }
    }
  });

  return result;
}

}  // namespace blender
