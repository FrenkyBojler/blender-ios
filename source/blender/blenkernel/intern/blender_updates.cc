
#include "BLI_fileops.h"
#include "BLI_path_utils.hh"
#include "BLI_serialize.hh"
#include "BLI_string_ref.hh"

#include "BKE_appdir.hh"
#include "BKE_blender_updates.hh"
#include "BKE_blender_version.h"
#include "BKE_global.hh"
#include "BKE_idprop.hh"

#include "DNA_userdef_types.h"

#include <chrono>
#include <format>

#ifdef WITH_PYTHON
#  include "BPY_extern_run.hh"
#endif

namespace blender::bke {

struct BlenderUpdates {
  std::optional<VersionUpdate> latest;
  std::optional<VersionUpdate> latest_lts;
  std::optional<VersionUpdate> current_release;
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
  static BlenderUpdates available_blender_updates;
  return available_blender_updates;
}

static std::chrono::utc_clock::time_point &last_time_version_update_check()
{
  static std::chrono::utc_clock::time_point last_time_version_update_check;
  return last_time_version_update_check;
}

static std::optional<std::chrono::sys_seconds> parse_timestamp_sys_seconds(StringRefNull timestamp)
{
  std::istringstream is(timestamp);
  std::chrono::sys_seconds time;
  if (!(is >> std::chrono::parse("%Y-%m-%dT%H:%M:%SZ", time))) {
    return std::nullopt;
  }
  return time;
}

static std::optional<BlenderVersion> blender_version_from_version_str(std::string str)
{
  std::stringstream ss(str);
  int major;
  ss >> major;
  if (ss.fail()) {
    return std::nullopt;
  }
  char sep = '.';
  ss >> sep;
  if (ss.fail() || sep != '.') {
    return std::nullopt;
  }
  int minor;
  ss >> minor;
  if (ss.fail()) {
    return std::nullopt;
  }
  ss >> sep;
  if (ss.fail() || sep != '.') {
    return std::nullopt;
  }
  int patch;
  ss >> patch;
  if (ss.fail() || !ss.eof()) {
    return std::nullopt;
  }
  return BlenderVersion{major * 100 + minor, patch};
}

static void register_blender_update(VersionUpdate update)
{
  BLI_assert(blender_version_from_version_str(update.version_str) &&
             update.version == blender_version_from_version_str(update.version_str));

  IgnoredBlenderVersions &ignored_updates = ignored_versions_updates();
  BlenderUpdates &updates = available_blender_updates();

  if (update.version.version == BLENDER_VERSION) {
    if (ignored_updates.current_release.patch < update.version.patch &&
        (!updates.current_release || updates.current_release->version < update.version))
    {
      updates.current_release = update;
    }
    return;
  }
  if (update.is_lts) {
    if (ignored_updates.latest_lts < update.version) {
      if (!updates.latest_lts || updates.latest_lts->version < update.version) {
        updates.latest_lts = update;
      }
      if (updates.latest && updates.latest->version < update.version) {
        updates.latest = std::nullopt;
      }
    }
    return;
  }
  if (ignored_updates.latest < update.version) {
    /* Ignore Latest releases prior to Latest LTS releases.  */
    if ((updates.latest_lts && updates.latest_lts->version >= update.version) ||
        ignored_updates.latest_lts >= update.version)
    {
      return;
    }
    if (!updates.latest || updates.latest->version < update.version) {
      updates.latest = update;
    }
  }
}

std::optional<VersionUpdate> read_version_update(io::serialize::Value *entry)
{
  using namespace io::serialize;
  if (!entry || entry->type() != eValueType::Dictionary) {
    return std::nullopt;
  }
  const DictionaryValue &dict = *entry->as_dictionary_value();
  std::optional<int64_t> build_size = dict.lookup_int("build_size");
  std::optional<StringRefNull> checksum_hash = dict.lookup_str("checksum_hash");
  std::optional<StringRefNull> commit_hash = dict.lookup_str("commit_hash");
  std::optional<StringRefNull> description = dict.lookup_str("description");
  std::optional<StringRefNull> download_url = dict.lookup_str("download_url");
  std::optional<StringRefNull> cycle = dict.lookup_str("cycle");
  const std::shared_ptr<Value> *is_lts = dict.lookup("is_lts");
  std::optional<StringRefNull> platform = dict.lookup_str("platform");
  std::optional<StringRefNull> release_notes_url = dict.lookup_str("release_notes_url");
  std::optional<StringRefNull> timestamp = dict.lookup_str("timestamp");
  std::optional<StringRefNull> version_str = dict.lookup_str("version");
  if (!(build_size && checksum_hash && commit_hash && description && download_url &&
        download_url && cycle && platform && release_notes_url && timestamp && version_str))
  {
    return std::nullopt;
  }
  if (!is_lts || is_lts->get()->type() != eValueType::Boolean) {
    return std::nullopt;
  }
  std::optional<BlenderVersion> version = blender_version_from_version_str(*version_str);
  if (!version) {
    return std::nullopt;
  }
  std::optional<std::chrono::sys_seconds> time = parse_timestamp_sys_seconds(*timestamp);
  if (!time) {
    return std::nullopt;
  }
  return VersionUpdate{
      .build_size = *build_size,
      .checksum_hash = *checksum_hash,
      .commit_hash = *commit_hash,
      .description = *description,
      .download_url = *download_url,
      .cycle = *cycle,
      .is_lts = is_lts->get()->as_boolean_value()->value(),
      .platform = *platform,
      .release_notes_url = *release_notes_url,
      .timestamp = *timestamp,
      .version_str = *version_str,
      .time = std::chrono::system_clock::to_time_t(*time),
      .version = *version,
  };
}

static bool download_updates_log(bContext &C)
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
    return false;
  }
  IDProperty *updates_idprop = *updates_ptr;

  /* Check the returned value. */
  if (updates_idprop == nullptr || updates_idprop->type != IDP_STRING) {
    IDP_FreeProperty(updates_idprop);
    return false;
  }
  std::string updates_str = IDP_string_get(updates_idprop);
  IDP_FreeProperty(updates_idprop);

  using namespace io::serialize;

  std::istringstream updates_stream(updates_str);

  JsonFormatter json;
  std::unique_ptr<Value> updates_json = json.deserialize(updates_stream);

  if (!updates_json) {
    return false;
  }
  if (updates_json->type() != eValueType::Array) {
    return false;
  }
  for (const std::shared_ptr<Value> &entry : updates_json->as_array_value()->elements()) {
    std::optional<VersionUpdate> update = read_version_update(entry.get());
    if (!update) {
      continue;
    }
    register_blender_update(*update);
  }

  return false;
}

