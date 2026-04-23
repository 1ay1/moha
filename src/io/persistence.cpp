#include "moha/io/persistence.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

#ifdef _WIN32
#  include <io.h>
#else
#  include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "moha/tool/util/utf8.hpp"

namespace moha::persistence {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
// Atomic + durable write: write to <target>.tmp, fsync, rename. A crash
// or ctrl-C mid-write leaves the previous version intact — the loader
// never sees a truncated file that its `catch (...)` would silently drop.
// Binary mode avoids CRLF translation so the on-disk bytes match dump(2).
bool write_json_atomic(const fs::path& target, const std::string& content) {
    fs::path tmp = target;
    tmp += ".tmp";
#ifdef _WIN32
    FILE* fp = ::_wfopen(tmp.wstring().c_str(), L"wb");
#else
    FILE* fp = std::fopen(tmp.c_str(), "wb");
#endif
    if (!fp) return false;
    if (std::fwrite(content.data(), 1, content.size(), fp) != content.size()) {
        std::fclose(fp);
        std::error_code ec; fs::remove(tmp, ec);
        return false;
    }
    std::fflush(fp);
#ifdef _WIN32
    (void)::_commit(::_fileno(fp));
#else
    (void)::fsync(::fileno(fp));
#endif
    if (std::fclose(fp) != 0) {
        std::error_code ec; fs::remove(tmp, ec);
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ec2; fs::remove(tmp, ec2);
        return false;
    }
    return true;
}
} // namespace

