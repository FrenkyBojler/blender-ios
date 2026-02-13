/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#pragma once

#include "..\..\..\extern\toml11\toml.hpp"

namespace blender {

void BLI_settings_init();
bool BLI_settings_save();

std::string BLI_settings_get_string(std::string table, std::string key, std::string default_value);
void BLI_settings_set_string(std::string table, std::string key, std::string value);

char BLI_settings_get_char(std::string table, std::string key, char default_value);
void BLI_settings_set_char(std::string table, std::string key, char value);

int32_t BLI_settings_get_int(std::string table, std::string key, int32_t default_value);
void BLI_settings_set_int(std::string table, std::string key, int32_t value);

int64_t BLI_settings_get_int64(std::string table, std::string key, int64_t default_value);
void BLI_settings_set_int64(std::string table, std::string key, int64_t value);

bool BLI_settings_get_bool(std::string table, std::string key, bool default_value);
void BLI_settings_set_bool(std::string table, std::string key, bool value);

float BLI_settings_get_float(std::string table, std::string key, float default_value);
void BLI_settings_set_float(std::string table, std::string key, float value);

std::vector<float> BLI_settings_get_floats(std::string table, std::string key);
void BLI_settings_set_floats(std::string table, std::string key, std::vector<float> values);

/**********************************/

/* Default settings file content. */

const std::string default_settings = R"_delim_(
title = "Settings"
name = "Blender"

["window.dimensions"]
userpref = [100.0, 940.0, 350.0, 900.0]
file = [100.0, 1160.0, 350.0, 950.0]
image = [50.0, 1360.0, 50.0, 830.0]
graph = [50.0, 950.0, 200.0, 780.0]
info = [100.0, 1000.0, 300.0, 880.0]
outliner = [100.0, 550.0, 350.0, 800.0]

)_delim_";

}  // namespace blender
