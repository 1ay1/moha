// activity_tape_test — the live-stream contract of maya::ActivityIndicator.
//
// The tape row is agentty's "model is working" narration. Its Config now has
// two states with a hard semantic boundary:
//   WAITING (stream empty): decorative noise + rotating word pool. Noise
//     MEANS "no bytes have arrived" — the pre-first-byte / TTFT signal.
//   LIVE (stream non-empty): a right-anchored hexdump of the REAL bytes the
//     host is receiving. The offset column is the true cumulative byte count
//     (an odometer, counting UP), the hex/ASCII columns are the actual
//     stream tail, and motion comes from data arrival, not the wall clock.
//
// These tests pin the properties that make the row honest rather than
// decorative — the exact critique that triggered the redesign ("merely a
// countdown and not hex values of the current"):
//   1. LIVE bytes surface verbatim: printable stream bytes appear in the
//      ASCII gutter, their hex in the hex column.
//   2. The offset is the TRUE total, not a countdown: rendering the same
//      stream with a larger total moves the printed offset UP.
//   3. Arrival is the only motion: with the anim clock FROZEN, appending
//      bytes still changes the rendered row (data drives the tape), while
//      the settled (non-hot-tail) region is time-invariant.
//   4. WAITING state still renders (word pool machinery intact) and
//      differs from LIVE — the two states are visually distinct by
//      construction, so noise can't be mistaken for signal.

#include "agtest.hpp"

#include <maya/app/inline.hpp>
#include <maya/core/anim_clock.hpp>
#include <maya/widget/activity_indicator.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace {

// Render one indicator row at a width generous enough for the FULL
// variant (offset + hex + gutter + detail).
std::string render_row(const maya::ActivityIndicator::Config& cfg,
                       int width = 120) {
    maya::ActivityIndicator w{cfg};
    return maya::render_to_string(w.build(), width);
}

struct FrozenClock {
    explicit FrozenClock(std::int64_t at_ms) {
        maya::testing::freeze_anim_clock(at_ms);
    }
    ~FrozenClock() { maya::testing::unfreeze_anim_clock(); }
};

const std::vector<std::string_view> kWords{"thinking", "reasoning"};

} // namespace

TEST_CASE("activity tape: live bytes surface verbatim in hex + gutter") {
    FrozenClock fc{100000};

    // A distinctive tail — if the tape is honest, these exact characters
    // are what the gutter shows (minus the hot tail, which tumbles).
    const std::string stream = "the quick brown fox JUMPED over";
    maya::ActivityIndicator::Config cfg;
    cfg.words        = kWords;
    cfg.stream       = stream;
    cfg.stream_total = stream.size();

    const std::string row = render_row(cfg);

    // The settled portion of the stream tail must appear as literal text in
    // the ASCII gutter. The visible window is the NEWEST bytes and the last
    // ~2 are the tumbling hot tail, so probe a mid-tail substring.
    CHECK(row.find("JUMPED") != std::string::npos);
    // And its hex must be present too ('J' = 4a, 'U' = 55, 'M' = 4d).
    CHECK(row.find("4a 55 4d") != std::string::npos);
}

TEST_CASE("activity tape: offset is the true total, counting up") {
    FrozenClock fc{100000};

    const std::string stream(64, 'x');
    maya::ActivityIndicator::Config cfg;
    cfg.words        = kWords;
    cfg.stream       = stream;

    cfg.stream_total = 0x40;                  // 64 bytes so far
    const std::string r1 = render_row(cfg);
    CHECK(r1.find("0x000040") != std::string::npos);

    cfg.stream_total = 0x2100;                // later in the same stream
    const std::string r2 = render_row(cfg);
    CHECK(r2.find("0x002100") != std::string::npos);
    // Countdown would render a value that DECREASES with progress; the
    // odometer strictly increases. Both totals printed verbatim proves the
    // column is data, not animation.
}

TEST_CASE("activity tape: arrival moves the tape even with time frozen") {
    FrozenClock fc{100000};

    maya::ActivityIndicator::Config cfg;
    cfg.words = kWords;

    const std::string s1 = "alpha beta gamma delta epsilon";
    cfg.stream       = s1;
    cfg.stream_total = s1.size();
    const std::string r1 = render_row(cfg);

    // Same frozen instant, three more bytes "arrive".
    const std::string s2 = s1 + " ze";
    cfg.stream       = s2;
    cfg.stream_total = s2.size();
    const std::string r2 = render_row(cfg);

    // Data is the motion source: the row must differ (window slid, offset
    // grew) with zero wall-clock movement.
    CHECK(r1 != r2);

    // And with NO arrival, the settled region is a pure function of the
    // data: re-rendering the identical config at the identical frozen time
    // is byte-identical (no hidden nondeterminism in the live path).
    const std::string r1_again = render_row(cfg);
    const std::string r2_again = render_row(cfg);
    CHECK(r2_again == r1_again);
}

TEST_CASE("activity tape: waiting state renders and differs from live") {
    FrozenClock fc{100000};

    maya::ActivityIndicator::Config waiting;
    waiting.words = kWords;   // stream empty → noise + word machinery

    maya::ActivityIndicator::Config live = waiting;
    const std::string stream = "some real streamed bytes here";
    live.stream       = stream;
    live.stream_total = stream.size();

    const std::string rw = render_row(waiting);
    const std::string rl = render_row(live);

    CHECK(!rw.empty());
    CHECK(!rl.empty());
    // The two states must be distinguishable at a glance — that contrast IS
    // the TTFT signal (noise = nothing yet, structure = bytes flowing).
    CHECK(rw != rl);
    // Waiting shows the decorative countdown offset (starts at 0x007ffd
    // band, far above any real total this test uses); live shows the true
    // total. Both are 6-digit so the chrome width can't jump at the flip.
    CHECK(rl.find("0x00001d") != std::string::npos);  // 29 == 0x1d
}

TEST_CASE("activity tape: narrow widths never wrap the row") {
    FrozenClock fc{100000};

    const std::string stream = "narrow-width honesty check";
    for (int w : {30, 44, 60, 80}) {
        maya::ActivityIndicator::Config cfg;
        cfg.words        = kWords;
        cfg.stream       = stream;
        cfg.stream_total = stream.size();
        maya::ActivityIndicator wid{cfg};
        const std::string out =
            maya::render_to_string(wid.build(), w);
        // Single row: no embedded newline betweeen non-empty lines.
        // (render_to_string appends a trailing newline per row; a wrapped
        // row would produce 2+ non-empty lines.)
        int lines = 0;
        std::size_t pos = 0;
        while (pos < out.size()) {
            std::size_t nl = out.find('\n', pos);
            if (nl == std::string::npos) nl = out.size();
            std::string_view line{out.data() + pos, nl - pos};
            if (line.find_first_not_of(' ') != std::string_view::npos)
                ++lines;
            pos = nl + 1;
        }
        CHECK(lines <= 1);
    }
}
