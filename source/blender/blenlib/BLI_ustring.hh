/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <OpenImageIO/ustring.h>

#include "BLI_fixed_string.hh"
#include "BLI_string_ref.hh"

namespace blender {

/**
 * This is a thin wrapper around OpenImageIO's ustring class. Additionally it also provides
 * conversions to our StringRef types.
 *
 * See the OpenImageIO documentation for more details:
 * https://openimageio.readthedocs.io/en/stable/imageioapi.html#efficient-unique-strings-ustring
 */
class UString : public OpenImageIO::ustring {
 public:
  /** Inherit constructors.  */
  using OpenImageIO::ustring::ustring;

  /** Construct from a StringRef.  */
  UString(StringRef str) : OpenImageIO::ustring(std::string_view(str)) {}

  /** Implicit conversion to StringRef. */
  operator StringRef() const
  {
    return StringRef(this->c_str(), this->length());
  }

  /** Implicit conversion to StringRefNull. */
  operator StringRefNull() const
  {
    return StringRefNull(this->c_str(), this->length());
  }
};

/**
 * Create a UString from a string literal. This is a template function so that each string is only
 * made unique once and not every time the literal is used.
 *
 * Note: OpenImageIO defines a similar `_us` string literal operator. However, it newly constructs
 * the ustring in each invocation instead of caching it in a static variable. Caching it like here
 * likely only works in C++20.
 */
template<FixedString FStr> inline UString operator""_ustr()
{
  /* Cache the resulting UString in a static variable so that it only has to be made unique once.
   * A separate static variable is used for each different string because this entire function is
   * templated on the string. */
  static UString ustr(FStr.data);
  return ustr;
}

}  // namespace blender
