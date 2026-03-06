/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_utils.hh"

#include "BLI_array.hh"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/unicodeUtils.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

namespace blender::io::usd {

static bool is_safe_char(const pxr::TfUtf8CodePoint cp,
                         bool is_first,
                         bool allow_colon,
                         bool allow_unicode)
{
  constexpr pxr::TfUtf8CodePoint cp_underscore = pxr::TfUtf8CodePointFromAscii('_');
  constexpr pxr::TfUtf8CodePoint cp_colon = pxr::TfUtf8CodePointFromAscii(':');

  if (cp == cp_underscore) {
    return true;
  }

  if (allow_colon && cp == cp_colon) {
    return true;
  }

  if (allow_unicode) {
    return is_first ? pxr::TfIsUtf8CodePointXidStart(cp) : pxr::TfIsUtf8CodePointXidContinue(cp);
  }

  constexpr uint32_t cp_A = pxr::TfUtf8CodePointFromAscii('A').AsUInt32();
  constexpr uint32_t cp_Z = pxr::TfUtf8CodePointFromAscii('Z').AsUInt32();
  constexpr uint32_t cp_a = pxr::TfUtf8CodePointFromAscii('a').AsUInt32();
  constexpr uint32_t cp_z = pxr::TfUtf8CodePointFromAscii('z').AsUInt32();
  constexpr uint32_t cp_0 = pxr::TfUtf8CodePointFromAscii('0').AsUInt32();
  constexpr uint32_t cp_9 = pxr::TfUtf8CodePointFromAscii('9').AsUInt32();

  const uint32_t cp_u32 = cp.AsUInt32();
  const bool is_letter = (cp_u32 >= cp_A && cp_u32 <= cp_Z) || (cp_u32 >= cp_a && cp_u32 <= cp_z);
  const bool is_digit = cp_u32 >= cp_0 && cp_u32 <= cp_9;
  return is_first ? is_letter : (is_letter || is_digit);
}

static std::string make_safe_name(const StringRef name, bool allow_colon, bool allow_unicode)
{
  if (name.is_empty()) {
    return "_";
  }

  const bool has_leading_digit = std::isdigit(name[0]);
  const bool has_leading_colon = allow_colon && name.front() == ':';
  const bool has_trailing_colon = allow_colon && name.back() == ':';
  const bool need_leading_underscore = has_leading_digit || has_leading_colon;

  /* Create temporary buffer using the original incoming string size, which can be larger than
   * required if unicode characters are converted to '_'. This size serves as the upper limit of
   * what might be produced. */
  int64_t adjust = (need_leading_underscore ? 1 : 0) + (has_trailing_colon ? 1 : 0);
  Array<char, 256> storage(name.size() + adjust);
  MutableSpan<char> buf(storage);

  /* Insert a leading '_' to account for invalid starting characters. */
  size_t offset = 0;
  bool first = true;
  if (need_leading_underscore) {
    buf[0] = '_';
    offset = 1;
    first = false;
  }

  for (auto cp : pxr::TfUtf8CodePointView{name}) {
    const bool cp_allowed = is_safe_char(cp, first, allow_colon, allow_unicode);
    if (!cp_allowed) {
      offset += BLI_str_utf8_from_unicode(uint32_t('_'), buf.data() + offset, buf.size() - offset);
    }
    else {
      offset += BLI_str_utf8_from_unicode(cp.AsUInt32(), buf.data() + offset, buf.size() - offset);
    }

    first = false;
  }

  if (allow_colon && buf[offset - 1] == ':') {
    buf[offset] = '_';
    offset++;
  }

  return {buf.data(), offset};
}

std::string make_safe_name(const StringRef name, bool allow_unicode)
{
  return make_safe_name(name, false, allow_unicode);
}

std::string make_safe_primvar_name(const StringRef name, bool allow_unicode)
{
  /* In order to allow nested primvar names like "primvars:ns:name", we allow the use of ':'. */
  return make_safe_name(name, true, allow_unicode);
}

pxr::SdfPath get_unique_path(pxr::UsdStageRefPtr stage, const std::string &path)
{
  std::string unique_path = path;
  int suffix = 2;
  while (stage->GetPrimAtPath(pxr::SdfPath(unique_path)).IsValid()) {
    unique_path = path + std::to_string(suffix++);
  }

  return pxr::SdfPath(unique_path);
}

}  // namespace blender::io::usd
