/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edutil
 */

#include "BLI_threads.h"

#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_preview_image.hh"

#include "DNA_ID.h"
#include "DNA_ID_enums.h"

#include "ED_async_resource_loader.hh"
#include "ED_render.hh"

#include "IMB_imbuf.hh"
#include "IMB_thumbs.hh"

#include "WM_api.hh"

#include "ED_preview_loader.hh"

namespace blender::ed {

void load_preview_image(PreviewLoader::PreviewResourceInfo &request);
/* TODO duplicated from render_preview.cc. */
static void icon_copy_rect(const ImBuf *ibuf, uint w, uint h, uint *rect);

PreviewLoader &loader_instance()
{
  static PreviewLoader instance;
  return instance;
}

PreviewLoader::PreviewLoader() : async_loader_(load_preview_image) {}

PreviewLoader::PreviewResourceInfo::~PreviewResourceInfo()
{
  if (r_loaded_img) {
    IMB_freeImBuf(r_loaded_img);
  }
}

/**
 * Do the actual preview loading, executed on a thread.
 */
void load_preview_image(PreviewLoader::PreviewResourceInfo &request)
{
  const char *filepath = request.filepath.c_str();

  IMB_thumb_locks_acquire();

  IMB_thumb_path_lock(filepath);
  ImBuf *thumb = IMB_thumb_manage(filepath, THB_LARGE, request.source);
  IMB_thumb_path_unlock(filepath);

  if (thumb) {
    /* PreviewImage assumes premultiplied alpha. */
    IMB_premultiply_alpha(thumb);
  }
  request.r_loaded_img = thumb;

  IMB_thumb_locks_release();
}

static void copy_to_preview(PreviewImage *preview, const eIconSizes size, ImBuf *image)
{
  if (image) {
    if (ED_preview_use_image_size(preview, size)) {
      preview->w[size] = image->x;
      preview->h[size] = image->y;
      BLI_assert(preview->rect[size] == nullptr);
      preview->rect[size] = (uint *)MEM_dupallocN(image->byte_buffer.data);
    }
    else {
      icon_copy_rect(image, preview->w[size], preview->h[size], preview->rect[size]);
    }
  }

  /* TODO: quick hack for testing. */
  BKE_previewimg_finish(preview, size);
}

Set<std::string> &known_downloaded_previews()
{
  BLI_assert(BLI_thread_is_main());
  static Set<std::string> known_downloaded_previews;
  return known_downloaded_previews;
}

static constexpr ResourceLoadPriority preview_to_resource_load_priority(
    const PreviewPriority priority)
{
  switch (priority) {
    case PreviewPriority::High:
      return ResourceLoadPriority::High;
    case PreviewPriority::Medium:
      return ResourceLoadPriority::Medium;
    case PreviewPriority::Low:
      return ResourceLoadPriority::Low;
  }
}

void PreviewLoader::request(PreviewImage *preview,
                            const eIconSizes size,
                            const PreviewPriority priority)
{
  std::optional<StringRefNull> filepath = BKE_previewimg_deferred_filepath_get(preview);
  if (!filepath) {
    BLI_assert_unreachable();
    return;
  }
  const std::optional<int> source = BKE_previewimg_deferred_thumb_source_get(preview);
  if (!source) {
    BLI_assert_unreachable();
    return;
  }

  PreviewLoader &loader = loader_instance();

  if (!loader.redraw_timer_) {
    /* TODO: Quick hack, probably needs a proper timer which also calls into the loader to update
     * finished requests. */
    loader.redraw_timer_ = WM_event_timer_add_notifier(
        static_cast<wmWindowManager *>(G_MAIN->wm.first), nullptr, NC_WINDOW, 0.01);
  }

  preview->flag[size] |= PRV_RENDERING;

  const bool is_downloading = BKE_previewimg_is_online(preview) &&
                              !known_downloaded_previews().contains_as(*filepath);
  /* Don't push to the async loader yet. Just remember the request until the download completes. */
  if (is_downloading) {
    loader.remember_downloading_request(preview, size, priority);
    return;
  }

  loader.request_ex(preview, *filepath, size, ThumbSource(*source), priority);
}

void PreviewLoader::request_ex(PreviewImage *preview,
                               const StringRef filepath,
                               const eIconSizes size,
                               const ThumbSource source,
                               const PreviewPriority priority)
{
  async_loader_.request(
      filepath,
      /*create_custom_data=*/
      [filepath, source]() {
        PreviewResourceInfo request;
        request.filepath = filepath;
        request.source = source;
        return request;
      },
      /*process_request_fn=*/
      [preview, size](PreviewResourceInfo &request) {
        copy_to_preview(preview, size, request.r_loaded_img);
      },
      preview_to_resource_load_priority(priority));
}

void PreviewLoader::remember_downloading_request(PreviewImage *preview,
                                                 const eIconSizes size,
                                                 const PreviewPriority priority)
{
  std::optional<StringRefNull> filepath = BKE_previewimg_deferred_filepath_get(preview);
  if (!filepath) {
    BLI_assert_unreachable();
    return;
  }

  OnlinePreviewRequests &downloading_requests = *downloading_previews_.lookup_or_add_cb_as(
      *filepath, []() { return std::make_unique<OnlinePreviewRequests>(); });
  if (int(priority) > int(downloading_requests.priority)) {
    downloading_requests.priority = priority;
  }
  downloading_requests.requested_sizes.add(size);
  BLI_assert(!downloading_requests.preview || downloading_requests.preview == preview);
  downloading_requests.preview = preview;
}

void PreviewLoader::on_download_completed(const StringRef preview_full_filepath)
{
  PreviewLoader &loader = loader_instance();

  const std::unique_ptr<OnlinePreviewRequests> *existing_requests =
      loader.downloading_previews_.lookup_ptr_as(preview_full_filepath);

  if (existing_requests) {
    PreviewImage *preview = (*existing_requests)->preview;
    BLI_assert(preview_full_filepath == BKE_previewimg_deferred_filepath_get(preview));

    const std::optional<int> source = BKE_previewimg_deferred_thumb_source_get(preview);
    if (!source) {
      BLI_assert_unreachable();
      return;
    }

    for (eIconSizes size : (*existing_requests)->requested_sizes) {
      loader.request_ex(preview,
                        preview_full_filepath,
                        size,
                        ThumbSource(*source),
                        (*existing_requests)->priority);
    }
  }
  else {
    /* No pending request, but one might still be coming. Add it to the "known" downloaded
     * previews. */
    known_downloaded_previews().add_as(preview_full_filepath);
  }
}

void PreviewLoader::on_download_requested(const StringRef preview_full_filepath)
{
  /* Preview was requested. Allow the system to detect it as being downloaded by removing it from
   * the files known as "already downloaded". This way once downloaded previews don't linger around
   * as "already downloaded" forever, and their downloading state can be recognized correctly. */
  known_downloaded_previews().remove_as(preview_full_filepath);
}

static void icon_copy_rect(const ImBuf *ibuf, uint w, uint h, uint *rect)
{
  if (ibuf == nullptr ||
      (ibuf->byte_buffer.data == nullptr && ibuf->float_buffer.data == nullptr) || rect == nullptr)
  {
    return;
  }

  float scaledx, scaledy;
  if (ibuf->x > ibuf->y) {
    scaledx = float(w);
    scaledy = (float(ibuf->y) / float(ibuf->x)) * float(w);
  }
  else {
    scaledx = (float(ibuf->x) / float(ibuf->y)) * float(h);
    scaledy = float(h);
  }

  /* Scaling down must never assign zero width/height, see: #89868. */
  int ex = std::max<int>(1, scaledx);
  int ey = std::max<int>(1, scaledy);

  int dx = (w - ex) / 2;
  int dy = (h - ey) / 2;

  ImBuf *ima = IMB_scale_into_new(ibuf, ex, ey, IMBScaleFilter::Nearest, false);
  if (ima == nullptr) {
    return;
  }

  /* if needed, convert to 32 bits */
  if (ima->byte_buffer.data == nullptr) {
    IMB_byte_from_float(ima);
  }

  const uint *srect = reinterpret_cast<const uint *>(ima->byte_buffer.data);
  uint *drect = rect;

  drect += dy * w + dx;
  for (; ey > 0; ey--) {
    memcpy(drect, srect, ex * sizeof(int));
    drect += w;
    srect += ima->x;
  }

  IMB_freeImBuf(ima);
}

}  // namespace blender::ed
