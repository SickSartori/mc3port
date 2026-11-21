#include "pc/settings.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "pc/log.h"
#include "pc/paths.h"

namespace xport::pc {
namespace {

struct Setting {
    const char* key;
    float value;          // default
    const char* comment;  // written above the key in a new file
};

const Setting kSettings[] = {
    {"game_language", 0, "Game language: 0 = like Windows, 1 = English, 3 = German, 4 = French, 5 = Spanish, 6 = Italian."},
    {"frame_rate", 60, "Game frames per second: 30 like the Xbox, 60 smoother (needs a PC that handles twice the work), 120 for a fast processor and a 120 Hz screen."},
    {"vsync", 0, "Wait for the screen before showing a frame: 1 = the picture never tears (on a slow PC the game may drop to 30 fps), 0 = more frames per second."},
    {"widescreen", 1, "Picture shape: 1 = 16:9, the game's own widescreen mode (wider view, no black bars); 0 = 4:3 like a classic TV; 2 = your screen's shape when it is wider than 16:9 (21:9, 32:9: the view widens to the sides, films and loading screens stay 16:9)."},
    {"field_of_view", 100, "Field of view, in percent of the game's own: 100 = like the Xbox, more = wider (75 to 130 in the launcher)."},
    {"hud_aspect", 1, "In 16:9, keep the HUD's round shapes round (radar, dials): 1 = yes, 0 = stretched as on the Xbox."},
    {"sharp_2d", 1, "The HUD's pictures (radar map, icons, letters) and the cars' stickers enlarged with their edges kept: 1 = sharper at high resolutions, 0 = as the game's."},
    {"sharp_textures", 0, "The textures of the 3D scene (roads, buildings, cars) enlarged two times with their edges kept, as sharp_2d does for the HUD: 1 = sharper from close, 0 = as the game's. Needs more graphics memory; they sharpen a few seconds after a place loads."},
    {"hud_style", 1, "HUD: 0 = the game's, 1 = mc3port's (speedometer, rev counter and gear redrawn, sharp at any resolution)."},
    {"hud_units", 0, "Speed in mc3port's HUD: 0 = mph, 1 = km/h."},
    {"fullscreen", 0, "Start in full screen: 1 = yes, 0 = in a window (Alt+Enter or F11 switches while playing)."},
    {"volume", 100, "Overall volume, 0 to 100."},
    {"launcher_music", 70, "Volume of the launcher's background music, 0 to 100 (0 = off)."},
    {"launcher_mute", 0, "1 = the launcher's background music is off (the speaker button in its bottom bar)."},
    {"fps_counter", 0, "Frames per second shown in a corner while playing: 1 = yes, 0 = no."},
    {"loading_music", 1, "Play optional PS2 loading music from loading_music/1.wav through 18.wav in the game data folder: 1 = on, 0 = off."},
    {"loading_music_mode", 0, "Loading music order: 0 = sequential, 1 = random, 2 = weighted random."},
    {"loading_music_weight_1", 100, "Relative selection weight for PS2 loading track 1: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_2", 100, "Relative selection weight for PS2 loading track 2: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_3", 100, "Relative selection weight for PS2 loading track 3: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_4", 100, "Relative selection weight for PS2 loading track 4: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_5", 100, "Relative selection weight for PS2 loading track 5: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_6", 100, "Relative selection weight for PS2 loading track 6: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_7", 100, "Relative selection weight for PS2 loading track 7: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_8", 100, "Relative selection weight for PS2 loading track 8: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_9", 100, "Relative selection weight for PS2 loading track 9: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_10", 100, "Relative selection weight for PS2 loading track 10: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_11", 100, "Relative selection weight for PS2 loading track 11: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_12", 100, "Relative selection weight for PS2 loading track 12: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_13", 100, "Relative selection weight for PS2 loading track 13: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_14", 100, "Relative selection weight for PS2 loading track 14: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_15", 100, "Relative selection weight for PS2 loading track 15: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_16", 100, "Relative selection weight for PS2 loading track 16: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_17", 100, "Relative selection weight for PS2 loading track 17: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_weight_18", 100, "Relative selection weight for PS2 loading track 18: 0 = disabled, 100 = normal, 300 = most frequent."},
    {"loading_music_volume", 70, "PS2 loading music volume, 0 to 100 (also follows overall volume)."},
    {"loading_music_fade_seconds", 0.75f, "Fade out PS2 loading music over this many seconds near the end of playback."},
    {"loading_screen_seconds", 0, "The game's loading screens stay at least this many seconds, any number; never less than 2 (0 = the shortest)."},
    {"skip_intro", 1, "Skip the films at start (Rockstar logos and the game's intro): 1 = yes, 0 = show them."},
    {"pause_in_background", 1, "Pause the game (in-game menu) when its window goes to the background: 1 = yes, 0 = no."},
    {"check_updates", 1, "Look for a newer mc3port release on GitHub when the launcher opens: 1 = yes, 0 = no."},
    {"render_thread", 1, "Drawing on a thread of its own, beside the one that reads the game's drawing commands: 1 = yes (faster, most of all in split screen; needs four processor threads or more), 0 = as before, if the picture or the game misbehaves."},
    {"gpu_vertices", 1, "Vertex processing on the graphics card: 1 = yes (faster), 0 = on the processor."},
    {"render_scale", 0, "Internal resolution, times the game's 480 lines: 0 = like the screen, 1 = like the Xbox (480p), 1.5 = 720p, 2.25 = 1080p, 3 = 1440p, 4.5 = 2160p (4K), 6 = 2880p (5K). Higher = smoother edges."},
    {"anisotropy", 16, "Anisotropic texture filtering: 1 = off, up to 16."},
    {"effects", 1, "Modern effects: 1 = on, 0 = original picture (F10 toggles them while playing)."},
    {"sharpen", 0.5f, "Adaptive sharpening: 0 = off, 1 = strongest."},
    {"bloom", 0.35f, "Glow around lights (headlights, signs, street lamps): 0 = off, 1 = strong."},
    {"bloom_threshold", 0.7f, "How bright an area must be to glow: 0 = everything, 1 = pure white only."},
    {"saturation", 1.12f, "Colour saturation: 1 = original, higher = more vivid."},
    {"contrast", 1.06f, "Contrast: 1 = original."},
    {"vignette", 0.15f, "Slight darkening of the picture's edges: 0 = off."},
    {"motion_blur", 0, "The game's speed blur: 0 = off, 1 = as strong as on the Xbox."},
    {"light_streaks", 0, "The streaks the game draws from lights at speed and in turns: 0 = off, 1 = as on the Xbox."},
    {"effects_advanced", 0, "Effects style Custom, with every effect's value in the settings: 1 = yes, 0 = Original or Enhanced."},
    {"ambient_occlusion", 0.6f, "Ambient occlusion (darker corners and contact areas): 0 = off, 1 = strong."},
    {"ambient_occlusion_radius", 1, "Ambient occlusion size: 0.5 = tight, 2 = wide."},
};

struct TextSetting {
    const char* key;
    const char* comment;
};

const TextSetting kTextSettings[] = {
    {"disc_image", "Your Midnight Club 3: DUB Edition Remix disc image (.iso or .cci; of an image split in parts, any part). Filled in when you choose it; empty = ask."},
    {"custom_style", "The launcher's Custom style, kept while another style is on; the launcher fills it in."},
    {"keyboard", "Keyboard keys for controller 1 (Windows virtual-key codes); change them in the launcher. Empty = defaults."},
};

std::filesystem::path g_file;
std::map<std::string, float> g_values;
std::map<std::string, float> g_start_values;  // settings_freeze
std::map<std::string, std::string> g_texts;

// Track weights are separate from mc3port.ini, but keep their existing setting keys for the launcher.
bool is_track_weight(const std::string& key) {
    constexpr char prefix[] = "loading_music_weight_";
    if (key.compare(0, sizeof(prefix) - 1, prefix) != 0) return false;
    const std::string number = key.substr(sizeof(prefix) - 1);
    if (number.empty() || number.size() > 2 || number.find_first_not_of("0123456789") != std::string::npos) return false;
    const int index = std::atoi(number.c_str());
    return index >= 1 && index <= 18 && std::to_string(index) == number;
}

std::filesystem::path weights_file() { return data_directory() / "loading_music" / "weights.txt"; }


std::string environment_name(const char* key) {
    std::string name = "MC3_";
    for (const char* c = key; *c; ++c) name += static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    return name;
}

std::string trim(std::string text) {
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t\r") + 1);
    return text;
}

// "key = value": the key, or empty for comments and other lines.
std::string line_key(const std::string& line) {
    if (line.empty() || line[0] == '#') return {};
    const size_t equals = line.find('=');
    return equals == std::string::npos ? std::string() : trim(line.substr(0, equals));
}

void write_defaults(const std::filesystem::path& file) {
    std::ofstream out(file);
    if (!out) return;
    out << "# mc3port settings. Change the values, then restart the game.\n"
           "# Any key can also be given as an environment variable MC3_<KEY> (for example MC3_BLOOM=0).\n\n";
    for (const TextSetting& s : kTextSettings) out << "# " << s.comment << "\n" << s.key << " =\n\n";
    for (const Setting& s : kSettings) {
        if (is_track_weight(s.key)) continue;
        char value[32];
        std::snprintf(value, sizeof(value), "%g", s.value);
        out << "# " << s.comment << "\n" << s.key << " = " << value << "\n\n";
    }
}

// Save the full set of weights so the file is usable even before the user touches a slider.
// A failed write leaves the existing file untouched.
bool write_weights() {
    const auto target = weights_file();
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) { log_line("settings: cannot create loading_music folder: %s", error.message().c_str()); return false; }
    auto temporary = target;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        out << "# PS2 loading music track weights (0 to 300; 0 = disabled, 100 = normal).\n";
        for (int n = 1; n <= 18; ++n) {
            const std::string key = "loading_music_weight_" + std::to_string(n);
            out << key << " = " << g_values[key] << "\n";
        }
        out.close();
        if (!out) { log_line("settings: cannot write %s", temporary.string().c_str()); return false; }
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log_line("settings: cannot replace %s (error %lu)", target.string().c_str(), GetLastError());
        return false;
    }
    return true;
}

}  // namespace

