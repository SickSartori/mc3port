#pragma once

// A crash leaves mc3port-crash.dmp next to the log, and the next start would overwrite the log. So the first start after
// a crash puts both in mc3port-crash-report.zip, ready to attach to a report.

#include <filesystem>
#include <string>

namespace mc3::pc {

std::filesystem::path crash_report_path();
bool make_crash_report();   // before the log is opened: true if a report was made
bool crash_report_ready();  // one was made at this start: the launcher offers it
// The crash in a few lines (the exception, the address read or written, the return addresses), for the issue form:
// from the log of the crashed run, kept by make_crash_report. Empty when there was none.
std::string crash_summary();
std::string crash_summary_of(const std::string& log);

}  // namespace mc3::pc
