/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#include "BLI_map.hh"
#include "BLI_string_ref.hh"

#include "AS_remote_library.hh"

namespace blender::asset_system {

using UrlToLibraryStatusMap = Map<std::string /*url*/, asset_system::RemoteLibraryLoadingStatus>;

static UrlToLibraryStatusMap &library_to_status_map()
{
  static UrlToLibraryStatusMap map = UrlToLibraryStatusMap{};
  return map;
}

void RemoteLibraryLoadingStatus::reset_timeout()
{
  this->last_updated_time_point = std::chrono::steady_clock::now();
}

void remote_library_status_ensure_loading(StringRef url, const float timeout)
{
  BLI_assert(timeout > 0.0f);

  RemoteLibraryLoadingStatus new_status{};
  new_status.timeout = timeout;
  new_status.status = RemoteLibraryLoadingStatus::Loading;
  new_status.reset_timeout();
  library_to_status_map().add_overwrite(url, new_status);
}

std::optional<RemoteLibraryLoadingStatus::Status> remote_library_status_get(StringRef url)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    return status->status;
  }
  return {};
}

void remote_library_status_set_finished(StringRef url)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    status->status = RemoteLibraryLoadingStatus::Finished;
    status->reset_timeout();
  }
}

void remote_library_status_set_failure(StringRef url, std::optional<StringRef> failure_message)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    status->status = RemoteLibraryLoadingStatus::Failure;
    status->failure_message = failure_message;
    status->reset_timeout();
  }
}

void remote_library_status_handle_timeout(StringRef url)
{
  if (RemoteLibraryLoadingStatus *status = library_to_status_map().lookup_ptr(url)) {
    std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() -
                                           status->last_updated_time_point;
    if (elapsed.count() >= status->timeout) {
      status->status = RemoteLibraryLoadingStatus::Failure;
      // TODO timeout message
      // status->failure_message = RPT
    }
  }
}

}  // namespace blender::asset_system