void settings_load(const std::filesystem::path& file) {
    g_file = file;
    for (const Setting& s : kSettings) g_values[s.key] = s.value;
    if (!std::filesystem::exists(file)) {
        write_defaults(file);
        log_line("settings: %s written with the defaults", file.string().c_str());
    }
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const std::string key = line_key(line);
        if (key.empty()) continue;
        const std::string value = trim(line.substr(line.find('=') + 1));
        if (is_track_weight(key)) continue;  // Never load weights from mc3port.ini.
        if (g_values.count(key)) {
            g_values[key] = static_cast<float>(std::atof(value.c_str()));  // stops at a trailing # comment
        } else {
            for (const TextSetting& s : kTextSettings) {
                if (key == s.key) g_texts[key] = value;  // whole line: paths may contain '#'
            }
        }
    }
    in.close();
    // Track weights are stored only in loading_music/weights.txt.
    const auto weights = weights_file();
    std::ifstream weight_input(weights);
    if (weight_input) {
        for (std::string entry; std::getline(weight_input, entry);) {
            const std::string key = line_key(entry);
            if (is_track_weight(key))
                g_values[key] = static_cast<float>(std::atof(trim(entry.substr(entry.find('=') + 1)).c_str()));
        }
        if (weight_input.bad()) log_line("settings: error reading %s", weights.string().c_str());
    } else {
        std::error_code error;
        if (!std::filesystem::exists(weights, error) && !error) write_weights();
    }

    // Before 0.2.5 render_scale was a whole 1 to 4: 2 and 4 become the nearest named resolution (1080p, 2160p).
    if (const float old_scale = g_values["render_scale"]; old_scale == 2 || old_scale == 4) {
        setting_save("render_scale", old_scale == 2 ? 2.25f : 4.5f);
        log_line("settings: render_scale %g is now %g", old_scale, g_values["render_scale"]);
    }
    // Before 0.2.5 the speed blur and the light streaks were on at 0.5 by default: an untouched 0.5 becomes off.
    if (g_values["motion_blur"] == 0.5f) {
        setting_save("motion_blur", 0);
        log_line("settings: motion_blur 0.5 (the old default) is now 0");
    }
    for (auto& [key, value] : g_values) {
        char text[32] = {};
        if (GetEnvironmentVariableA(environment_name(key.c_str()).c_str(), text, sizeof(text))) {
            value = static_cast<float>(std::atof(text));
        }
        log_line("settings: %s = %g", key.c_str(), value);
    }
}

