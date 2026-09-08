// thread_blob_test — image bytes and large tool outputs must round-trip
// through the blob store byte-for-byte, and must keep the thread JSON small.
//
// Why this exists: a real thread here reached 31 MB, of which 10.8 MB was
// base64 image data and 12 MB was tool output, against 0.7 MB of actual
// conversation text. All of it was re-read and re-parsed on every thread
// switch, so opening a long thread stalled the UI for over a second. The
// bytes now live in threads/blobs/<hash> and the message keeps a reference.
//
// The contract has two halves and BOTH matter: the payload must come back
// identical (a store that loses bytes is worse than a slow one), and the
// JSON must actually shrink (or the change bought nothing).

#include "agtest.hpp"

#include "agentty/io/persistence.hpp"
#include "agentty/domain/conversation.hpp"
#include "agentty/util/base64.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace agentty;

// load_thread lives on io::FsStore; persistence exposes the file-level form.
static std::optional<Thread> load_t(const ThreadId& id) {
    auto p = persistence::threads_dir() / (id.value + ".json");
    auto loaded = persistence::load_thread_file(p);
    if (!loaded) return std::nullopt;
    return std::move(*loaded);
}

namespace {

[[nodiscard]] std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

// Bytes that are hostile to a text pipeline: NULs, a lone 0xFF, and a
// sequence that is not valid UTF-8. A blob must not sanitise these.
[[nodiscard]] std::string binary_payload(std::size_t n) {
    std::string s;
    s.reserve(n);
    s += "\x89PNG\r\n\x1a\n";              // real PNG magic
    for (std::size_t i = s.size(); i < n; ++i)
        s.push_back(static_cast<char>(i * 7919u));   // full 0..255 range
    return s;
}

} // namespace

