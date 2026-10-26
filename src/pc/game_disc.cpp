#include "pc/game_disc.h"

#include <windows.h>
#include <bcrypt.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

#include "pc/log.h"
#include "pc/settings.h"

namespace mc3::pc {
namespace {

// SHA-256 of default.xbe on Midnight Club 3: DUB Edition Remix (Europe) (En,Fr,De,Es,It): the build the port's
// code patches (hle_patches.cpp, dsound_hle.cpp, input_hle.cpp) are made for.
constexpr char kSupportedXbe[] = "29d866afda9c54590a854fdfa98cf6e69d871dd3fc02da20bbbf76c3582c7b09";
constexpr char kSupportedName[] = "Midnight Club 3: DUB Edition Remix, Xbox, Europe (En,Fr,De,Es,It)";

// Dialog texts in English, or in Italian on an Italian Windows.
const char* text(const char* english, const char* italian) {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN ? italian : english;
}

std::wstring wide(const std::string& utf8) {
    std::wstring result(MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0), 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(),
                        static_cast<int>(result.size()));
    return result;
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string sha256(const std::vector<uint8_t>& data) {
    uint8_t digest[32] = {};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<uint8_t*>(data.data()), static_cast<ULONG>(data.size()),
               digest, sizeof(digest));
    std::string hex;
    char byte[3];
    for (uint8_t b : digest) {
        std::snprintf(byte, sizeof(byte), "%02x", b);
        hex += byte;
    }
    return hex;
}

}  // namespace

std::optional<GameDisc> check_game_disc(const std::filesystem::path& path, std::string& problem) {
    std::string error;
    std::unique_ptr<XdvdfsImage> image = XdvdfsImage::open(path, &error);
    const std::optional<XdvdfsImage::Entry> entry = image ? image->find("default.xbe") : std::nullopt;
    if (!entry) {
        log_line("disc: %s is not an Xbox disc image (%s)", utf8(path).c_str(), image ? "no default.xbe" : error.c_str());
        problem = text("This file is not an Xbox disc image.", "Questo file non e' un'immagine di un disco Xbox.");
        return std::nullopt;
    }
    std::vector<uint8_t> file = image->read_all(*entry);
    const std::string hash = sha256(file);
    std::optional<Xbe> xbe = Xbe::parse(std::move(file), &error);
    if (!xbe) {
        log_line("disc: %s: default.xbe: %s", utf8(path).c_str(), error.c_str());
        problem = text("The game on this disc image cannot be read.", "Il gioco in questa immagine non si puo' leggere.");
        return std::nullopt;
    }
    log_line("disc: %s: '%s', title id %08X, default.xbe SHA-256 %s", utf8(path).c_str(), xbe->title().c_str(),
             xbe->title_id(), hash.c_str());
    if (hash != kSupportedXbe) {
        if (GetEnvironmentVariableW(L"MC3_ANY_VERSION", nullptr, 0)) {
            log_line("disc: not the supported version; MC3_ANY_VERSION is set, trying it anyway");
        } else {
            char message[512];
            std::snprintf(message, sizeof(message),
                          text("This disc image holds \"%s\" (title id %08X), not the version mc3port supports:\n%s.",
                               "Questa immagine contiene \"%s\" (title id %08X), non la versione supportata da "
                               "mc3port:\n%s."),
                          xbe->title().c_str(), xbe->title_id(), kSupportedName);
            problem = message;
            return std::nullopt;
        }
    }
    return GameDisc{path, std::move(image), std::move(*xbe)};
}

std::filesystem::path command_line_disc() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::filesystem::path result;
    if (argv && argc > 1) result = argv[1];
    LocalFree(argv);
    return result;
}

namespace {

std::vector<std::filesystem::path> nearby_discs(const std::filesystem::path& exe_directory) {
    std::vector<std::filesystem::path> found;
    std::error_code ignored;
    for (const std::filesystem::path& directory : {exe_directory, std::filesystem::current_path(ignored)}) {
        for (const auto& item : std::filesystem::directory_iterator(directory, ignored)) {
            std::wstring extension = item.path().extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (item.is_regular_file(ignored) && extension == L".iso" &&
                std::find(found.begin(), found.end(), item.path()) == found.end()) {
                found.push_back(item.path());
            }
        }
    }
    return found;
}

}  // namespace

std::filesystem::path ask_for_disc_image(void* owner_window) {
    wchar_t file[MAX_PATH * 4] = {};
    const std::wstring title = wide(text("Choose your Midnight Club 3: DUB Edition Remix disc image (Xbox, Europe)",
                                         "Scegli l'immagine del disco di Midnight Club 3: DUB Edition Remix (Xbox, "
                                         "Europa)"));
    const std::wstring filter = wide(text("Xbox disc image (*.iso)", "Immagine disco Xbox (*.iso)")) +
                                std::wstring(L"\0*.iso\0", 7) + wide(text("All files", "Tutti i file")) +
                                std::wstring(L"\0*.*\0\0", 6);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(owner_window);
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(std::size(file));
    dialog.lpstrTitle = title.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    return GetOpenFileNameW(&dialog) ? std::filesystem::path(file) : std::filesystem::path();
}

std::optional<GameDisc> find_game_disc(const std::filesystem::path& exe_directory, std::string& problem) {
    std::vector<std::filesystem::path> candidates;
    if (const std::filesystem::path argument = command_line_disc(); !argument.empty()) candidates.push_back(argument);
    const std::string remembered = setting_text("disc_image");
    if (!remembered.empty()) {
        candidates.push_back(std::filesystem::path(std::u8string(remembered.begin(), remembered.end())));
    }
    for (const std::filesystem::path& nearby : nearby_discs(exe_directory)) candidates.push_back(nearby);
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code ignored;
        if (!std::filesystem::is_regular_file(candidate, ignored)) {
            log_line("disc: %s not found", utf8(candidate).c_str());
            continue;
        }
        if (std::optional<GameDisc> disc = check_game_disc(candidate, problem)) return disc;
    }
    return std::nullopt;
}

std::optional<GameDisc> open_game_disc(const std::filesystem::path& exe_directory) {
    std::string problem;
    if (std::optional<GameDisc> disc = find_game_disc(exe_directory, problem)) return disc;

    // MC3_NO_DIALOG: automated runs end here instead of waiting for the player.
    const bool dialogs = !GetEnvironmentVariableW(L"MC3_NO_DIALOG", nullptr, 0);
    if (!dialogs) {
        log_line("disc: no usable disc image, and MC3_NO_DIALOG forbids asking");
        return std::nullopt;
    }
    if (!problem.empty()) {
        const std::wstring message = wide(problem + "\n\n" + text("Please choose your disc image.",
                                                                  "Scegli la tua immagine del disco."));
        MessageBoxW(nullptr, message.c_str(), L"mc3port", MB_ICONWARNING | MB_OK);
    }
    for (;;) {
        const std::filesystem::path chosen = ask_for_disc_image(nullptr);
        if (chosen.empty()) {
            log_line("disc: the player closed the file dialog");
            return std::nullopt;
        }
        if (std::optional<GameDisc> disc = check_game_disc(chosen, problem)) {
            setting_save_text("disc_image", utf8(chosen));
            return disc;
        }
        const std::wstring message =
            wide(problem + "\n\n" + text("Choose another file?", "Vuoi scegliere un altro file?"));
        if (MessageBoxW(nullptr, message.c_str(), L"mc3port", MB_ICONWARNING | MB_YESNO) != IDYES) return std::nullopt;
    }
}

}  // namespace mc3::pc
