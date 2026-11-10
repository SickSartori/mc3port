#pragma once
// The launcher's styles: what Original and Enhanced set, and which one the settings are in. Apart from the launcher so
// that a test can check it.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace xport::pc {

struct StyleValue {
    const char* key;
    float original, enhanced;
};

// Enhanced is the defaults of settings.cpp (the test checks it). A style leaves alone what is a matter of the player
// or of the PC, not of the look: language, volumes, window or full screen, speed unit, frame counter, keys.
inline constexpr StyleValue kStyleValues[] = {
    {"frame_rate", 30, 60},
    {"widescreen", 0, 1},
    {"render_scale", 1, 0},
    {"anisotropy", 1, 16},
    {"sharp_2d", 0, 1},
    {"hud_style", 0, 1},
    {"skip_intro", 0, 1},
    {"loading_screen_seconds", 8, 0},
    {"effects", 0, 1},
    {"sharpen", 0.5f, 0.5f},
    {"bloom", 0.35f, 0.35f},
    {"bloom_threshold", 0.7f, 0.7f},
    {"saturation", 1.12f, 1.12f},
    {"contrast", 1.06f, 1.06f},
    {"vignette", 0.15f, 0.15f},
    {"motion_blur", 1, 0},
    {"light_streaks", 1, 0},
    {"ambient_occlusion", 0, 0.6f},
    {"ambient_occlusion_radius", 1, 1},
};

enum { kStyleOriginal, kStyleEnhanced, kStyleCustom };

// `chosen_custom`: the player picked Custom, which stays even when the values happen to match a style.
template <class Get>
int style_of(Get get, bool chosen_custom) {
    if (chosen_custom) return kStyleCustom;
    bool original = true, enhanced = true;
    for (const StyleValue& v : kStyleValues) {
        const float now = get(v.key);
        original = original && std::fabs(now - v.original) < 0.001f;
        enhanced = enhanced && std::fabs(now - v.enhanced) < 0.001f;
    }
    return original ? kStyleOriginal : enhanced ? kStyleEnhanced : kStyleCustom;
}

// The player's own values, kept while a ready-made style is on: ",key=value,key=value,".
template <class Get>
std::string style_snapshot(Get get) {
    std::string out = ",";
    for (const StyleValue& v : kStyleValues) {
        char value[32];
        std::snprintf(value, sizeof(value), "%g", get(v.key));
        out += std::string(v.key) + "=" + value + ",";
    }
    return out;
}

template <class Set>
void style_restore(const std::string& snapshot, Set set) {
    for (const StyleValue& v : kStyleValues) {
        const std::string name = std::string(",") + v.key + "=";
        const size_t at = snapshot.find(name);
        if (at != std::string::npos) set(v.key, static_cast<float>(std::atof(snapshot.c_str() + at + name.size())));
    }
}

}  // namespace xport::pc
