#include "moha/tool/util/glob.hpp"

namespace moha::tools::util {

bool glob_match(std::string_view pattern, std::string_view name) noexcept {
    size_t pi = 0, ni = 0, star = std::string_view::npos, match = 0;
#ifdef _WIN32
    auto eq = [](char a, char b) {
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        return a == b;
    };
#else
    auto eq = [](char a, char b) { return a == b; };
#endif
    while (ni < name.size()) {
        if (pi < pattern.size() && pattern[pi] == '*') {
            star = pi++;
            match = ni;
            while (pi < pattern.size() && pattern[pi] == '*') pi++;  // collapse **
        } else if (pi < pattern.size() && pattern[pi] == '?') {
            pi++; ni++;
        } else if (pi < pattern.size() && pattern[pi] == '[') {
            size_t close = pattern.find(']', pi + 1);
            if (close == std::string_view::npos) {
                if (eq(pattern[pi], name[ni])) { pi++; ni++; }
                else if (star != std::string_view::npos) { pi = star + 1; ni = ++match; }
                else return false;
                continue;
            }
            bool neg = pi + 1 < close && pattern[pi + 1] == '!';
            bool hit = false;
            size_t j = pi + 1 + (neg ? 1 : 0);
            while (j < close) {
                if (j + 2 < close && pattern[j + 1] == '-') {
                    if (name[ni] >= pattern[j] && name[ni] <= pattern[j + 2]) hit = true;
                    j += 3;
                } else {
                    if (eq(pattern[j], name[ni])) hit = true;
                    j++;
                }
            }
            if (hit != neg) { pi = close + 1; ni++; }
            else if (star != std::string_view::npos) { pi = star + 1; ni = ++match; }
            else return false;
        } else if (pi < pattern.size() && eq(pattern[pi], name[ni])) {
            pi++; ni++;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            ni = ++match;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') pi++;
    return pi == pattern.size();
}

} // namespace moha::tools::util
