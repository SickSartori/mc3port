#pragma once

// The launcher's update check, the parts that need no network (tested in tests/test_update_check.cpp).

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace mc3::pc {

// The numbers in a version text, in order: "v0.3.10" is {0, 3, 10}, compared as a vector.
inline std::vector<int> version_numbers(const std::string& text) {
    std::vector<int> numbers;
    for (size_t i = 0; i < text.size();) {
        if (std::isdigit(static_cast<unsigned char>(text[i]))) {
            size_t end = i;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
            numbers.push_back(std::atoi(text.substr(i, end - i).c_str()));
            i = end;
        } else {
            ++i;
        }
    }
    return numbers;
}

// A link from GitHub's answer is used only if it is the repository's own `https://github.com/<repository>/releases/
// <kind>...` page or download, in plain characters with no "..": a download's last part becomes a file name in
// %TEMP%.
inline bool release_link(const std::string& url, const char* repository, const char* kind) {
    const std::string prefix = std::string("https://github.com/") + repository + "/releases/" + kind;
    if (url.size() <= prefix.size() || url.compare(0, prefix.size(), prefix) != 0) return false;
    if (url.find("..") != std::string::npos || url.back() == '/') return false;
    for (size_t i = prefix.size(); i < url.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(url[i]);
        if (!std::isalnum(c) && !std::strchr("._-/", c)) return false;
    }
    return true;
}

}  // namespace mc3::pc
