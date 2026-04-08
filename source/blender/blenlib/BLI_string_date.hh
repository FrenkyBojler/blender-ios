/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * This file...
 */

#include <chrono>
#include <string>

#include "BLI_string_ref.hh"

namespace blender {

std::string BLI_date_format_date(const std::tm *date_time, const char *locale_iso = nullptr);

std::string BLI_date_format_time(const std::tm *date_time, const char *locale_iso = nullptr);

std::string BLI_date_format_datetime(const std::tm *date_time,
                                     const char *locale_iso = nullptr,
                                     const std::tm *now = nullptr,
                                     const StringRef &today = {},
                                     const StringRef &yesterday = {});

}  // namespace blender
