#include "BKE_image.hh"

#include "BLI_math_bits.h"

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

ImageMipmapCache::ImageMipmapCache() : last_texture_mipmap_level_(0) {}
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
    mipmap_level--;
  }
  mipmap_level_clamp_max_ = size() - 1 - mipmap_level;

  CLOG_INFO(&LOG,
            3,
            "initialized mipmap level clamping to min=%d,max=%d",
            mipmap_level_clamp_min_,
            mipmap_level_clamp_max_);
}

void ImageMipmapCache::update_mipmap_cache(const ImBuf &imbuf,
                                           bool use_high_bitdef,
                                           bool use_greyscale)
{
  clear();
  CLOG_INFO(&LOG, 2, "update mipmap cache");

  eGPUTextureFormat texture_format = IMB_gpu_get_texture_format(
      &imbuf, use_high_bitdef, use_greyscale);
  BLI_assert(ELEM(texture_format, GPU_SRGB8_A8, GPU_RGBA8UI));

  // TODO calculate correct bytes per pixel get texture format from imbuf.
  init_resolution_size_offset_for_each_mipmap_level(uint2(imbuf.x, imbuf.y), 4);
  init_mipmap_level_clamping();

  data_.reinitialize(bytes_all_mips_);
  data_.fill(0);

  ImBuf *mipmap_ibuf = IMB_dupImBuf(&imbuf);
  for (int mipmap_level : IndexRange(size())) {
    uint2 mipmap_resolution = resolution_per_mipmap_[mipmap_level];
    IMB_scale(mipmap_ibuf, UNPACK2(mipmap_resolution), IMBScaleFilter::Bilinear);
    mipmap_data_mutable(mipmap_level)
        .copy_from(Span<uint8_t>(mipmap_ibuf->byte_buffer.data, bytes_per_mipmap_[mipmap_level]));
  }
  IMB_freeImBuf(mipmap_ibuf);
}

ImageGPUTextures ImageMipmapCache::gpu_mipmap_texture_get_try(ImageMipmapMask mipmap_mask)
{
  ImageMipmapLevel clamped_mipmap_level(std::clamp(mipmap_mask.lowest_mipmap_level(size()).level,
                                                   mipmap_level_clamp_min_,
                                                   mipmap_level_clamp_max_));
  CLOG_INFO(&LOG,
            4,
            "querying texture_ready=%c mipmap_level=%d, clamped_mipmap_level=%d",
            last_texture_ == nullptr ? 'n' : 'y',
            last_texture_mipmap_level_.level,
            clamped_mipmap_level);
  return {&last_texture_, nullptr, last_texture_mipmap_level_ != clamped_mipmap_level};
}

ImageGPUTextures ImageMipmapCache::gpu_mipmap_texture_get(ImageMipmapMask mipmap_mask)
{
  CLOG_INFO(&LOG, 3, "requesting mipmap mask %d", mipmap_mask.mask);
  ImageMipmapLevel clamped_mipmap_level(std::clamp(mipmap_mask.lowest_mipmap_level(size()).level,
                                                   mipmap_level_clamp_min_,
                                                   mipmap_level_clamp_max_));
  if (last_texture_ != nullptr && clamped_mipmap_level == last_texture_mipmap_level_) {
    CLOG_INFO(&LOG,
              3,
              "requested clamped mipmap level %d is same as last used. Using cached GPU texture",
              clamped_mipmap_level.level);
    return {&last_texture_, nullptr, false};
  }

  CLOG_INFO(&LOG, 2, "recreate new mipmap level texture %d", clamped_mipmap_level.level);
  GPU_TEXTURE_FREE_SAFE(last_texture_);
  last_texture_ = GPU_texture_create_2d(
      __func__,
      UNPACK2(resolution_per_mipmap_[clamped_mipmap_level.level]),
      offsets_per_mipmap_.size() - clamped_mipmap_level.level,
      GPU_SRGB8_A8,
      GPU_TEXTURE_USAGE_GENERAL,
      nullptr);
  last_texture_mipmap_level_ = clamped_mipmap_level;

  /* Upload missing mipmap levels */
  // TODO: in stead of copying all mipmaps, only copy the mipmap levels that are not available
  // in the last texture. The other could be copied from the current last texture.
  for (int mipmap = clamped_mipmap_level.level; mipmap < size(); mipmap++) {
    GPU_texture_update_mipmap(last_texture_,
                              mipmap - clamped_mipmap_level.level,
                              GPU_DATA_UBYTE,
                              mipmap_data(mipmap).data());
  }

  return {&last_texture_, nullptr, false};
}
}  // namespace blender::bke
