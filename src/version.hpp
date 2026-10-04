#pragma once

#include <string>
#include <vector>
#include <sstream>

#define VIRTUALCAM_VERSION "2.3.0"
#define VIRTUALCAM_GITHUB_OWNER "dimalinau-lab"
#define VIRTUALCAM_GITHUB_REPO "Virtual-Camera"

namespace VersionHelper {

inline std::vector<int> parseSemver(const std::string& v) {
    std::vector<int> parts;
    std::string s = v;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) {
        s = s.substr(1);
    }
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, '.')) {
        try {
            parts.push_back(std::stoi(item));
        } catch (...) {
            parts.push_back(0);
        }
    }
    while (parts.size() < 3) parts.push_back(0);
    return parts;
}

// Возвращает true, если candidate строго новее current
inline bool isNewer(const std::string& current, const std::string& candidate) {
    auto cur = parseSemver(current);
    auto cand = parseSemver(candidate);
    for (size_t i = 0; i < 3; ++i) {
        if (cand[i] > cur[i]) return true;
        if (cand[i] < cur[i]) return false;
    }
    return false;
}

} // namespace VersionHelper
