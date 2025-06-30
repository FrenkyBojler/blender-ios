#include "BKE_image.hh"

#include "GPU_texture.hh"

#include "IMB_imbuf.hh"

#include "CLG_log.h"

static CLG_LogRef LOG = {"image.streaming"};

namespace blender::bke {
    ImageMipmapCache::ImageMipmapCache() {

    }
    ImageMipmapCache::~ImageMipmapCache() {
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

    void ImageMipmapCache::init_resolution_size_offset_for_each_mipmap_level(
        uint2 mipmap0_resolution, int64_t bytes_per_pixel)
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
    }

    void ImageMipmapCache::update_mipmap_cache(const ImBuf& imbuf) {
      clear();

      // TODO calculate correct bytes per pixel get texture format from imbuf.
      eGPUTextureFormat texture_format = GPU_RGBA8UI;
      int64_t bytes_per_pixel = 4;
      init_resolution_size_offset_for_each_mipmap_level(uint2(imbuf.x, imbuf.y), bytes_per_pixel);

      data_.reinitialize(bytes_all_mips_);
      data_.fill(0);
    }

    ImageGPUTextures ImageMipmapCache::gpu_mipmap_texture_get_try()
    {
      CLOG_INFO(&LOG,
                3,
                "querying texture_ready=%c mipmap_level=%d",
                last_texture_ == nullptr ? 'n' : 'y',
                last_texture_mipmap_level_);
      return {&last_texture_, nullptr, last_texture_mipmap_level_};
    }
    ImageGPUTextures ImageMipmapCache::gpu_mipmap_texture_get(int mipmap_level)
    {
      CLOG_INFO(&LOG, 3, "requesting mipmap level %d", mipmap_level);
      if (last_texture_ != nullptr && mipmap_level == last_texture_mipmap_level_) {
        CLOG_INFO(&LOG,
                  3,
                  "requested mipmap level %d is same as last used. Using cached GPU texture",
                  mipmap_level);
        return {&last_texture_, nullptr, last_texture_mipmap_level_};
      }

      CLOG_INFO(&LOG, 2, "recreate new mipmap level texture %d", mipmap_level);
      // TODO: in stead of copying all mipmaps, only copy the mipmap levels that are not available
      // in the last texture. The other could be copied from the current last texture.
      GPU_TEXTURE_FREE_SAFE(last_texture_);
      last_texture_ = GPU_texture_create_2d(__func__,
                                            UNPACK2(resolution_per_mipmap_[mipmap_level]),
                                            offsets_per_mipmap_.size() - mipmap_level,
                                            GPU_RGBA8UI,
                                            GPU_TEXTURE_USAGE_GENERAL,
                                            nullptr);
      last_texture_mipmap_level_ = mipmap_level;

      return {&last_texture_, nullptr, last_texture_mipmap_level_};
    }

    bool ImageMipmapCache::contains_mipmap(int mipmap_level) const
    {
      return last_texture_ != nullptr && last_texture_mipmap_level_ <= mipmap_level;
    }
}