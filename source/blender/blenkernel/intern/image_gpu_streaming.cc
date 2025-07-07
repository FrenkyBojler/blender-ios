#include "BKE_image.hh"

#include "BLI_math_bits.h"
#include "BLI_time.h"

#include "GPU_texture.hh"

#include "IMB_imbuf.hh"

#include "CLG_log.h"

static CLG_LogRef LOG = {"image.streaming"};

namespace blender::bke {

ImageMipmapLevel ImageMipmapMask::lowest_mipmap_level(int num_levels) const
{
  if (mask == 0) {
    return ImageMipmapLevel(16);
  }
  uint scan = bitscan_reverse_uint(mask);
  int highest_resolution_log = 31 - scan;
  int highest_level = 16 - highest_resolution_log;
  int offset = 16 - num_levels;
  return ImageMipmapLevel(std::clamp(highest_level - offset, 0, num_levels));
}

ImageMipmapCache::ImageMipmapCache()
    : last_texture_mipmap_level_(0), mipmap_level_last_used_timestamp_(0.0)
{
}
ImageMipmapCache::~ImageMipmapCache()
{
  clear();
}

void ImageMipmapCache::clear()
{
  GPU_TEXTURE_FREE_SAFE(last_texture_);
  offsets_per_mipmap_.clear();
  bytes_per_mipmap_.clear();
  resolution_per_mipmap_.clear();
  data_.reinitialize(0);
  bytes_all_mips_ = 0;
}

void ImageMipmapCache::init_resolution_size_offset_for_each_mipmap_level(uint2 mipmap0_resolution,
                                                                         int64_t bytes_per_pixel)
{
  bytes_all_mips_ = 0;
  uint2 mipmap_resolution(mipmap0_resolution);

  /* Determine resolution, size and offset for each mipmap level. */
  while (mipmap_resolution.x > 1 || mipmap_resolution.y > 1) {
    offsets_per_mipmap_.append(bytes_all_mips_);
    int64_t bytes_mipmap = mipmap_resolution.x * mipmap_resolution.y * bytes_per_pixel;
    bytes_per_mipmap_.append(bytes_mipmap);
    resolution_per_mipmap_.append(mipmap_resolution);

    bytes_all_mips_ += bytes_mipmap;

    mipmap_resolution.x = std::max<uint>(1, mipmap_resolution.x >> 1);
    mipmap_resolution.y = std::max<uint>(1, mipmap_resolution.y >> 1);
  }
  BLI_assert(mipmap_resolution.x == 1);
  BLI_assert(mipmap_resolution.y == 1);
  offsets_per_mipmap_.append(bytes_all_mips_);
  bytes_all_mips_ += bytes_per_pixel;
  resolution_per_mipmap_.append(mipmap_resolution);
  bytes_per_mipmap_.append(bytes_per_pixel);

  for (int mipmap_level : IndexRange(size())) {
    CLOG_INFO(&LOG,
              2,
              "mipmap=%d, resolution=%dx%d, offset=%lu, size_in_bytes=%lu, bytes_per_pixel=%lu",
              mipmap_level,
              UNPACK2(resolution_per_mipmap_[mipmap_level]),
              offsets_per_mipmap_[mipmap_level],
              bytes_per_mipmap_[mipmap_level],
              bytes_per_pixel);
  }
}

void ImageMipmapCache::init_mipmap_level_clamping()
{
  // TODO: check resolution that can be allocated.
  mipmap_level_clamp_min_ = 0;

  /* Max clamping is calculated by the mipmaps that can fit in 4kb of allocation.. */
  int64_t bytes = 0;
  int mipmap_level = 0;
  for (mipmap_level = size() - 1; mipmap_level != 0; mipmap_level--) {
    bytes += bytes_per_mipmap_[mipmap_level];
    if (bytes > mipmap_level_max_clamping_byte_size) {
      break;
    }
  }
  if (bytes > mipmap_level_max_clamping_byte_size) {
    mipmap_level++;
  }
  mipmap_level_clamp_max_ = mipmap_level;

  CLOG_INFO(&LOG,
            3,
            "init clamping image=%s,min=%d,max=%d",
            name_.c_str(),
            mipmap_level_clamp_min_,
            mipmap_level_clamp_max_);
}

void ImageMipmapCache::update_mipmap_cache(const ImBuf &imbuf,
                                           bool use_high_bitdef,
                                           bool use_greyscale,
                                           blender::StringRefNull name)
{
  clear();
  name_ = name;
  CLOG_INFO(&LOG, 2, "update mipmap cache name=%s", name_.c_str());

  eGPUTextureFormat texture_format = IMB_gpu_get_texture_format(
      &imbuf, use_high_bitdef, use_greyscale);
  gpu_texture_format_ = texture_format;
  eGPUDataFormat data_format = imbuf.float_buffer.data ? GPU_DATA_FLOAT : GPU_DATA_UBYTE;
  gpu_data_format_ = data_format;

  int bytes_per_pixel = 4;
  if (texture_format == GPU_SRGB8_A8) {
    bytes_per_pixel = 4;
  }
  else if (texture_format == GPU_R16F) {
    // For now as we don't support OPENXR_HALF
    bytes_per_pixel = 4;
  }
  else if (texture_format == GPU_R32F) {
    bytes_per_pixel = 4;
  }
  else if (texture_format == GPU_RGBA16F) {
    // For now as we don't support OPENXR_HALF
    bytes_per_pixel = 16;
  }
  else if (texture_format == GPU_RGBA32F) {
    bytes_per_pixel = 16;
  }

  // TODO calculate correct bytes per pixel get texture format from imbuf.
  init_resolution_size_offset_for_each_mipmap_level(uint2(imbuf.x, imbuf.y), bytes_per_pixel);
  init_mipmap_level_clamping();

  data_.reinitialize(bytes_all_mips_);
  data_.fill(0);

  ImBuf *mipmap_ibuf = IMB_dupImBuf(&imbuf);
  for (int mipmap_level : IndexRange(size())) {
    uint2 mipmap_resolution = resolution_per_mipmap_[mipmap_level];
    IMB_scale(mipmap_ibuf, UNPACK2(mipmap_resolution), IMBScaleFilter::Bilinear);

    uint8_t *source_buf = nullptr;
    if (mipmap_ibuf->float_buffer.data) {
      source_buf = (uint8_t *)(mipmap_ibuf->float_buffer.data);
    }
    else {
      source_buf = mipmap_ibuf->byte_buffer.data;
    }
    MutableSpan<uint8_t> mipmap_dst = mipmap_data_mutable(mipmap_level);
    int64_t mipmap_size = bytes_per_mipmap_[mipmap_level];
    Span<uint8_t> source(source_buf, mipmap_size);
    mipmap_dst.copy_from(source);
  }
  IMB_freeImBuf(mipmap_ibuf);
}

void ImageMipmapCache::reset_mipmap_timestamp(ImageMipmapLevel mipmap_level, double timestamp)
{
  if (mipmap_level == last_texture_mipmap_level_) {
    mipmap_level_last_used_timestamp_ = timestamp;
  }
}

bool ImageMipmapCache::recreate_mipmap(ImageMipmapLevel mipmap_level, double timestamp)
{
  const bool texture_availale = last_texture_ != nullptr;
  const bool texture_contains_mipmap = last_texture_mipmap_level_.contains(mipmap_level);
  const bool mipmap_level_elapsed = (timestamp - mipmap_level_last_used_timestamp_) >
                                    unused_mipmap_level0_elapse_time;

  return (!texture_availale || !texture_contains_mipmap || mipmap_level_elapsed);
}

ImageGPUTextures ImageMipmapCache::gpu_mipmap_texture_get_try(ImageMipmapMask mipmap_mask,
                                                              double timestamp)
{
  ImageMipmapLevel mipmap_level(std::clamp(mipmap_mask.lowest_mipmap_level(size()).level,
                                           mipmap_level_clamp_min_,
                                           mipmap_level_clamp_max_));
  reset_mipmap_timestamp(mipmap_level, timestamp);
  const bool recreate = recreate_mipmap(mipmap_level, timestamp);
  return {&last_texture_, nullptr, recreate};
}

ImageGPUTextures ImageMipmapCache::gpu_mipmap_texture_get(ImageMipmapMask mipmap_mask,
                                                          double timestamp)
{
  ImageMipmapLevel mipmap_level(std::clamp(mipmap_mask.lowest_mipmap_level(size()).level,
                                           mipmap_level_clamp_min_,
                                           mipmap_level_clamp_max_));
  reset_mipmap_timestamp(mipmap_level, timestamp);
  const bool recreate = recreate_mipmap(mipmap_level, timestamp);
  if (!recreate) {
    return {&last_texture_, nullptr, false};
  }

  CLOG_INFO(&LOG,
            2,
            "recreate texture image=%s,mask=%d,old_mipmap=%d,new_mipmap=%d",
            name_.c_str(),
            mipmap_mask,
            last_texture_mipmap_level_.level,
            mipmap_level.level);
  GPUTexture *old_texture = last_texture_;
  ImageMipmapLevel old_mipmap_level = last_texture_mipmap_level_;
  GPUTexture *new_texture = GPU_texture_create_2d(
      name_.c_str(),
      UNPACK2(resolution_per_mipmap_[mipmap_level.level]),
      offsets_per_mipmap_.size() - mipmap_level.level,
      (eGPUTextureFormat)gpu_texture_format_,
      GPU_TEXTURE_USAGE_GENERAL,
      nullptr);
  GPU_texture_original_size_set(new_texture, UNPACK2(resolution_per_mipmap_[0]));

  /* Copy mipmap levels that are shared between the old and new texture. */
  int new_mipmap_len = size() - mipmap_level.level;
  int old_mipmap_len = old_texture ? size() - old_mipmap_level.level : 0;
  int mipmap_levels_to_copy = std::min(new_mipmap_len, old_mipmap_len);
  int mipmap_levels_to_upload = new_mipmap_len - mipmap_levels_to_copy;
  if (mipmap_levels_to_copy != 0) {
    GPU_texture_copy_mipmaps(new_texture,
                             old_texture,
                             mipmap_levels_to_upload,
                             old_mipmap_len - mipmap_levels_to_copy,
                             mipmap_levels_to_copy);
  }

  /* Upload missing mipmap levels, those are the lower levels (higher resolution). */
  // TODO: stream in missing mipmap levels in a background thread.
  // TODO: Load missing mipmaps in from disk.
  for (int mipmap : IndexRange(mipmap_level.level, mipmap_levels_to_upload)) {
    GPU_texture_update_mipmap(new_texture,
                              mipmap - mipmap_level.level,
                              (eGPUDataFormat)gpu_data_format_,
                              mipmap_data(mipmap).data());
  }

  GPU_TEXTURE_FREE_SAFE(last_texture_);
  old_texture = nullptr;
  last_texture_ = new_texture;
  last_texture_mipmap_level_ = mipmap_level;
  mipmap_level_last_used_timestamp_ = timestamp;
  return {&last_texture_, nullptr, false};
}
}  // namespace blender::bke
