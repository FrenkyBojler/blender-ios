/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup bke
 *
 * GPU texture API for #Image. Splits the GPU-side concerns
 * (texture lifecycle, material/viewer/2D texture retrieval, GC) off
 * #BKE_image.hh so non-GPU consumers don't pull GPU declarations in.
 */

namespace blender {

namespace gpu {
class Texture;
}  // namespace gpu

struct ImBuf;
struct Image;
struct ImageTile;
struct ImageUser;
struct Main;

/**
 * Not to be use directly.
 */
gpu::Texture *BKE_image_create_gpu_texture_from_ibuf(Image *image, ImBuf *ibuf);

/**
 * Get the #gpu::Texture for a given `Image`.
 */
gpu::Texture *BKE_image_get_gpu_texture(Image *image, ImageUser *iuser);

/*
 * Like BKE_image_get_gpu_texture, but can also get render or compositing result.
 */
gpu::Texture *BKE_image_get_gpu_viewer_texture(Image *image, ImageUser *iuser);

/*
 * Like BKE_image_get_gpu_viewer_texture, but the image buffer is provided explicitly.
 */
gpu::Texture *BKE_image_get_gpu_viewer_texture(Image *image,
                                               ImageUser *iuser,
                                               ImBuf *image_buffer);

/*
 * Like BKE_image_get_gpu_texture, but can also return a GPU array texture and tile mapping
 * texture for UDIM tiles as used in material shaders.
 */
struct ImageGPUTextures {
  ImBuf *image_buffer = nullptr;
  ImBuf *tile_mapping_buffer = nullptr;

  ImageGPUTextures() = default;
  ~ImageGPUTextures();
  ImageGPUTextures(const ImageGPUTextures &other);
  ImageGPUTextures &operator=(const ImageGPUTextures &other);

  /* Get GPU texture pointers. */
  gpu::Texture *texture() const;
  gpu::Texture *tile_mapping() const;

  /* Get pointers to GPU texture pointers for deferred loading. */
  gpu::Texture **texture_ref() const;
  gpu::Texture **tile_mapping_ref() const;
};

ImageGPUTextures BKE_image_get_gpu_material_texture(Image *image,
                                                    ImageUser *iuser,
                                                    const bool use_tile_mapping);

/* Same as BKE_image_get_gpu_material_texture but will not load the texture if it isn't already. */
ImageGPUTextures BKE_image_get_gpu_material_texture_try(Image *image,
                                                        ImageUser *iuser,
                                                        const bool use_tile_mapping);

/**
 * Is the alpha of the `gpu::Texture` for a given image/ibuf premultiplied.
 */
bool BKE_image_has_gpu_texture_premultiplied_alpha(Image *image, ImBuf *ibuf);

/**
 * Put an externally created GPU texture in the image cache. Should not
 * generally be used, currently needed for synthetic Image datablocks for
 * studio lights.
 */
void BKE_image_assign_gpu_texture(Image *image, gpu::Texture *texture);

/**
 * Free the process global fallback image buffer.
 */
void BKE_image_free_gpu_fallback();

/**
 * Check if image has an associated GPU texture.
 */
bool BKE_image_has_gpu_texture(Image *ima);

void BKE_image_free_gputextures(Image *ima);
void BKE_image_free_gpu_udim_textures(Image *ima);
void BKE_image_free_all_gputextures(Main *bmain);
/**
 * Same as #BKE_image_free_all_gputextures but only free animated images.
 */
void BKE_image_free_anim_gputextures(Main *bmain);

/**
 * Partial update of texture for texture painting.
 * This is often much quicker than fully updating the texture for high resolution images.
 */
void BKE_image_update_gputexture(Image *ima, ImageUser *iuser, int x, int y, int w, int h);

/**
 * Mark areas on the #gpu::Texture that need to be updated. The areas are marked in chunks.
 * The next time the #gpu::Texture is used these tiles will be refreshed. This saves time
 * when writing to the same place multiple times during foreground rendering.
 */
void BKE_image_update_gputexture_delayed(
    Image *ima, ImageTile *image_tile, ImBuf *ibuf, int x, int y, int w, int h);

/**
 * Called on entering and exiting texture paint mode, temporarily disabling/enabling
 * mipmapping on all images for quick partial texture updates. Images that didn't
 * change don't have to be re-uploaded to the GPU.
 */
void BKE_image_paint_set_mipmap(Main *bmain, bool mipmap);

}  // namespace blender