float setting(const char* key) {
    const auto found = g_values.find(key);
    if (found != g_values.end()) return found->second;
    for (const Setting& s : kSettings) {
        if (std::strcmp(s.key, key) == 0) return s.value;
    }
    return 0;
}

void settings_freeze() { g_start_values = g_values; }

float setting_at_start(const char* key) {
    const auto found = g_start_values.find(key);
    return found != g_start_values.end() ? found->second : setting(key);
}

std::string setting_text(const char* key) {
    const auto found = g_texts.find(key);
    return found != g_texts.end() ? found->second : std::string();
}

namespace {

// Replaces the key's line in the file, or appends it with the key's comment.
void save_line(const char* key, const std::string& value, const char* comment) {
    std::vector<std::string> lines;
    {
        std::ifstream in(g_file);
        for (std::string line; std::getline(in, line);) lines.push_back(line);
    }
    bool replaced = false;
    for (std::string& line : lines) {
        if (line_key(line) == key) {
            line = std::string(key) + " = " + value;
            replaced = true;
        }
    }
    if (!replaced) {
        if (comment) lines.insert(lines.end(), {"", std::string("# ") + comment});
        lines.push_back(std::string(key) + " = " + value);
    }
    // Written beside the file and renamed over it: a crash in between leaves the old file, not an empty one.
    const std::filesystem::path temporary = g_file.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        for (const std::string& line : lines) out << line << "\n";
        if (!out) {
            log_line("settings: could not write %s", temporary.string().c_str());
            return;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, g_file, error);
    if (error) log_line("settings: could not replace %s: %s", g_file.string().c_str(), error.message().c_str());
}

}  // namespace

void setting_save_text(const char* key, const std::string& value) {
    g_texts[key] = value;
    const char* comment = nullptr;
    for (const TextSetting& s : kTextSettings) {
        if (std::strcmp(s.key, key) == 0) comment = s.comment;
    }
    save_line(key, value, comment);
}

float setting_default(const char* key) {
    for (const Setting& s : kSettings) {
        if (std::strcmp(s.key, key) == 0) return s.value;
    }
    return 0;
}

void setting_save(const char* key, float value) {
    g_values[key] = value;
    if (is_track_weight(key)) {
        write_weights();
        return;
    }
    const char* comment = nullptr;
    for (const Setting& s : kSettings) {
        if (std::strcmp(s.key, key) == 0) comment = s.comment;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%g", value);
    save_line(key, text, comment);
}

}  // namespace xport::pc
