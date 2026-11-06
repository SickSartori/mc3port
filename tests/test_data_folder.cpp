// The move from the old data folder, when the player asks for it: the saves arrive, nothing is overwritten or lost.

#include "pc/paths.h"
#include "test.h"

using namespace mc3::pc;

int main() {
    TemporaryFolder folder("mc3port_test_data_folder");
    const auto from = folder.path / "old", to = folder.path / "new";
    const std::string save = "xbox_hdd/Partition1/UDATA/54540079/DF2A56878DAE/Giocatore 1.mc3Remix";

    // No old folder: nothing to do, nothing made.
    CHECK(move_data_folder(from, to));
    CHECK(!std::filesystem::exists(to));

    // Everything moves, and the empty old folder goes.
    std::filesystem::create_directories((from / save).parent_path());
    std::filesystem::create_directories(from / "logs");
    write_file(from / save, std::string("the red car"));
    write_file(from / "mc3port.ini", std::string("old settings"));
    write_file(from / "logs" / "mc3port.log", std::string("a log"));
    CHECK(move_data_folder(from, to));
    CHECK(read_file(to / save) == "the red car");
    CHECK(read_file(to / "mc3port.ini") == "old settings");
    CHECK(read_file(to / "logs" / "mc3port.log") == "a log");
    CHECK(!std::filesystem::exists(from));

    // An older version ran again afterwards: what exists in the new folder is never overwritten, and the old stays.
    std::filesystem::create_directories((from / save).parent_path());
    write_file(from / save, std::string("an older save"));
    write_file(from / "mc3port.ini", std::string("older settings"));
    write_file(from / "shaders.bin", std::string("new here"));
    CHECK(move_data_folder(from, to));
    CHECK(read_file(to / save) == "the red car");
    CHECK(read_file(to / "mc3port.ini") == "old settings");
    CHECK(read_file(to / "shaders.bin") == "new here");
    CHECK(read_file(from / save) == "an older save");
    CHECK(read_file(from / "mc3port.ini") == "older settings");

    // The destination cannot be made (a file in the way): false, and the saves stay where they are.
    const auto blocked = folder.path / "blocked";
    write_file(blocked, std::string("a file"));
    CHECK(!move_data_folder(from, blocked / "mc3port"));
    CHECK(read_file(from / save) == "an older save");

    return test_result();
}
