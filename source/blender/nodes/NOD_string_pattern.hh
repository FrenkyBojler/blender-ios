/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <variant>

#include "BLI_string_ref.hh"

#include "RNA_types.hh"

namespace blender::nodes {

enum class StringPatternMode {
  /** Only exact matches are allowed. */
  Exact = 0,
  /** A single wildcard (*) is allowed. */
  Wildcard = 1,
};

class StringPattern {
 private:
  struct Exact {
    StringRef pattern;

    bool match(const StringRef query) const
    {
      return pattern == query;
    }
  };

  struct Wildcard {
    StringRef prefix;
    StringRef suffix;

    bool match(const StringRef query) const
    {
      return query.startswith(prefix) && query.endswith(suffix);
    }
  };

  using PatternVariant = std::variant<Exact, Wildcard>;
  PatternVariant variant_;

  StringPattern(PatternVariant variant) : variant_(std::move(variant)) {}

 public:
  static std::optional<StringPattern> from_string(StringPatternMode mode,
                                                  StringRef pattern,
                                                  std::string &r_error);

  bool match(StringRef query) const;

  std::optional<StringRef> exact_pattern() const
  {
    if (const Exact *exact = std::get_if<Exact>(&variant_)) {
      return exact->pattern;
    }
    return std::nullopt;
  }
};

extern const EnumPropertyItem string_pattern_mode_items[];

}  // namespace blender::nodes
