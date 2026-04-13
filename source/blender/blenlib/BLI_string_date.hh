/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * This allows converting std::tm datetime structures to localized date and
 * time strings. The localization is based on the CLDR from the current locale.
 */

// #include <chrono>
// #include <iomanip>
#include <string>

#include "BLI_string_ref.hh"

namespace blender::date_format {

std::string date(const std::tm *date_time, const char *locale_iso = nullptr);

std::string time(const std::tm *date_time, const char *locale_iso = nullptr);

std::string datetime(const std::tm *date_time,
                     const char *locale_iso = nullptr,
                     const std::tm *now = nullptr,
                     const StringRef &today = {},
                     const StringRef &yesterday = {});

}  // namespace blender::date_format
