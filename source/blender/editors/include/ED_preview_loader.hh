/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include "BLI_map.hh"
#include "BLI_set.hh"

#include "DNA_ID_enums.h"

#include "ED_async_resource_loader.hh"

struct ImBuf;
struct PreviewImage;
struct wmTimer;
enum ThumbSource : int8_t;

namespace blender::ed {

enum class PreviewPriority {
  Low = 0,
  Medium = 1,
  High = 2,
};

class PreviewLoader {
  struct PreviewResourceInfo;
  struct OnlinePreviewRequests;

  /** The loader managing the asynchronous work. */
  AsyncResourceLoader<PreviewResourceInfo> async_loader_;

  Map<std::string, std::unique_ptr<OnlinePreviewRequests>> downloading_previews_;
  wmTimer *redraw_timer_ = nullptr;

 public:
  PreviewLoader(const PreviewLoader &) = delete;
  PreviewLoader &operator=(const PreviewLoader &) = delete;

  static void request(PreviewImage *preview,
                      const eIconSizes size,
                      const PreviewPriority priority = PreviewPriority::Medium);

  static void on_download_completed(const StringRef preview_full_filepath);
  static void on_download_requested(const StringRef preview_full_filepath);

 private:
  friend PreviewLoader &loader_instance();
  friend void load_preview_image(PreviewResourceInfo &request);

  PreviewLoader();

  void request_ex(PreviewImage *preview,
                  StringRef filepath,
                  eIconSizes size,
                  ThumbSource source,
                  PreviewPriority priority);
  void remember_downloading_request(PreviewImage *preview,
                                    const eIconSizes size,
                                    const PreviewPriority priority);

  /* Note that this represents a single resource, not the individual requests for it (which will be
   * de-duplicated). */
  struct PreviewResourceInfo {
    ~PreviewResourceInfo();

    std::string filepath;
    ThumbSource source = ThumbSource(0);

    /** The loaded image, assigned by #load_preview. Null before that. Afterwards, a null image
     * means there was a failure to load the preview image. */
    ImBuf *r_loaded_img = nullptr;
  };

  struct OnlinePreviewRequests {
    PreviewImage *preview;
    Set<eIconSizes> requested_sizes;
    PreviewPriority priority = PreviewPriority::Low;
  };
};

}  // namespace blender::ed
