
#include "BLI_fileops.h"
#include "BLI_serialize.hh"
#include "BLI_string_ref.hh"

#include "BKE_blender_updates.hh"
#include "BKE_blender_version.h"
#include "BKE_global.hh"
#include "BKE_idprop.hh"

#include "DNA_userdef_types.h"

#include <chrono>

#ifdef WITH_PYTHON
#  include "BPY_extern_run.hh"
#endif

namespace blender::bke {

struct BlenderUpdates {
  std::optional<VersionUpdate> latest;
  std::optional<VersionUpdate> latest_lts;
  std::optional<VersionUpdate> current_release;
};

struct BlenderVersion {
  int version;
  int patch;
  friend auto operator<=>(const BlenderVersion &a, const BlenderVersion &b) = default;
};

struct IgnoredBlenderVersions {
  BlenderVersion latest;
  BlenderVersion latest_lts;
  BlenderVersion current_release;
};

std::string VersionUpdate::date() const
{
  char time[8];
  char date[16];
  bool is_today;
  bool is_yesterday;

  BLI_filelist_entry_datetime_to_string(
      nullptr, int64_t(this->time), false, time, date, &is_today, &is_yesterday);
  return date;
}

static IgnoredBlenderVersions &ignored_versions_updates()
{
  /* Ignore any update prior the current version. */
  static IgnoredBlenderVersions ignored_blender_versions{
      {BLENDER_VERSION, BLENDER_VERSION_PATCH},
      {BLENDER_VERSION, BLENDER_VERSION_PATCH},
      {BLENDER_VERSION, BLENDER_VERSION_PATCH},
  };
  return ignored_blender_versions;
}

static BlenderUpdates &available_blender_updates()
{
  static BlenderUpdates available_blender_updates{};
  return available_blender_updates;
}

std::optional<BlenderVersion> blender_version_from_version_str(StringRefNull str)
{
  std::stringstream ss(str);
  int major;
  ss >> major;
  if (ss.fail()) {
    return {};
  }
  char sep = '.';
  ss >> sep;
  if (ss.fail() || sep != '.') {
    return {};
  }
  int minor;
  ss >> minor;
  if (ss.fail()) {
    return {};
  }
  ss >> sep;
  if (ss.fail() || sep != '.') {
    return {};
  }
  int patch;
  ss >> patch;
  if (ss.fail() || !ss.eof()) {
    return {};
  }
  return BlenderVersion{major * 100 + minor, patch};
}

static void register_blender_update(VersionUpdate update)
{
  std::optional<BlenderVersion> version_opt = blender_version_from_version_str(update.version);
  if (!version_opt) {
    return;
  }
  BlenderVersion version = *version_opt;
  IgnoredBlenderVersions &ignored_updates = ignored_versions_updates();
  BlenderUpdates &updates = available_blender_updates();
  auto version_from_optional_version_update = [](std::optional<VersionUpdate> version_update) {
    return version_update ? blender_version_from_version_str(version_update->version) :
                            std::nullopt;
  };
  if (version.version == BLENDER_VERSION) {
    std::optional<BlenderVersion> current_version = version_from_optional_version_update(
        updates.current_release);
    if (ignored_updates.current_release.patch < version.patch &&
        (!current_version || *current_version < version))
    {
      updates.current_release = update;
    }
    return;
  }
  if (update.is_lts && ignored_updates.latest_lts < version) {
    std::optional<BlenderVersion> latest_lts_version = version_from_optional_version_update(
        updates.latest_lts);
    if (!latest_lts_version || *latest_lts_version < version) {
      updates.latest_lts = update;
    }
  }
  if (ignored_updates.latest < version) {
    std::optional<BlenderVersion> latest_version = version_from_optional_version_update(
        updates.latest);
    if (!latest_version || *latest_version < version) {
      updates.latest = update;
    }
  }
}

static std::string download_updates_info(bContext &C)
{
  constexpr const char *expr =
      R"(
import tempfile
with tempfile.TemporaryDirectory() as temp_dir:
    from pathlib import Path
    output_dir = Path(temp_dir)
    
    from _bpy_internal.http import downloader as http_dl
    metadata_provider = http_dl.MetadataProviderFilesystem(cache_location= output_dir / "http_metadata")
    
    downloader = http_dl.ConditionalDownloader(metadata_provider=metadata_provider)
    downloader.download_to_file("http://localhost:8000/updates.json", Path(output_dir / "blender-updates.json"))
    
    import os
    with open(os.path.join(temp_dir,  "blender-updates.json"), 'r') as file:
        _result = file.read()
)";
  std::unique_ptr locals = bke::idprop::create_group("locals");
  std::optional<blender::IDProperty *> updates_ptr = BPY_run_string_exec_with_locals_return_idprop(
      &C, expr, *locals, "_result");
  if (!updates_ptr) {
    return "";
  }
  IDProperty *updates_idprop = *updates_ptr;

  /* Check the returned value. */
  if (updates_idprop == nullptr || updates_idprop->type != IDP_STRING) {
    IDP_FreeProperty(updates_idprop);
    return "";
  }
  std::string updates_str = IDP_string_get(updates_idprop);
  IDP_FreeProperty(updates_idprop);

  using namespace io::serialize;

  std::istringstream updates_stream(updates_str);

  JsonFormatter json;
  std::unique_ptr<Value> updates_json = json.deserialize(updates_stream);