TEST_CASE("thread blobs: images round-trip byte-exact and shrink the JSON") {
    Message user;
    user.role = Role::User;
    user.text = "look at this";
    ImageContent img;
    img.media_type = "image/png";
    img.bytes = binary_payload(300u * 1024u);   // a realistic screenshot
    const std::string original = img.bytes;
    user.images.push_back(std::move(img));

    Thread t{ThreadId{"blobtest"}, "blob round-trip", {user}, {}, {}};
    persistence::save_thread(t);
    persistence::flush_pending_saves();

    const fs::path file =
        persistence::threads_dir() / "blobtest.json";
    const std::string json_text = read_file(file);

    // The 300 KB payload must NOT be sitting in the thread JSON. Base64 of
    // it would be ~400 KB; a reference is a few dozen bytes.
    CHECK_MESSAGE(json_text.size() < 32u * 1024u,
                  "thread JSON must not carry the image payload inline");

    auto loaded = load_t(ThreadId{"blobtest"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->messages.size() == 1u);
    REQUIRE(loaded->messages[0].images.size() == 1u);
    CHECK_MESSAGE(loaded->messages[0].images[0].bytes == original,
                  "image bytes must survive verbatim, NUls and all");
    CHECK(loaded->messages[0].images[0].media_type == "image/png");

    persistence::delete_thread(ThreadId{"blobtest"});
}

TEST_CASE("thread blobs: large tool output round-trips") {
    Message asst;
    asst.role = Role::Assistant;
    asst.text = "ran it";
    ToolUse tc;
    tc.id   = ToolCallId{"call_1"};
    tc.name = ToolName{"shell"};
    // Well past the 8 KB inline threshold.
    std::string big;
    for (int i = 0; i < 4000; ++i)
        big += "line " + std::to_string(i) + " of captured output\n";
    const std::string original = big;
    ToolUse::Done done;
    done.output = big;
    tc.status = std::move(done);
    asst.tool_calls.push_back(std::move(tc));

    Thread t{ThreadId{"blobtool"}, "tool blob", {asst}, {}, {}};
    persistence::save_thread(t);
    persistence::flush_pending_saves();

    const std::string json_text =
        read_file(persistence::threads_dir() / "blobtool.json");
    CHECK_MESSAGE(json_text.size() < 8u * 1024u,
                  "large tool output must not be inline in the thread JSON");

    auto loaded = load_t(ThreadId{"blobtool"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->messages.size() == 1u);
    REQUIRE(loaded->messages[0].tool_calls.size() == 1u);
    CHECK_MESSAGE(loaded->messages[0].tool_calls[0].output() == original,
                  "tool output must survive verbatim");

    persistence::delete_thread(ThreadId{"blobtool"});
}

TEST_CASE("thread blobs: identical payloads share one file") {
    // Content addressing means the same screenshot pasted twice — or the
    // same turn retried — costs one blob, not N.
    const std::string payload = binary_payload(64u * 1024u);

    auto make = [&](const char* id) {
        Message m;
        m.role = Role::User;
        m.text = "same image";
        ImageContent img;
        img.media_type = "image/png";
        img.bytes = payload;
        m.images.push_back(std::move(img));
        Thread t{ThreadId{id}, "dedup", {m}, {}, {}};
        persistence::save_thread(t);
        persistence::flush_pending_saves();
    };

    const fs::path blobs = persistence::threads_dir() / "blobs";
    std::error_code ec;
    const auto count_blobs = [&] {
        std::size_t n = 0;
        for (auto& e : fs::directory_iterator(blobs, ec))
            if (e.is_regular_file(ec)) ++n;
        return n;
    };

    make("dedup_a");
    const std::size_t after_first = count_blobs();
    make("dedup_b");
    const std::size_t after_second = count_blobs();

    CHECK_MESSAGE(after_second == after_first,
                  "an identical payload must not create a second blob");

    // Both threads still read their image back.
    for (const char* id : {"dedup_a", "dedup_b"}) {
        auto loaded = load_t(ThreadId{id});
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->messages[0].images.size() == 1u);
        CHECK(loaded->messages[0].images[0].bytes == payload);
        persistence::delete_thread(ThreadId{id});
    }
}

TEST_CASE("thread blobs: thinking payloads round-trip") {
    // thinking_blocks + signatures were 6.8 MB of one real 32 MB thread.
    // They are never displayed — only replayed to the provider — but
    // Anthropic 400s a tool_use turn whose thinking block was dropped, so
    // they must come back EXACTLY, not merely approximately.
    Message a;
    a.role = Role::Assistant;
    a.text = "done";
    std::string reasoning;
    for (int i = 0; i < 3000; ++i)
        reasoning += "considering branch " + std::to_string(i) + "\n";
    std::string sig(12u * 1024u, 'S');   // opaque provider signature
    a.thinking = reasoning;
    a.thinking_signature = sig;
    a.thinking_blocks.push_back(
        Message::ThinkingBlock{reasoning, sig, ""});

    Thread t{ThreadId{"blobthink"}, "thinking blob", {a}, {}, {}};
    persistence::save_thread(t);
    persistence::flush_pending_saves();

    const std::string json_text =
        read_file(persistence::threads_dir() / "blobthink.json");
    CHECK_MESSAGE(json_text.size() < 8u * 1024u,
                  "thinking payloads must not sit inline in the thread JSON");

    auto loaded = load_t(ThreadId{"blobthink"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->messages.size() == 1u);
    const auto& got = loaded->messages[0];
    CHECK(got.thinking == reasoning);
    CHECK(got.thinking_signature == sig);
    REQUIRE(got.thinking_blocks.size() == 1u);
    CHECK_MESSAGE(got.thinking_blocks[0].text == reasoning,
                  "block text must replay verbatim");
    CHECK_MESSAGE(got.thinking_blocks[0].signature == sig,
                  "a mangled signature 400s the next turn");

    persistence::delete_thread(ThreadId{"blobthink"});
}

TEST_CASE("thread blobs: a legacy inline thread still loads") {
    // Threads written before the blob store keep an inline base64 "data"
    // field. They must keep working with no migration step — otherwise
    // upgrading silently blanks every image already on disk.
    const std::string bytes = binary_payload(4096);
    Message m;
    m.role = Role::User;
    m.text = "legacy";
    ImageContent img;
    img.media_type = "image/png";
    img.bytes = bytes;
    m.images.push_back(std::move(img));
    Thread t{ThreadId{"legacyimg"}, "legacy", {m}, {}, {}};
    persistence::save_thread(t);
    persistence::flush_pending_saves();

    // Rewrite the file in the OLD shape: inline data, no blob reference.
    const fs::path file = persistence::threads_dir() / "legacyimg.json";
    std::string text = read_file(file);
    const auto pos = text.find("\"blob\"");
    REQUIRE(pos != std::string::npos);
    // Swap the reference for the inline encoding the old writer produced.
    const auto end = text.find('"', text.find(':', pos) + 2);
    const auto close = text.find('"', end + 1);
    text.replace(pos, close - pos + 1,
                 "\"data\":\"" + util::base64_encode(bytes) + "\"");
    { std::ofstream out(file, std::ios::binary); out << text; }

    auto loaded = load_t(ThreadId{"legacyimg"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->messages[0].images.size() == 1u);
    CHECK_MESSAGE(loaded->messages[0].images[0].bytes == bytes,
                  "inline legacy images must still decode");

    persistence::delete_thread(ThreadId{"legacyimg"});
}
