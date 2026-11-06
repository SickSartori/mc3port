// The launcher's styles: which one the settings are in, and the player's own values coming back.
#include <map>
#include <string>
#include "pc/launcher_style.h"
#include "pc/settings.h"
#include "test.h"

using namespace mc3::pc;

int main() {
    std::map<std::string, float> values;
    const auto get = [&](const char* key) { return values[key]; };
    const auto set = [&](const char* key, float value) { values[key] = value; };

    // A new installation starts in Enhanced.
    for (const StyleValue& v : kStyleValues) {
        CHECK(std::fabs(setting_default(v.key) - v.enhanced) < 0.001f);
    }

    for (const StyleValue& v : kStyleValues) values[v.key] = v.original;
    CHECK(style_of(get, false) == kStyleOriginal);
    CHECK(style_of(get, true) == kStyleCustom);
    for (const StyleValue& v : kStyleValues) values[v.key] = v.enhanced;
    CHECK(style_of(get, false) == kStyleEnhanced);

    // One value changed: custom. Its snapshot brings it back after a ready-made style.
    values["sharpen"] = 0.25f;
    values["loading_screen_seconds"] = 4;
    CHECK(style_of(get, false) == kStyleCustom);
    const std::string mine = style_snapshot(get);
    for (const StyleValue& v : kStyleValues) values[v.key] = v.original;
    style_restore(mine, set);
    CHECK(values["sharpen"] == 0.25f);
    CHECK(values["loading_screen_seconds"] == 4);
    CHECK(values["frame_rate"] == 60);

    // Nothing saved yet: nothing changes.
    style_restore("", set);
    CHECK(values["sharpen"] == 0.25f);
    return test_result();
}