  if (!updates_json) {
    return "";
  }
  if (updates_json->type() != eValueType::Array) {
    return "";
  }
  for (const std::shared_ptr<Value> &entry : updates_json->as_array_value()->elements()) {
    if (!entry || entry->type() != eValueType::Dictionary) {
      continue;
    }
    const DictionaryValue &dict = *entry->as_dictionary_value();
    std::optional<int64_t> build_size = dict.lookup_int("build_size");
    std::optional<StringRefNull> checksum_hash = dict.lookup_str("checksum_hash");
    std::optional<StringRefNull> commit_hash = dict.lookup_str("commit_hash");
    std::optional<StringRefNull> description = dict.lookup_str("description");
    std::optional<StringRefNull> download_url = dict.lookup_str("download_url");
    std::optional<StringRefNull> cycle = dict.lookup_str("cycle");
    const std::shared_ptr<blender::io::serialize::Value> *is_lts = dict.lookup("is_lts");
    std::optional<StringRefNull> platform = dict.lookup_str("platform");
    std::optional<StringRefNull> release_notes_url = dict.lookup_str("release_notes_url");
    std::optional<StringRefNull> timestamp = dict.lookup_str("timestamp");
    std::optional<StringRefNull> version = dict.lookup_str("version");
    if (!(build_size && checksum_hash && commit_hash && description && download_url &&
          download_url && cycle && platform && release_notes_url && timestamp && version))
    {
      continue;
    }
    if (!is_lts || is_lts->get()->type() != eValueType::Boolean) {
      continue;
    }
    std::string timestamp_value = *timestamp;
    std::istringstream is(timestamp_value);
    std::chrono::sys_seconds time;
    if (!(is >> std::chrono::parse("%Y-%m-%dT%H:%M:%SZ", time))) {
      continue;
    }
    register_blender_update(VersionUpdate{.build_size = *build_size,
                                          .checksum_hash = *checksum_hash,
                                          .commit_hash = *commit_hash,
                                          .description = *description,
                                          .download_url = *download_url,
                                          .cycle = *cycle,
                                          .is_lts = is_lts->get()->as_boolean_value()->value(),
                                          .platform = *platform,
                                          .release_notes_url = *release_notes_url,
                                          .timestamp = *timestamp,
                                          .version = *version,
                                          .time = std::chrono::system_clock::to_time_t(time)});
  }

  return "";
}

bool check_for_available_updates(bContext &C, bool use_cache, bool ignore_skipped_versions)
{
  if (!(G.f & G_FLAG_INTERNET_ALLOW &&
        (U.flag & (USER_BLENDER_UPDATE_LATEST_RELEASE | USER_BLENDER_UPDATE_LATEST_LTS_RELEASE |
                   USER_BLENDER_UPDATE_CURRENT_RELEASE))))
  {
    return false;
  }
  if (ignore_skipped_versions) {
    ignored_versions_updates() = {
        {BLENDER_VERSION, BLENDER_VERSION_PATCH},
        {BLENDER_VERSION, BLENDER_VERSION_PATCH},
        {BLENDER_VERSION, BLENDER_VERSION_PATCH},
    };
  }
  static std::chrono::utc_clock::time_point last_time_check;
  if (!use_cache || std::chrono::duration_cast<std::chrono::days>(
                        (std::chrono::utc_clock::now() - last_time_check))
                            .count() > 1)
  {
    download_updates_info(C);
    last_time_check = std::chrono::utc_clock::now();
  }
  return !available_updates().is_empty();
}

Vector<const VersionUpdate *> available_updates()
{
  BlenderUpdates &updates = available_blender_updates();
  Vector<const VersionUpdate *> tmp;
  if (U.flag & USER_BLENDER_UPDATE_LATEST_LTS_RELEASE && updates.latest_lts) {
    tmp.append(&(*updates.latest_lts));
  }
  if (U.flag & USER_BLENDER_UPDATE_LATEST_RELEASE && updates.latest &&
      (!updates.latest_lts || (*blender_version_from_version_str(updates.latest_lts->version) <
                               *blender_version_from_version_str(updates.latest->version))))
  {
    tmp.append(&(*updates.latest_lts));
  }
  if (U.flag & USER_BLENDER_UPDATE_CURRENT_RELEASE && updates.current_release) {
    tmp.append(&(*updates.current_release));
  }
  std::ranges::sort(tmp, [](const VersionUpdate *a, const VersionUpdate *b) {
    return (blender_version_from_version_str(a->version)) <
           blender_version_from_version_str(b->version);
  });
  std::ranges::reverse(tmp);
  return tmp;
}

void ignore_update_version(const VersionUpdate &version_info)
{
  std::optional<BlenderVersion> version = blender_version_from_version_str(version_info.version);
  BLI_assert(version);
  IgnoredBlenderVersions &ignored_updates = ignored_versions_updates();

  BlenderUpdates &updates = available_blender_updates();

  if (version->version == BLENDER_VERSION &&
      version->patch > ignored_updates.current_release.patch)
  {
    ignored_updates.current_release = *version;
  }

  if (version_info.is_lts && ignored_updates.latest_lts < *version) {
    ignored_updates.latest_lts = *version;
  }

  if (ignored_updates.latest < *version) {
    ignored_updates.latest = *version;
  }

  if (updates.latest && version_info == *updates.latest) {
    updates.latest = {};
  }
  if (updates.latest_lts && version_info == *updates.latest_lts) {
    updates.latest_lts = {};
  }
  if (updates.current_release && version_info == *updates.current_release) {
    updates.current_release = {};
  }
}

void ignore_update(const VersionUpdate *version_info)
{
  ignore_update_version(*version_info);
}

void ignore_all_updates()
{
  BlenderUpdates &updates = available_blender_updates();
  if (updates.latest) {
    ignore_update_version(*updates.latest);
  }
  if (updates.latest_lts) {
    ignore_update_version(*updates.latest_lts);
  }
  if (updates.current_release) {
    ignore_update_version(*updates.current_release);
  }
}

}  // namespace blender::bke
