// The crash report: when it is made, and that a tool other than ours opens the zip.

#include <chrono>
#include <cstdlib>
#include <iterator>

#include "pc/crash_report.h"
#include "pc/paths.h"
#include "test.h"

namespace {
std::filesystem::path g_folder;
}

// crash_report.cpp's only tie to the rest of the program.
namespace mc3::pc {
std::filesystem::path data_directory() { return g_folder; }
}  // namespace mc3::pc

using namespace mc3::pc;

int main() {
    TemporaryFolder folder("mc3port-test-crash");
    g_folder = folder.path;
    const auto logs = log_directory();
    std::filesystem::create_directories(logs);
    const auto log = logs / "mc3port.log", dump = logs / "mc3port-crash.dmp";
    const std::string log_text = "a log\nof two lines\n";
    std::string dump_text(70000, '\0');
    for (size_t i = 0; i < dump_text.size(); ++i) dump_text[i] = static_cast<char>(i * 31);

    // Nothing to report.
    CHECK(!make_crash_report());
    CHECK(!crash_report_ready());
    write_file(log, log_text);
    CHECK(!make_crash_report());
    std::filesystem::remove(log);
    write_file(dump, dump_text);
    CHECK(!make_crash_report());

    // A dump left long before this log is not this crash's: it is left alone.
    write_file(log, log_text);
    std::filesystem::last_write_time(dump, std::filesystem::last_write_time(log) - std::chrono::minutes(11));
    CHECK(!make_crash_report());
    CHECK(!crash_report_ready());
    CHECK(std::filesystem::exists(dump));
    CHECK(!std::filesystem::exists(crash_report_path()));

    // A real one.
    std::filesystem::last_write_time(dump, std::filesystem::last_write_time(log) - std::chrono::minutes(9));
    CHECK(make_crash_report());
    CHECK(crash_report_ready());
    CHECK(std::filesystem::exists(crash_report_path()));
    CHECK(!std::filesystem::exists(dump));  // or the next start would make it again
    CHECK(!std::filesystem::exists(logs / "mc3port-crash-report.zip.tmp"));
    CHECK(read_file(log) == log_text);

    // tar comes with Windows and reads zip; a wrong size or CRC makes it fail.
    const auto out = folder.path / "out";
    std::filesystem::create_directories(out);
    const std::string command = "tar -xf \"" + crash_report_path().string() + "\" -C \"" + out.string() + "\"";
    CHECK(std::system(command.c_str()) == 0);
    CHECK(read_file(out / "mc3port.log") == log_text);
    CHECK(read_file(out / "mc3port-crash.dmp") == dump_text);

    // The zip's own structure: the local entry first, the end record last, with both entries counted.
    const std::string zip = read_file(crash_report_path());
    CHECK(zip.compare(0, 4, "PK\x03\x04") == 0);
    CHECK(zip.size() > 22 && zip.compare(zip.size() - 22, 4, "PK\x05\x06") == 0);
    CHECK(zip.size() > 22 && zip[zip.size() - 22 + 10] == 2);

    // The next start, with no new crash, makes nothing and keeps the report there.
    CHECK(!make_crash_report());
    CHECK(std::filesystem::exists(crash_report_path()));

    // The summary for the issue form: the first exception's line, its address and return addresses, nothing else.
    CHECK(crash_summary().empty());  // the log above had no exception
    const std::string crashed =
        "audio: fine\r\nfirst-chance exception:\r\n\r\nunhandled exception C0000005 at 0042D3B6 (in the game's code)\r\n"
        "  write of address 0AF6F000\r\n  memory there: region 0AF6F000+11000 (reserved)\r\n  eax=1 ebx=2\r\n"
        "  return addresses on the stack: 00489C88 004390C8\r\nfirst-chance exception:\r\n\r\n"
        "unhandled exception C0000005 at 00212316 (in the game's code)\r\n";
    CHECK(crash_summary_of(crashed) ==
          "unhandled exception C0000005 at 0042D3B6 (in the game's code)\n  write of address 0AF6F000\n"
          "  return addresses on the stack: 00489C88 004390C8\n");
    CHECK(crash_summary_of("no crash\n").empty());
    return test_result();
}