fs::path data_dir() {
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
    fs::path p = home ? fs::path(home) : fs::current_path();
    p /= ".moha";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

fs::path threads_dir() {
    auto p = data_dir() / "threads";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

static std::string role_to_string(Role r) {
    switch (r) {
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::System: return "system";
    }
    return "user";
}
static Role role_from_string(const std::string& s) {
    if (s == "assistant") return Role::Assistant;
    if (s == "system")    return Role::System;
    return Role::User;
}

static json message_to_json(const Message& m) {
    // Belt-and-suspenders UTF-8 scrub. Tool output and freeform text can
    // contain raw bytes from arbitrary files (Latin-1 .htm, Shift-JIS logs)
    // that nlohmann::json::dump() refuses to serialise — it throws
    // type_error.316 and we used to terminate(). Scrub at the boundary so
    // bad bytes can never reach dump(). Tools that already scrub upstream
    // pay only the validate cost here.
    json j;
    j["role"] = role_to_string(m.role);
    j["text"] = tools::util::to_valid_utf8(m.text);
    j["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
        m.timestamp.time_since_epoch()).count();
    json tcs = json::array();
    for (const auto& tc : m.tool_calls) {
        json t;
        t["id"] = tc.id;
        t["name"] = tc.name;
        t["args"] = tc.args;
        t["output"] = tools::util::to_valid_utf8(tc.output()); // empty unless terminal
        t["status"] = std::string{tc.status_name()};
        tcs.push_back(std::move(t));
    }
    j["tool_calls"] = std::move(tcs);
    if (m.checkpoint_id) j["checkpoint_id"] = *m.checkpoint_id;
    return j;
}

static Message message_from_json(const json& j) {
    Message m;
    m.role = role_from_string(j.value("role", "user"));
    m.text = j.value("text", "");
    if (j.contains("timestamp"))
        m.timestamp = std::chrono::system_clock::time_point{
            std::chrono::seconds{j["timestamp"].get<long long>()}};
    if (j.contains("tool_calls")) {
        for (const auto& t : j["tool_calls"]) {
            ToolUse tc;
            tc.id = ToolCallId{t.value("id", "")};
            tc.name = ToolName{t.value("name", "")};
            tc.args = t.value("args", json::object());
            // Old persisted threads stored status as an int enum; new ones
            // use the string tag returned by ToolUse::status_name(). Accept
            // both so existing on-disk threads keep loading.
            std::string status_tag;
            std::string output = t.value("output", "");
            if (auto it = t.find("status"); it != t.end()) {
                if (it->is_string())          status_tag = it->get<std::string>();
                else if (it->is_number()) {
                    static constexpr std::string_view legacy[] = {
                        "pending","approved","running","done","failed","rejected"};
                    int idx = it->get<int>();
                    status_tag = std::string{
                        idx >= 0 && idx < (int)std::size(legacy)
                            ? legacy[idx] : std::string_view{"pending"}};
                }
            }
            // Reconstruct the variant. Persisted threads only ever land in
            // terminal states (in-flight tools are never serialized), so the
            // intermediate states reset to a no-args-time-stamp default.
            if (status_tag == "done")
                tc.status = ToolUse::Done{{}, {}, std::move(output)};
            else if (status_tag == "failed" || status_tag == "error")
                tc.status = ToolUse::Failed{{}, {}, std::move(output)};
            else if (status_tag == "rejected")
                tc.status = ToolUse::Rejected{{}};
            else if (status_tag == "running")
                tc.status = ToolUse::Running{{}, {}};
            else if (status_tag == "approved")
                tc.status = ToolUse::Approved{{}};
            else
                tc.status = ToolUse::Pending{{}};
            m.tool_calls.push_back(std::move(tc));
        }
    }
    if (j.contains("checkpoint_id"))
        m.checkpoint_id = CheckpointId{j["checkpoint_id"].get<std::string>()};
    return m;
}

std::vector<Thread> load_all_threads() {
    std::vector<Thread> out;
    std::error_code ec;
    if (!fs::exists(threads_dir(), ec)) return out;
    for (const auto& e : fs::directory_iterator(threads_dir(), ec)) {
        if (e.path().extension() != ".json") continue;
        std::ifstream ifs(e.path());
        if (!ifs) continue;
        try {
            json j; ifs >> j;
            Thread t;
            t.id = ThreadId{j.value("id", "")};
            t.title = j.value("title", "");
            if (j.contains("created_at"))
                t.created_at = std::chrono::system_clock::time_point{
                    std::chrono::seconds{j["created_at"].get<long long>()}};
            if (j.contains("updated_at"))
                t.updated_at = std::chrono::system_clock::time_point{
                    std::chrono::seconds{j["updated_at"].get<long long>()}};
            for (const auto& mj : j.value("messages", json::array()))
                t.messages.push_back(message_from_json(mj));
            out.push_back(std::move(t));
        } catch (...) {
            continue;
        }
    }
    std::sort(out.begin(), out.end(), [](const Thread& a, const Thread& b){
        return a.updated_at > b.updated_at;
    });
    return out;
}

void save_thread(const Thread& t) {
    if (t.id.empty() || t.messages.empty()) return;
    json j;
    j["id"] = t.id;
    j["title"] = t.title;
    j["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(
        t.created_at.time_since_epoch()).count();
    j["updated_at"] = std::chrono::duration_cast<std::chrono::seconds>(
        t.updated_at.time_since_epoch()).count();
    json msgs = json::array();
    for (const auto& m : t.messages) msgs.push_back(message_to_json(m));
    j["messages"] = std::move(msgs);
    // dump() throws type_error.316 on non-UTF-8 bytes. Scrubbing in
    // message_to_json should have caught everything, but swallow the
    // exception as a belt-and-suspenders guard against future regressions —
    // a silently-skipped save beats a process-terminating uncaught throw.
    try {
        (void)write_json_atomic(threads_dir() / (t.id.value + ".json"), j.dump(2));
    } catch (const nlohmann::json::exception&) {
        // caller can't react; best-effort persistence is acceptable here.
    }
}

void delete_thread(const ThreadId& id) {
    std::error_code ec;
    fs::remove(threads_dir() / (id.value + ".json"), ec);
}

store::Settings load_settings() {
    store::Settings s;
    std::ifstream ifs(data_dir() / "settings.json");
    if (!ifs) return s;
    try {
        json j; ifs >> j;
        s.model_id = ModelId{j.value("model_id", "")};
        s.profile = static_cast<Profile>(j.value("profile", 0));
        auto favs = j.value("favorite_models", std::vector<std::string>{});
        for (auto& f : favs) s.favorite_models.push_back(ModelId{std::move(f)});
    } catch (...) {}
    return s;
}

void save_settings(const store::Settings& s) {
    json j;
    j["model_id"] = s.model_id;
    j["profile"] = static_cast<int>(s.profile);
    json favs = json::array();
    for (const auto& mid : s.favorite_models) favs.push_back(mid);
    j["favorite_models"] = std::move(favs);
    (void)write_json_atomic(data_dir() / "settings.json", j.dump(2));
}

ThreadId new_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << dist(rng);
    return ThreadId{oss.str()};
}

std::string title_from_first_message(std::string_view text) {
    std::string t{text};
    for (auto& c : t) if (c == '\n' || c == '\r') c = ' ';
    if (t.size() > 60) { t.resize(57); t += "..."; }
    if (t.empty()) t = "New thread";
    return t;
}

} // namespace moha::persistence
