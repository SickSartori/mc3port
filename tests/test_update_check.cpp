// The launcher's update check without the network: version order and which links from GitHub's answer are used.
#include <string>

#include "pc/update_check.h"
#include "test.h"

using namespace mc3::pc;

int main() {
    // Versions compare number by number, with or without the tag's "v".
    CHECK(version_numbers("v0.3.10") > version_numbers("0.3.9"));
    CHECK(version_numbers("v0.3.5") == version_numbers("0.3.5"));
    CHECK(version_numbers("0.4.0") > version_numbers("0.3.99"));
    CHECK(!(version_numbers("0.3.5") > version_numbers("0.3.5")));
    CHECK(version_numbers("").empty());

    const char* repository = "SickSartori/mc3port";
    const std::string downloads = "https://github.com/SickSartori/mc3port/releases/download/";
    CHECK(release_link(downloads + "v0.3.5/mc3port-0.3.5-windows-x86-setup.exe", repository, "download/"));
    CHECK(release_link(downloads + "v0.3.5/SHA256SUMS", repository, "download/"));
    CHECK(release_link("https://github.com/SickSartori/mc3port/releases/tag/v0.3.5", repository, "tag/"));

    // Anything else is refused: another repository or host, another kind of page, a way out of the folder or a
    // name that would not be a plain file in %TEMP%.
    CHECK(!release_link("https://github.com/someone/mc3port/releases/download/v0.3.5/x.exe", repository, "download/"));
    CHECK(!release_link("https://example.com/SickSartori/mc3port/releases/download/v1/x.exe", repository, "download/"));
    CHECK(!release_link("http://github.com/SickSartori/mc3port/releases/download/v1/x.exe", repository, "download/"));
    CHECK(!release_link(downloads + "v0.3.5/x.exe", repository, "tag/"));
    CHECK(!release_link(downloads + "v0.3.5/../../../evil/x.exe", repository, "download/"));
    CHECK(!release_link(downloads + "v0.3.5/a\\..\\b.exe", repository, "download/"));
    CHECK(!release_link(downloads + "v0.3.5/x.exe?y=1", repository, "download/"));
    CHECK(!release_link(downloads + "v0.3.5/", repository, "download/"));
    CHECK(!release_link(downloads, repository, "download/"));

    return test_result();
}
