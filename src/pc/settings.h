#pragma once

// User settings: mc3port.ini next to mc3.exe ("key = value" lines, '#' comments), written with the defaults and an
// explanation of each key when it does not exist. An environment variable MC3_<KEY> (upper case) overrides the file.

#include <filesystem>
#include <string>

namespace mc3::pc {

void settings_load(const std::filesystem::path& file);

// The value of a key listed in settings.cpp (its default when the file and the environment do not set it).
float setting(const char* key);
// Keys the game reads once (language, 16:9, frame rate, resolution...): the value when the game started, so that
// changing them in the in-game menu takes effect only at the next start. settings_freeze marks that moment; before
// it, the current value.
void settings_freeze();
float setting_at_start(const char* key);
// The key's default value.
float setting_default(const char* key);
// Stores a new value into the file (the other lines and their comments stay as they are).
void setting_save(const char* key, float value);

// A text key listed in settings.cpp (UTF-8; empty when not set), and storing a new value into the file (the other
// lines and their comments stay as they are).
std::string setting_text(const char* key);
void setting_save_text(const char* key, const std::string& value);

}  // namespace mc3::pc
