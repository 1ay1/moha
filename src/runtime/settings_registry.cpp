// settings_registry.cpp — the table's derived operations.
//
// Every function here WALKS kSettings. None of them names an individual
// setting, which is the whole point: adding a knob is adding a row, and these
// four functions plus the UI pick it up for free.

#include "agentty/runtime/settings_registry.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <string>

namespace agentty::settings::registry {

namespace {

// The shipped defaults, as a default-constructed config. Comparing against
// this is how `is_default` avoids a second hand-maintained list of defaults —
// the struct's member initialisers are already the single source of truth.
const store::RagConfig& defaults() {
    static const store::RagConfig d{};
    return d;
}

[[nodiscard]] std::string lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

[[nodiscard]] bool parse_bool(std::string_view s, bool& out) {
    const std::string v = lower(s);
    if (v == "1" || v == "true"  || v == "on"  || v == "yes") { out = true;  return true; }
    if (v == "0" || v == "false" || v == "off" || v == "no")  { out = false; return true; }
    return false;
}

// Fixed-decimal formatting without <format>: these are bounded ratios, so
// integer scaling is exact enough and keeps the output stable across
// platforms (printf's %g varies).
[[nodiscard]] std::string fmt_real(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

// One place that knows how to reach a row's storage. Every accessor below
// funnels through these, so a Slot alternative added later fails to compile
// in exactly one function per direction rather than scattering.
template <class Visit>
auto with_slot(const SettingDef& d, Visit&& v) {
    return std::visit(std::forward<Visit>(v), d.slot);
}

} // namespace

// ── Read ─────────────────────────────────────────────────────────────────

std::string get(const store::RagConfig& c, const SettingDef& d) {
    return with_slot(d, [&](auto member) -> std::string {
        using M = std::decay_t<decltype(c.*member)>;
        if constexpr (std::is_same_v<M, bool>)
            return (c.*member) ? "true" : "false";
        else if constexpr (std::is_same_v<M, int>)
            return std::to_string(c.*member);
        else if constexpr (std::is_same_v<M, float> || std::is_same_v<M, double>)
            return fmt_real(static_cast<double>(c.*member));
        else
            return c.*member;   // std::string (Enum)
    });
}

// ── Write ────────────────────────────────────────────────────────────────

bool set(store::RagConfig& c, const SettingDef& d, std::string_view value) {
    return with_slot(d, [&](auto member) -> bool {
        using M = std::decay_t<decltype(c.*member)>;

        if constexpr (std::is_same_v<M, bool>) {
            bool b{};
            if (!parse_bool(value, b)) return false;
            c.*member = b;
            return true;
        }
        else if constexpr (std::is_same_v<M, int>) {
            try {
                const long long n = std::stoll(std::string{value});
                // Clamped, never rejected-then-forgotten: the config must not
                // be able to HOLD an out-of-range value.
                c.*member = static_cast<int>(
                    std::clamp<double>(static_cast<double>(n), d.min, d.max));
                return true;
            } catch (...) { return false; }
        }
        else if constexpr (std::is_same_v<M, float> || std::is_same_v<M, double>) {
            try {
                const double x = std::stod(std::string{value});
                c.*member = static_cast<M>(std::clamp(x, d.min, d.max));
                return true;
            } catch (...) { return false; }
        }
        else {
            // Enum: the value must be one of the declared options, or a typo
            // in settings.json would silently select a mode that does not
            // exist and the engine would fall back without saying so.
            std::string_view opts = d.options;
            const std::string want = lower(value);
            while (!opts.empty()) {
                const auto bar = opts.find('|');
                const auto one = opts.substr(0, bar);
                if (lower(one) == want) { c.*member = std::string{one}; return true; }
                if (bar == std::string_view::npos) break;
                opts.remove_prefix(bar + 1);
            }
            return false;
        }
    });
}

// ── Defaults ─────────────────────────────────────────────────────────────

bool is_default(const store::RagConfig& c, const SettingDef& d) {
    const auto& dflt = defaults();
    return with_slot(d, [&](auto member) {
        return (c.*member) == (dflt.*member);
    });
}

void reset(store::RagConfig& c, const SettingDef& d) {
    const auto& dflt = defaults();
    with_slot(d, [&](auto member) { c.*member = dflt.*member; });
}

// ── Environment ──────────────────────────────────────────────────────────

void apply_env(store::RagConfig& c) {
    for (const auto& d : kSettings) {
        if (d.env.empty()) continue;
        const char* raw = std::getenv(std::string{d.env}.c_str());
        if (!raw || !raw[0]) continue;
        // A malformed env value is IGNORED, not fatal and not silently
        // coerced: the shipped default stands, which is the least surprising
        // behaviour for a typo in a shell profile.
        (void)set(c, d, raw);
    }
}

} // namespace agentty::settings::registry
