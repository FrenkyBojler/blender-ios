/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_report.hh"
#include "BKE_wm_runtime.hh"

#include "BLI_ghash.h"
#include "BLI_listbase.h"

namespace blender::bke {

WindowManagerRuntime::WindowManagerRuntime()
{
  BKE_reports_init(&this->reports, RPT_STORE);
}

WindowManagerRuntime::~WindowManagerRuntime()
{
  BKE_reports_free(&this->reports);

  BLI_freelistN(&this->notifier_queue);
  if (this->notifier_queue_set) {
    BLI_gset_free(this->notifier_queue_set, nullptr);
  }
}

void WindowManagerRuntime::asset_library_status_ensure_loading(StringRef url, const float timeout)
{
  BLI_assert(timeout > 0.0f);

  AssetLibraryLoadingStatus new_status{};
  new_status.timeout = timeout;
  new_status.status = AssetLibraryLoadingStatus::Loading;
  new_status.touch();
  this->asset_library_statuses.add_overwrite(url, new_status);
}

std::optional<AssetLibraryLoadingStatus::Status> WindowManagerRuntime::asset_library_status_get(
    StringRef url)
{
  if (AssetLibraryLoadingStatus *status = this->asset_library_statuses.lookup_ptr(url)) {
    return status->status;
  }
  return {};
}

void WindowManagerRuntime::asset_library_status_set_finished(StringRef url)
{
  if (AssetLibraryLoadingStatus *status = this->asset_library_statuses.lookup_ptr(url)) {
    status->status = AssetLibraryLoadingStatus::Finished;
    status->touch();
  }
}

void WindowManagerRuntime::asset_library_status_set_cancelled(StringRef url)
{
  if (AssetLibraryLoadingStatus *status = this->asset_library_statuses.lookup_ptr(url)) {
    status->status = AssetLibraryLoadingStatus::Cancelled;
    status->touch();
  }
}

void WindowManagerRuntime::asset_library_status_handle_timeout(StringRef url)
{
  if (AssetLibraryLoadingStatus *status = this->asset_library_statuses.lookup_ptr(url)) {
    std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() -
                                           status->last_updated_time_point;
    if (elapsed.count() >= status->timeout) {
      status->status = AssetLibraryLoadingStatus::Cancelled;
    }
  }
}

void AssetLibraryLoadingStatus::touch()
{
  this->last_updated_time_point = std::chrono::steady_clock::now();
}

WindowRuntime::~WindowRuntime()
{
#ifdef WITH_INPUT_IME
  BLI_assert(this->ime_data == nullptr);
#endif
  /** The event_queue should be freed when the window is freed. */
  BLI_assert(BLI_listbase_is_empty(&this->event_queue));
}

}  // namespace blender::bke
