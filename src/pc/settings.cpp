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

namespace mc3::pc {
namespace {

struct Setting {
    const char* key;
    float value;          // default
    const char* comment;  // written above the key in a new file
};

const Setting kSettings[] = {
    {"frame_rate", 30, "Game frames per second: 30 like the Xbox, 60 smoother (needs a PC that handles twice the work)."},
    {"widescreen", 1, "16:9 widescreen: 1 = the game's own widescreen mode (wider view, no black bars), 0 = 4:3 like a classic TV."},
    {"gpu_vertices", 1, "Vertex processing on the graphics card: 1 = yes (faster), 0 = on the processor."},
    {"render_scale", 2, "Internal resolution: 1 = like the Xbox (640x480), 2 = 1280x960, 3, 4. Higher = smoother edges."},
    {"anisotropy", 16, "Anisotropic texture filtering: 1 = off, up to 16."},
    {"effects", 1, "Modern effects: 1 = on, 0 = original picture (F10 toggles them while playing)."},
    {"sharpen", 0.5f, "Adaptive sharpening: 0 = off, 1 = strongest."},
    {"bloom", 0.35f, "Glow around lights (headlights, signs, street lamps): 0 = off, 1 = strong."},
    {"bloom_threshold", 0.7f, "How bright an area must be to glow: 0 = everything, 1 = pure white only."},
    {"saturation", 1.12f, "Colour saturation: 1 = original, higher = more vivid."},
    {"contrast", 1.06f, "Contrast: 1 = original."},
    {"vignette", 0.15f, "Slight darkening of the picture's edges: 0 = off."},
    {"ambient_occlusion", 0.6f, "Ambient occlusion (darker corners and contact areas): 0 = off, 1 = strong."},
    {"ambient_occlusion_radius", 1, "Ambient occlusion size: 0.5 = tight, 2 = wide."},
};

struct TextSetting {
    const char* key;
    const char* comment;
};

const TextSetting kTextSettings[] = {
    {"disc_image", "Your Midnight Club 3: DUB Edition Remix disc image (.iso). Filled in when you choose it; empty = ask."},
};

std::filesystem::path g_file;
std::map<std::string, float> g_values;
std::map<std::string, std::string> g_texts;

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
        char value[32];
        std::snprintf(value, sizeof(value), "%g", s.value);
        out << "# " << s.comment << "\n" << s.key << " = " << value << "\n\n";
    }
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
        if (g_values.count(key)) {
            g_values[key] = static_cast<float>(std::atof(value.c_str()));  // stops at a trailing # comment
        } else {
            for (const TextSetting& s : kTextSettings) {
                if (key == s.key) g_texts[key] = value;  // whole line: paths may contain '#'
            }
        }
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

std::string setting_text(const char* key) {
    const auto found = g_texts.find(key);
    return found != g_texts.end() ? found->second : std::string();
}

void setting_save_text(const char* key, const std::string& value) {
    g_texts[key] = value;
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
        for (const TextSetting& s : kTextSettings) {
            if (std::strcmp(s.key, key) == 0) lines.insert(lines.end(), {"", std::string("# ") + s.comment});
        }
        lines.push_back(std::string(key) + " = " + value);
    }
    std::ofstream out(g_file, std::ios::trunc);
    for (const std::string& line : lines) out << line << "\n";
    if (!out) log_line("settings: could not write %s", g_file.string().c_str());
}

}  // namespace mc3::pc