#define BLENDER_AVAILABLE_UPDATES_FILE "available_updates.json"

void read_blender_updates_cache_file()
{
  std::optional<std::string> datafiles_path = BKE_appdir_folder_id(BLENDER_USER_CONFIG, "");
  if (!datafiles_path) {
    return;
  }
  std::string available_updates_file = *datafiles_path + SEP + BLENDER_AVAILABLE_UPDATES_FILE;
  size_t size;
  std::unique_ptr<char, MEM_smart_ptr_deleter<char>> json_text = nullptr;
  json_text.reset(BLI_file_read_text_as_mem(available_updates_file.c_str(), 0, &size));

  if (!json_text || size == 0) {
    return;
  }
  std::istringstream available_updates_stream(json_text.get());

  using namespace io::serialize;

  JsonFormatter json;
  std::unique_ptr<Value> file_json = json.deserialize(available_updates_stream);

  if (!file_json) {
    return;
  }
  if (file_json->type() != eValueType::Dictionary) {
    return;
  }
  const DictionaryValue *file_dict = file_json->as_dictionary_value();
  const DictionaryValue *ignored_versions_dict = file_dict->lookup_dict("ignored_versions");
  if (!ignored_versions_dict) {
    return;
  }
  std::optional<StringRefNull> latest_lts_ignored_str = ignored_versions_dict->lookup_str(
      "latest_lts");
  std::optional<StringRefNull> latest_ignored_str = ignored_versions_dict->lookup_str("latest");
  std::optional<StringRefNull> current_release_ignored_str = ignored_versions_dict->lookup_str(
      "current_release");
  if (!latest_lts_ignored_str || !latest_ignored_str || !current_release_ignored_str) {
    return;
  }
  std::optional<BlenderVersion> latest_lts_ignored = blender_version_from_version_str(
      *latest_lts_ignored_str);
  std::optional<BlenderVersion> latest_ignored = blender_version_from_version_str(
      *latest_ignored_str);
  std::optional<BlenderVersion> current_release_ignored = blender_version_from_version_str(
      *current_release_ignored_str);

  if (!latest_lts_ignored || !latest_ignored || !current_release_ignored) {
    return;
  }
  IgnoredBlenderVersions &ignored_updates = ignored_versions_updates();
  if (ignored_updates.latest_lts < *latest_lts_ignored) {
    ignored_updates.latest_lts = *latest_lts_ignored;
  }
  if (ignored_updates.latest < *latest_ignored) {
    ignored_updates.latest = *latest_ignored;
  }
  if (ignored_updates.current_release < *current_release_ignored) {
    ignored_updates.current_release = *current_release_ignored;
  }
  const ArrayValue *updates = file_dict->lookup_array("blender_updates");
  if (!updates) {
    return;
  }
  for (const std::shared_ptr<Value> &entry : updates->as_array_value()->elements()) {
    std::optional<VersionUpdate> update = read_version_update(entry.get());
    if (!update) {
      continue;
    }
    register_blender_update(*update);
  }
  std::optional<StringRefNull> last_time_check_str = file_dict->lookup_str("last_time_check");
  if (!last_time_check_str) {
    return;
  }
  std::optional<std::chrono::sys_seconds> last_time_check = parse_timestamp_sys_seconds(
      *last_time_check_str);
  if (!last_time_check) {
    return;
  }
  last_time_version_update_check() = std::chrono::utc_clock::from_sys(*last_time_check);
}

