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
 * The following documentation is a verbatim copy of the OpenImageIO documentation in ustring.h.
 *
 * A ustring is an alternative to char* or std::string for storing
 * strings, in which the character sequence is unique (allowing many
 * speed advantages for assignment, equality testing, and inequality
 * testing).
 *
 * The implementation is that behind the scenes there is a hash set of
 * allocated strings, so the characters of each string are unique.  A
 * ustring itself is a pointer to the characters of one of these canonical
 * strings.  Therefore, assignment and equality testing is just a single
 * 32- or 64-bit int operation, the only mutex is when a ustring is
 * created from raw characters, and the only malloc is the first time
 * each canonical ustring is created.
 *
 * The internal table also contains a std::string version and the length
 * of the string, so converting a ustring to a std::string (via
 * ustring::string()) or querying the number of characters (via
 * ustring::size() or ustring::length()) is extremely inexpensive, and does
 * not involve creation/allocation of a new std::string or a call to
 * strlen.
 *
 * We try very hard to completely mimic the API of std::string,
 * including all the constructors, comparisons, iterations, etc.  Of
 * course, the characters of a ustring are non-modifiable, so we do not
 * replicate any of the non-const methods of std::string.  But in most
 * other ways it looks and acts like a std::string and so most templated
 * algorithms that would work on a "const std::string &" will also work
 * on a ustring.
 *
 * Note that like a `char*`, but unlike a `std::string`, a ustring is not
 * allowed to contain any embedded NUL ('\0') characters. When constructing
 * ustrings from a std::string or a string_view, the contents will be
 * truncated at the point of any NUL character. This is done to ensure that
 * ustring::c_str() refers to the same C-style character sequence as the
 * ustring itself or ustring::string().
 *
 * Usage guidelines:
 *
 * Compared to standard strings, ustrings have several advantages:
 *
 *   - Each individual ustring is very small -- in fact, we guarantee that
 *     a ustring is the same size and memory layout as an ordinary char*.
 *   - Storage is frugal, since there is only one allocated copy of each
 *     unique character sequence, throughout the lifetime of the program.
 *   - Assignment from one ustring to another is just copy of the pointer;
 *     no allocation, no character copying, no reference counting.
 *   - Equality testing (do the strings contain the same characters) is
 *     a single operation, the comparison of the pointer.
 *   - Memory allocation only occurs when a new ustring is constructed from
 *     raw characters the FIRST time -- subsequent constructions of the
 *     same string just finds it in the canonical string set, but doesn't
 *     need to allocate new storage.  Destruction of a ustring is trivial,
 *     there is no de-allocation because the canonical version stays in
 *     the set.  Also, therefore, no user code mistake can lead to
 *     memory leaks.
 *
 * But there are some problems, too.  Canonical strings are never freed
 * from the table.  So in some sense all the strings "leak", but they
 * only leak one copy for each unique string that the program ever comes
 * across.  Also, creation of unique strings from raw characters is more
 * expensive than for standard strings, due to hashing, table queries,
 * and other overhead.
 *
 * On the whole, ustrings are a really great string representation
 *   - if you tend to have (relatively) few unique strings, but many
 *     copies of those strings;
 *   - if the creation of strings from raw characters is relatively
 *     rare compared to copying or comparing to existing strings;
 *   - if you tend to make the same strings over and over again, and
 *     if it's relatively rare that a single unique character sequence
 *     is used only once in the entire lifetime of the program;
 *   - if your most common string operations are assignment and equality
 *     testing and you want them to be as fast as possible;
 *   - if you are doing relatively little character-by-character assembly
 *     of strings, string concatenation, or other "string manipulation"
 *     (other than equality testing).
 *
 * ustrings are not so hot
 *   - if your program tends to have very few copies of each character
 *     sequence over the entire lifetime of the program;
 *   - if your program tends to generate a huge variety of unique
 *     strings over its lifetime, each of which is used only a short
 *     time and then discarded, never to be needed again;
 *   - if you don't need to do a lot of string assignment or equality
 *     testing, but lots of more complex string manipulation.
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
