#pragma once

// User settings: mc3port.ini next to mc3.exe ("key = value" lines, '#' comments), written with the defaults and an
// explanation of each key when it does not exist. An environment variable MC3_<KEY> (upper case) overrides the file.

#include <filesystem>

namespace mc3::pc {

void settings_load(const std::filesystem::path& file);

// The value of a key listed in settings.cpp (its default when the file and the environment do not set it).
float setting(const char* key);

}  // namespace mc3::pc
