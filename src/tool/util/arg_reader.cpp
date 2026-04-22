#include "moha/tool/util/arg_reader.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace moha::tools::util {

using nlohmann::json;

const json* ArgReader::raw(std::string_view key) const noexcept {
    if (!args_.is_object()) return nullptr;
    auto it = args_.find(std::string{key});
    return it == args_.end() ? nullptr : &*it;
}

std::string ArgReader::str(std::string_view key,
                           std::string def,
                           std::string* note) const {
    const json* v = raw(key);
    if (!v) return def;
    if (v->is_string()) return v->get<std::string>();
    if (v->is_null()) return def;
    if (v->is_array()) {
        std::string out;
        for (std::size_t i = 0; i < v->size(); ++i) {
            if (i) out += '\n';
            const auto& el = (*v)[i];
            if (el.is_string()) out += el.get<std::string>();
            else                out += el.dump();
        }
        if (note) *note = " (" + std::string{key} + " was an array — joined with newlines)";
        return out;
    }
    // number / bool / object — surface a literal repr so the model can see
    // what it sent and correct on retry.
    std::string out = v->dump();
    if (note) *note = " (" + std::string{key} + " was not a string — coerced)";
    return out;
}

std::optional<std::string> ArgReader::require_str(std::string_view key) const {
    const json* v = raw(key);
    if (!v || v->is_null()) return std::nullopt;
    if (v->is_string()) {
        auto s = v->get<std::string>();
        return s.empty() ? std::nullopt : std::optional<std::string>{std::move(s)};
    }
    // Numbers/bools coerced via dump() — still surface as "has a value".
    auto s = v->dump();
    return s.empty() ? std::nullopt : std::optional<std::string>{std::move(s)};
}

int ArgReader::integer(std::string_view key, int def) const {
    const json* v = raw(key);
    if (!v || v->is_null()) return def;
    if (v->is_number_integer()) return v->get<int>();
    if (v->is_number_float())   return static_cast<int>(v->get<double>());
    if (v->is_string()) {
        const auto& s = v->get_ref<const std::string&>();
        int out = def;
        auto r = std::from_chars(s.data(), s.data() + s.size(), out);
        if (r.ec == std::errc{}) return out;
    }
    return def;
}

bool ArgReader::boolean(std::string_view key, bool def) const {
    const json* v = raw(key);
    if (!v || v->is_null()) return def;
    if (v->is_boolean()) return v->get<bool>();
    if (v->is_number_integer()) return v->get<int>() != 0;
    if (v->is_string()) {
        std::string s = v->get<std::string>();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (s == "true" || s == "1" || s == "yes")  return true;
        if (s == "false"|| s == "0" || s == "no")   return false;
    }
    return def;
}

} // namespace moha::tools::util
