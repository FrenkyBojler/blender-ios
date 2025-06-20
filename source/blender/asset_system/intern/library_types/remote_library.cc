/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#include "BLI_listbase.h"

#include "BLT_translation.hh"

#include "DNA_userdef_types.h"

#include "AS_remote_library.hh"
#include "remote_library.hh"

namespace blender::asset_system {

RemoteAssetLibrary::RemoteAssetLibrary(const StringRef remote_url, StringRef cache_root_path)
    : AssetLibrary(ASSET_LIBRARY_CUSTOM, "", cache_root_path)
{
  import_method_ = ASSET_IMPORT_APPEND_REUSE;
  may_override_import_method_ = false;
  remote_url_ = remote_url;
}

std::optional<AssetLibraryReference> RemoteAssetLibrary::library_reference() const
{
  int i;
  LISTBASE_FOREACH_INDEX (const bUserAssetLibrary *, asset_library, &U.asset_libraries, i) {
    if ((asset_library->flag & ASSET_LIBRARY_USE_REMOTE_URL) == 0) {
      continue;
    }

    if (asset_library->remote_url == this->remote_url_) {
      AssetLibraryReference library_ref{};
      library_ref.type = ASSET_LIBRARY_CUSTOM;
      library_ref.custom_library_index = i;
      return library_ref;
    }
  }

  return {};
}

void RemoteAssetLibrary::refresh_catalogs()
{
  this->catalog_service().reload_catalogs();
}

/* -------------------------------------------------------------------- */
/** \name Remote Library Loading Status
 * \{ */

/*
 * Note: Some of the status setters here only modify status if the current status is
 * #RemoteLibraryLoadingStatus::Loading. That is done to avoid status changes after loading ended,
 * mostly after a timeout. E.g. if the status is failed after a timeout, but Python is actually
 * still processing items, it might send a #RemoteLibraryLoadingStatus::set_finished(). The UI
 * would stop displaying an error on the next redraw, even though C++ side loading was aborted.
 */

using UrlToLibraryStatusMap = Map<std::string /*url*/, asset_system::RemoteLibraryLoadingStatus>;

static UrlToLibraryStatusMap &library_to_status_map()
{
  static UrlToLibraryStatusMap map = UrlToLibraryStatusMap{};
  return map;
}

void RemoteLibraryLoadingStatus::reset_timeout()
{
  this->last_updated_time_point_ = std::chrono::steady_clock::now();
}

void RemoteLibraryLoadingStatus::begin_loading(StringRef url, const float timeout)
{
  BLI_assert(timeout > 0.0f);

  RemoteLibraryLoadingStatus new_status{};
  new_status.timeout_ = timeout;
  new_status.status_ = RemoteLibraryLoadingStatus::Loading;
  new_status.reset_timeout();
  library_to_status_map().add_overwrite(url, new_status);
}

void RemoteLibraryLoadingStatus::ping_still_loading(StringRef url)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    if (status->status_ == RemoteLibraryLoadingStatus::Loading) {
      status->reset_timeout();
    }
  }
}

std::optional<RemoteLibraryLoadingStatus::Status> RemoteLibraryLoadingStatus::status(StringRef url)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    return status->status_;
  }
  return {};
}

void RemoteLibraryLoadingStatus::set_finished(StringRef url)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    if (status->status_ == RemoteLibraryLoadingStatus::Loading) {
      status->status_ = RemoteLibraryLoadingStatus::Finished;
      status->reset_timeout();
    }
  }
}

void RemoteLibraryLoadingStatus::set_failure(StringRef url,
                                             std::optional<StringRefNull> failure_message)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    if (status->status_ == RemoteLibraryLoadingStatus::Loading) {
      status->status_ = RemoteLibraryLoadingStatus::Failure;
      status->failure_message_ = failure_message;
      status->reset_timeout();
    }
  }
}

std::optional<StringRefNull> RemoteLibraryLoadingStatus::failure_message(StringRef url)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    if (status->status_ == RemoteLibraryLoadingStatus::Failure) {
      return status->failure_message_;
    }
  }

  return {};
}

bool RemoteLibraryLoadingStatus::handle_timeout(StringRef url)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    if (status->status_ != RemoteLibraryLoadingStatus::Loading) {
      /* Only handle timeouts while loading. */
      return false;
    }

    std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() -
                                           status->last_updated_time_point_;
    if (elapsed.count() >= status->timeout_) {
      status->status_ = RemoteLibraryLoadingStatus::Failure;
      status->failure_message_ = RPT_("Asset system lost connection to downloader (timed out).");
      return true;
    }
  }

  return false;
}

/** \} */

}  // namespace blender::asset_system
