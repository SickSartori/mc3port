// Which disc images are accepted. "real": the supported game itself, through MC3_TEST_DISC (a disc image of one's own;
// nothing of it stays): the known file, one that differs only in its header, one that differs in its code.

#include <windows.h>

#include <cstring>

#include "disc_builder.h"
#include "pc/game_disc.h"
#include "pc/log.h"
#include "pc/settings.h"
#include "test.h"

// game_disc.cpp's only ties to the rest of the program.
namespace mc3::pc {
void log_line(const char*, ...) {}
std::string setting_text(const char*) { return {}; }
void setting_save_text(const char*, const std::string&) {}
}  // namespace mc3::pc

using namespace mc3;
using namespace mc3::pc;

namespace {

constexpr int kSkipped = 77;

std::vector<DiscNode> disc_with(std::vector<uint8_t> xbe) { return {{"data.bin", {1}}, {"default.xbe", std::move(xbe)}}; }

void test_refused(const std::filesystem::path& folder) {
    std::string problem;
    write_file(folder / "text.iso", std::string(200000, 'x'));
    CHECK(!check_game_disc(folder / "text.iso", problem) && !problem.empty());

    problem.clear();
    write_file(folder / "no_xbe.iso", make_xiso({{"other.bin", {1}}}));
    CHECK(!check_game_disc(folder / "no_xbe.iso", problem) && !problem.empty());

    problem.clear();
    write_file(folder / "broken.iso", make_xiso(disc_with(std::vector<uint8_t>(1000, 'x'))));
    CHECK(!check_game_disc(folder / "broken.iso", problem) && !problem.empty());

    problem.clear();
    write_file(folder / "other.iso", make_xiso(disc_with(make_xbe("Another Game"))));
    CHECK(!check_game_disc(folder / "other.iso", problem));
    CHECK(problem.find("Another Game") != std::string::npos);  // the player is told what the image holds
    CHECK(problem.find("54540079") != std::string::npos);

    SetEnvironmentVariableW(L"MC3_ANY_VERSION", L"1");
    problem.clear();
    auto any = check_game_disc(folder / "other.iso", problem);
    CHECK(any.has_value() && any->xbe.title() == "Another Game" && problem.empty());
    SetEnvironmentVariableW(L"MC3_ANY_VERSION", nullptr);
}

int test_real(const std::filesystem::path& folder) {
    wchar_t path[1024] = {};
    if (!GetEnvironmentVariableW(L"MC3_TEST_DISC", path, 1024)) {
        std::printf("no MC3_TEST_DISC: skipped\n");
        return kSkipped;
    }
    std::string problem;
    auto original = check_game_disc(path, problem);
    CHECK(original.has_value());
    if (!original) return test_result();
    const std::vector<uint8_t>& xbe = original->xbe.file();

    const auto accepted = [&](const char* name, std::vector<uint8_t> file) {
        const std::filesystem::path image = folder / name;
        write_file(image, make_xiso(disc_with(std::move(file))));
        problem.clear();
        const bool result = check_game_disc(image, problem).has_value();
        std::filesystem::remove(image);
        return result;
    };
    CHECK(accepted("same.iso", xbe));

    std::vector<uint8_t> changed = xbe;
    const uint32_t certificate = load_le32(xbe.data() + 0x118) - original->xbe.base_address();
    changed[certificate + 0xA0] ^= 1;  // the game region: a region-free dump
    CHECK(accepted("header.iso", changed));

    changed = xbe;
    changed[original->xbe.sections()[0].raw_address + 0x100] ^= 1;
    CHECK(!accepted("code.iso", changed));
    CHECK(!problem.empty());
    return test_result();
}

}  // namespace

int main(int argc, char** argv) {
    TemporaryFolder folder("mc3port-test-disc");
    if (argc > 1 && std::strcmp(argv[1], "real") == 0) return test_real(folder.path);
    test_refused(folder.path);
    return test_result();
}
