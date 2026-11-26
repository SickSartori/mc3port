#pragma once

#include <filesystem>

namespace xport {
class XdvdfsImage;
}

namespace xport::pc {

// game_language 7: Português (Brasil), João13 e cia's translation (CC BY-NC 4.0) over the game's English.
constexpr int kLanguagePortuguese = 7;

// Before the game starts: with game_language 7, makes the translated files from the player's disc the first time (from
// translations\pt-br.mc3patch beside mc3port), and has the game run in English, which they replace.
void translation_prepare(const xport::XdvdfsImage& disc);
// Where the translated files are, read instead of the disc's (kernel_io.cpp); empty when no translation is on.
const std::filesystem::path& translation_folder();

}  // namespace xport::pc
