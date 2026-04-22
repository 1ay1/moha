#include "moha/tool/util/fs_helpers.hpp"

#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace moha::tools::util {

std::string read_file(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream oss; oss << ifs.rdbuf();
    return oss.str();
}

std::string write_file(const fs::path& p, std::string_view content) {
    auto parent = p.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) return "failed to create directory '" + parent.string() + "': " + ec.message();
    }
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) return "cannot open '" + p.string() + "' for writing";
    ofs.write(content.data(), (std::streamsize)content.size());
    ofs.flush();
    if (!ofs) return "write to '" + p.string() + "' failed (disk full, locked, or permission denied)";
    return {};
}

fs::path normalize_path(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))  s.remove_suffix(1);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"')
                          || (s.front() == '\'' && s.back() == '\''))) {
        s.remove_prefix(1);
        s.remove_suffix(1);
    }
    fs::path p{s};
    std::error_code ec;
    if (!p.is_absolute()) p = fs::absolute(p, ec);
    return p;
}

bool should_skip_dir(std::string_view name) noexcept {
    static const std::vector<std::string_view> skip = {
        ".git", "node_modules", "build", "target", "__pycache__",
        ".cache", "vendor", "dist", "out", ".next", ".venv",
        "cmake-build-debug", "cmake-build-release", ".idea", ".vscode",
        "_deps", "third_party", "thirdparty", "3rdparty", "external",
    };
    for (auto s : skip) if (name == s) return true;
    return false;
}

bool is_binary_file(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return true;
    char buf[512];
    ifs.read(buf, sizeof(buf));
    auto n = ifs.gcount();
    for (int i = 0; i < n; ++i)
        if (buf[i] == '\0') return true;
    return false;
}

} // namespace moha::tools::util
