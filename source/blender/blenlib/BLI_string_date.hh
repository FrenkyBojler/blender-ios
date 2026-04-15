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

#include <ctime>
#include <string>

#include "BLI_string_ref.hh"

namespace blender::date_format {

std::string date(const std::tm *date_time, const StringRef locale_iso = {});

std::string time(const std::tm *date_time, const StringRef locale_iso = {});

std::string datetime(const std::tm *date_time,
                     const StringRef locale_iso = {},
                     const std::tm *now = nullptr,
                     const StringRef today = {},
                     const StringRef yesterday = {});

}  // namespace blender::date_format