void write_blender_updates_cache_file()
{
  std::optional<std::string> datafiles_path = BKE_appdir_folder_id(BLENDER_USER_CONFIG, "");
  if (!datafiles_path) {
    return;
  }
  std::string available_updates_file = *datafiles_path + SEP + BLENDER_AVAILABLE_UPDATES_FILE;

  using namespace io::serialize;
  std::shared_ptr<DictionaryValue> dict = std::make_shared<DictionaryValue>();
  auto version_to_str = [](const BlenderVersion &version) {
    return fmt::format("{}.{}.{}", version.version / 100, version.version % 100, version.patch);
  };
  const IgnoredBlenderVersions &ignored_versions = ignored_versions_updates();
  std::shared_ptr<DictionaryValue> ignored_versions_dict = dict->append_dict("ignored_versions");
  ignored_versions_dict->append_str("current_release",
                                    version_to_str(ignored_versions.current_release));
  ignored_versions_dict->append_str("latest", version_to_str(ignored_versions.latest));
  ignored_versions_dict->append_str("latest_lts", version_to_str(ignored_versions.latest_lts));

  std::shared_ptr<ArrayValue> updates_array = dict->append_array("blender_updates");
  Vector<const VersionUpdate *> updates = available_updates();
  for (const VersionUpdate *update : updates) {
    std::shared_ptr<DictionaryValue> entry = std::make_unique<DictionaryValue>();
    entry->append_int("build_size", update->build_size);
    entry->append_str("checksum_hash", update->checksum_hash);
    entry->append_str("commit_hash", update->commit_hash);
    entry->append_str("description", update->description);
    entry->append_str("download_url", update->download_url);
    entry->append_str("cycle", update->cycle);
    entry->append("is_lts", std::make_unique<BooleanValue>(update->is_lts));
    entry->append_str("platform", update->platform);
    entry->append_str("release_notes_url", update->release_notes_url);
    entry->append_str("timestamp", update->timestamp);
    entry->append_str("version", update->version_str);
    updates_array->append(entry);
  }
  std::chrono::time_point time_seconds = std::chrono::time_point_cast<std::chrono::seconds>(
      last_time_version_update_check());
  dict->append_str("last_time_check", std::format("{:%FT%TZ}", time_seconds));
  io::serialize::write_json_file(available_updates_file, *dict);
}
bool check_for_available_updates(bContext &C, bool use_cache, bool ignore_skipped_versions)
{
  [[maybe_unused]] static int i = []() -> int {
    read_blender_updates_cache_file();
    return 0;
  }();
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
  if (!use_cache || std::chrono::duration_cast<std::chrono::days>(
                        (std::chrono::utc_clock::now() - last_time_version_update_check()))
                            .count() > 1)
  {
    download_updates_log(C);
    last_time_version_update_check() = std::chrono::utc_clock::now();
    write_blender_updates_cache_file();
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
      (!updates.latest_lts || (updates.latest_lts->version) < updates.latest->version))
  {
    tmp.append(&(*updates.latest_lts));
  }
  if (U.flag & USER_BLENDER_UPDATE_CURRENT_RELEASE && updates.current_release) {
    tmp.append(&(*updates.current_release));
  }
  std::ranges::sort(
      tmp, [](const VersionUpdate *a, const VersionUpdate *b) { return a->version < b->version; });
  std::ranges::reverse(tmp);
  return tmp;
}

void ignore_update_impl(const VersionUpdate *update)
{
  IgnoredBlenderVersions &ignored_updates = ignored_versions_updates();

  BlenderUpdates &updates = available_blender_updates();

  if (update->version.version == BLENDER_VERSION &&
      update->version.patch > ignored_updates.current_release.patch)
  {
    ignored_updates.current_release = update->version;
  }

  if (update->is_lts && ignored_updates.latest_lts < update->version) {
    ignored_updates.latest_lts = update->version;
  }

  if (ignored_updates.latest < update->version) {
    ignored_updates.latest = update->version;
  }

  if (updates.latest && update == &*updates.latest) {
    updates.latest = std::nullopt;
  }
  if (updates.latest_lts && update == &*updates.latest_lts) {
    updates.latest_lts = std::nullopt;
  }
  if (updates.current_release && update == &*updates.current_release) {
    updates.current_release = std::nullopt;
  }
}

void ignore_update(const VersionUpdate *update)
{
  ignore_update_impl(update);
  write_blender_updates_cache_file();
}

void ignore_all_updates()
{
  BlenderUpdates &updates = available_blender_updates();
  if (updates.latest) {
    ignore_update(&*updates.latest);
  }
  if (updates.latest_lts) {
    ignore_update(&*updates.latest_lts);
  }
  if (updates.current_release) {
    ignore_update(&*updates.current_release);
  }
  write_blender_updates_cache_file();
}

}  // namespace blender::bke
