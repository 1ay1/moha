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
// funnels through this, so a Slot alternative added later fails to compile in
// exactly one place rather than scattering.
//
// Generic over the OWNING STRUCT. A row binds to exactly one, so half the
// variant's alternatives do not apply to any given `C`; those are SKIPPED, not
// an error — `apply_env(Settings&)` walking the whole table and passing over
// every rag.* row is the intended behaviour. Returns whether the row was ours,
// so a caller that needs to know can answer honestly rather than hand back a
// default that reads like real data.
template <class C, class Visit>
bool with_slot(C& c, const SettingDef& d, Visit&& v) {
    return std::visit([&](auto member) -> bool {
        if constexpr (requires { c.*member; }) {
            v(c.*member);
            return true;
        } else {
            return false;
        }
    }, d.slot);
}

} // namespace

// ── Read ─────────────────────────────────────────────────────────────────

namespace {

template <class C>
std::string get_impl(const C& c, const SettingDef& d) {
    std::string out;
    (void)with_slot(c, d, [&](const auto& value) {
        using M = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<M, bool>)
            out = value ? "true" : "false";
        else if constexpr (std::is_same_v<M, int>)
            out = std::to_string(value);
        else if constexpr (std::is_same_v<M, float> || std::is_same_v<M, double>)
            out = fmt_real(static_cast<double>(value));
        else
            out = value;   // std::string (Enum)
    });
    return out;
}

} // namespace

std::string get(const store::RagConfig& c, const SettingDef& d) {
    return get_impl(c, d);
}

std::string get(const smart::RoleConfig& s, const SettingDef& d) {
    return get_impl(s, d);
}

// ── Write ────────────────────────────────────────────────────────────────

namespace {

template <class C>
bool set_impl(C& c, const SettingDef& d, std::string_view value) {
    bool ok = false;
    const bool reached = with_slot(c, d, [&](auto& field) {
        using M = std::decay_t<decltype(field)>;

        if constexpr (std::is_same_v<M, bool>) {
            bool b{};
            if (!parse_bool(value, b)) return;
            field = b;
            ok = true;
        }
        else if constexpr (std::is_same_v<M, int>) {
            try {
                const long long n = std::stoll(std::string{value});
                // Clamped, never rejected-then-forgotten: the config must not
                // be able to HOLD an out-of-range value.
                field = static_cast<int>(
                    std::clamp<double>(static_cast<double>(n), d.min, d.max));
                ok = true;
            } catch (...) {}
        }
        else if constexpr (std::is_same_v<M, float> || std::is_same_v<M, double>) {
            try {
                const double x = std::stod(std::string{value});
                field = static_cast<M>(std::clamp(x, d.min, d.max));
                ok = true;
            } catch (...) {}
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
                if (lower(one) == want) { field = std::string{one}; ok = true; return; }
                if (bar == std::string_view::npos) break;
                opts.remove_prefix(bar + 1);
            }
        }
    });
    return reached && ok;
}

template <class C>
bool is_default_impl(const C& c, const C& dflt, const SettingDef& d) {
    bool same = true;
    (void)with_slot(c, d, [&](const auto& field) {
        // Reach the SAME member on the defaults instance. with_slot resolves
        // the pointer against whichever object it is handed, so this is the
        // one comparison rather than a second switch on type.
        (void)with_slot(dflt, d, [&](const auto& other) {
            if constexpr (std::is_same_v<std::decay_t<decltype(field)>,
                                         std::decay_t<decltype(other)>>)
                same = (field == other);
        });
    });
    return same;
}

template <class C>
void reset_impl(C& c, const C& dflt, const SettingDef& d) {
    (void)with_slot(c, d, [&](auto& field) {
        (void)with_slot(dflt, d, [&](const auto& other) {
            if constexpr (std::is_same_v<std::decay_t<decltype(field)>,
                                         std::decay_t<decltype(other)>>)
                field = other;
        });
    });
}

template <class C>
void apply_env_impl(C& c) {
    for (const auto& d : kSettings) {
        if (d.env.empty()) continue;
        const char* raw = std::getenv(std::string{d.env}.c_str());
        if (!raw || !raw[0]) continue;
        // A malformed env value is IGNORED, not fatal and not silently
        // coerced: the shipped default stands, which is the least surprising
        // behaviour for a typo in a shell profile. A row belonging to the
        // OTHER config struct is skipped by with_slot, so each overload
        // applies exactly the rows it owns.
        (void)set_impl(c, d, raw);
    }
}

} // namespace

bool set(store::RagConfig& c, const SettingDef& d, std::string_view value) {
    return set_impl(c, d, value);
}

bool set(smart::RoleConfig& s, const SettingDef& d, std::string_view value) {
    return set_impl(s, d, value);
}

// ── Defaults ─────────────────────────────────────────────────

bool is_default(const store::RagConfig& c, const SettingDef& d) {
    return is_default_impl(c, defaults(), d);
}

bool is_default(const smart::RoleConfig& s, const SettingDef& d) {
    static const smart::RoleConfig kDefaults{};
    return is_default_impl(s, kDefaults, d);
}

void reset(store::RagConfig& c, const SettingDef& d) {
    reset_impl(c, defaults(), d);
}

void reset(smart::RoleConfig& s, const SettingDef& d) {
    static const smart::RoleConfig kDefaults{};
    reset_impl(s, kDefaults, d);
}

// ── Environment ─────────────────────────────────────────────

void apply_env(store::RagConfig& c) { apply_env_impl(c); }
void apply_env(smart::RoleConfig& s) { apply_env_impl(s); }

std::string env_override(const SettingDef& d) {
    if (d.env.empty()) return {};
    const char* raw = std::getenv(std::string{d.env}.c_str());
    return (raw && raw[0]) ? std::string{d.env} : std::string{};
}

} // namespace agentty::settings::registry
