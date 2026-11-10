// mc3port.ini: defaults written when missing, values and comments read back, MC3_<KEY> over the file, a saved value
// replacing its line and keeping the others.
#include <windows.h>

#include <cmath>
#include <string>

#include "pc/settings.h"
#include "test.h"

using namespace xport::pc;

namespace {
bool same(float a, float b) { return std::fabs(a - b) < 0.001f; }
}  // namespace

int main() {
    TemporaryFolder folder("mc3port-test-settings");
    const std::filesystem::path file = folder.path / "mc3port.ini";

    // No file: the defaults, and a file holding them.
    settings_load(file);
    CHECK(std::filesystem::exists(file));
    CHECK(same(setting("frame_rate"), setting_default("frame_rate")));

    // Values, with spaces, a trailing comment, a commented-out line and an unknown key.
    write_file(file, std::string("# a comment\n"
                                 "frame_rate = 30   # thirty\n"
                                 "#sharpen = 0.9\n"
                                 "  sharpen=0.25\n"
                                 "no_such_key = 7\n"));
    settings_load(file);
    CHECK(same(setting("frame_rate"), 30));
    CHECK(same(setting("sharpen"), 0.25f));
    CHECK(same(setting("bloom"), setting_default("bloom")));  // not in the file
    CHECK(setting("no_such_key") == 0);

    // The environment wins over the file.
    SetEnvironmentVariableA("MC3_FRAME_RATE", "60");
    settings_load(file);
    CHECK(same(setting("frame_rate"), 60));
    SetEnvironmentVariableA("MC3_FRAME_RATE", nullptr);

    // Saving replaces the key's line only, through a temporary file that does not stay behind.
    setting_save("sharpen", 0.75f);
    const std::string saved = read_file(file);
    CHECK(saved.find("# a comment") != std::string::npos);
    CHECK(saved.find("#sharpen = 0.9") != std::string::npos);
    CHECK(saved.find("  sharpen=0.25") == std::string::npos);
    CHECK(!std::filesystem::exists(file.string() + ".tmp"));
    settings_load(file);
    CHECK(same(setting("sharpen"), 0.75f));
    CHECK(same(setting("frame_rate"), 30));

    return test_result();
}
