// Shared pure helpers for reading tool args and tool output. See
// tool_args.hpp for intent. No maya / widget dependencies — pure data
// shaping — so both thread.cpp (timeline compact-body) and tool_card.cpp
// (widget render) can link against it without dragging widget headers.

#include "moha/runtime/view/tool_args.hpp"

#include <chrono>

namespace moha::ui {

std::string safe_arg(const nlohmann::json& args, const char* key) {
    if (!args.is_object()) return {};
    return args.value(key, "");
}

std::string pick_arg(const nlohmann::json& args,
                     std::initializer_list<const char*> keys) {
    if (!args.is_object()) return {};
    for (const char* k : keys) {
        if (auto it = args.find(k); it != args.end() && it->is_string()) {
            const auto& s = it->get_ref<const std::string&>();
            if (!s.empty()) return s;
        }
    }
    return {};
}

int safe_int_arg(const nlohmann::json& args, const char* key, int def) {
    if (!args.is_object() || !args.contains(key)) return def;
    return args.value(key, def);
}

int count_lines(const std::string& s) {
    int n = 0;
    for (char c : s) if (c == '\n') n++;
    return n + (!s.empty() && s.back() != '\n' ? 1 : 0);
}

float tool_elapsed(const ToolUse& tc) {
    auto zero = std::chrono::steady_clock::time_point{};
    auto started = tc.started_at();
    if (started == zero) return 0.0f;
    auto finished = tc.finished_at();
    auto end = finished == zero ? std::chrono::steady_clock::now() : finished;
    auto dt = end - started;
    return std::chrono::duration<float>(dt).count();
}

std::string strip_bash_output_fence(const std::string& s) {
    std::string_view sv{s};
    auto drop_trailer = [&](std::string_view marker) {
        auto pos = sv.rfind(marker);
        if (pos != std::string_view::npos) sv = sv.substr(0, pos);
        while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r'
                               || sv.back() == ' '  || sv.back() == '\t'))
            sv.remove_suffix(1);
    };
    drop_trailer("\n\n[elapsed:");
    drop_trailer("\n\n[output truncated");

    auto fence = sv.find("```");
    if (fence == std::string_view::npos) return std::string{sv};
    // Allow a leading "Command …\n\n" header before the fence — the failure
    // and timeout branches put one there.
    auto body_start = fence + 3;
    // Skip a language tag (we don't emit one, but be forgiving) and the
    // newline after the opening fence.
    while (body_start < sv.size() && sv[body_start] != '\n') ++body_start;
    if (body_start < sv.size() && sv[body_start] == '\n') ++body_start;

    auto close = sv.rfind("```");
    if (close == std::string_view::npos || close <= body_start)
        return std::string{sv.substr(body_start)};

    auto body_end = close;
    while (body_end > body_start
           && (sv[body_end - 1] == '\n' || sv[body_end - 1] == '\r'))
        --body_end;

    std::string header{sv.substr(0, fence)};
    while (!header.empty() && (header.back() == '\n' || header.back() == '\r'
                               || header.back() == ' '))
        header.pop_back();
    std::string body{sv.substr(body_start, body_end - body_start)};
    if (header.empty()) return body;
    if (body.empty()) return header;
    return header + "\n\n" + body;
}

int parse_exit_code(const std::string& output) {
    struct Marker { const char* text; size_t skip; };
    static constexpr Marker markers[] = {
        {"failed with exit code ", 22},
        {"[exit code ",            11},
    };
    for (const auto& m : markers) {
        auto pos = output.rfind(m.text);
        if (pos == std::string::npos) continue;
        try { return std::stoi(output.substr(pos + m.skip)); }
        catch (...) { return 1; }
    }
    if (output.find("timed out") != std::string::npos) return 124;
    return 0;
}

} // namespace moha::ui
