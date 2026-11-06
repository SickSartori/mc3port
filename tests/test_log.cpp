// The log of this run and of the two before it: older ones go, nothing else moves.

#include "pc/log.h"
#include "test.h"

using namespace mc3::pc;

int main() {
    TemporaryFolder folder("mc3port_test_log");
    const auto log = folder.path / "mc3port.log", one = folder.path / "mc3port.1.log", two = folder.path / "mc3port.2.log";

    rotate_log(log);  // the first start: nothing to move
    CHECK(!std::filesystem::exists(log) && !std::filesystem::exists(one) && !std::filesystem::exists(two));

    write_file(log, std::string("first"));
    rotate_log(log);
    CHECK(!std::filesystem::exists(log));
    CHECK(read_file(one) == "first");
    CHECK(!std::filesystem::exists(two));

    write_file(log, std::string("second"));
    rotate_log(log);
    CHECK(read_file(one) == "second");
    CHECK(read_file(two) == "first");

    write_file(log, std::string("third"));
    rotate_log(log);
    CHECK(read_file(one) == "third");
    CHECK(read_file(two) == "second");  // the first is gone
    CHECK(std::distance(std::filesystem::directory_iterator(folder.path), std::filesystem::directory_iterator()) == 2);

    return test_result();
}
