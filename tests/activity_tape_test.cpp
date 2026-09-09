// activity_tape_test — the honest-tape contract of maya::ActivityIndicator.
//
// The tape row is agentty's "model is working" narration. It has three
// modes, selected strictly by what data exists, and NOTHING it renders is
// fake:
//   WRITE  (stream non-empty): right-anchored hexdump of the REAL bytes
//     arriving (model output). Offset = true cumulative byte count (an
//     odometer, counting UP); motion comes from data arrival, not the
//     wall clock.
//   READ   (context non-empty, stream empty): a read head scanning the
//     REAL prompt bytes at a steady cadence — the input-side mirror for
//     the TTFT window. Offset = the head's true position in the context.
//   STATIC (both empty): channel noise, offset pinned 0x000000 — honest
//     "no signal", never dressed as information.
//
// These tests pin the properties that make the row information rather
// than decoration — the critique that triggered the redesign ("merely a
// countdown and not hex values of the current"):
//   1. WRITE bytes surface verbatim: printable stream bytes appear in the
//      ASCII gutter, their hex in the hex column.
//   2. The WRITE offset is the TRUE total: rendering the same stream with
//      a larger total moves the printed offset UP.
//   3. Arrival is WRITE's only motion: with the anim clock FROZEN,
//      appending bytes still changes the row, while identical input at
//      identical time renders identically.
//   4. READ shows the real context: prompt words appear in the gutter,
//      the offset is a real in-context position, and the head ADVANCES
//      as time passes (input-side liveness).
//   5. STATIC is pinned at offset 0x000000 and shows no real text — the
//      three modes are pairwise distinguishable at a glance.
//   6. No width 30–120 ever wraps the row.

#include "agtest.hpp"

#include <maya/app/inline.hpp>
#include <maya/core/anim_clock.hpp>
#include <maya/widget/activity_indicator.hpp>

#include <string>
#include <string_view>

namespace {

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

} // namespace

TEST_CASE("activity tape: WRITE bytes surface verbatim in hex + gutter") {
    FrozenClock fc{100000};

    const std::string stream = "the quick brown fox JUMPED over";
    maya::ActivityIndicator::Config cfg;
    cfg.stream       = stream;
    cfg.stream_total = stream.size();

    const std::string row = render_row(cfg);

    // The settled portion of the stream tail must appear as literal text
    // in the ASCII gutter (the last ~2 bytes are the tumbling hot tail, so
    // probe a mid-tail substring)…
    CHECK(row.find("JUMPED") != std::string::npos);
    // …and its hex must be present too ('J' = 4a, 'U' = 55, 'M' = 4d).
    CHECK(row.find("4a 55 4d") != std::string::npos);
}

TEST_CASE("activity tape: WRITE offset is the true total, counting up") {
    FrozenClock fc{100000};

    const std::string stream(64, 'x');
    maya::ActivityIndicator::Config cfg;
    cfg.stream = stream;

    cfg.stream_total = 0x40;                  // 64 bytes so far
    CHECK(render_row(cfg).find("0x000040") != std::string::npos);

    cfg.stream_total = 0x2100;                // later in the same stream
    CHECK(render_row(cfg).find("0x002100") != std::string::npos);
    // A countdown would DECREASE with progress; the odometer strictly
    // increases. Both totals printed verbatim proves the column is data.
}

TEST_CASE("activity tape: arrival moves WRITE even with time frozen") {
    FrozenClock fc{100000};

    maya::ActivityIndicator::Config cfg;

    const std::string s1 = "alpha beta gamma delta epsilon";
    cfg.stream       = s1;
    cfg.stream_total = s1.size();
    const std::string r1 = render_row(cfg);

    // Same frozen instant, three more bytes "arrive".
    const std::string s2 = s1 + " ze";
    cfg.stream       = s2;
    cfg.stream_total = s2.size();
    const std::string r2 = render_row(cfg);

    // Data is the motion source: the row must differ with zero wall-clock
    // movement.
    CHECK(r1 != r2);

    // And identical input at identical frozen time renders identically —
    // no hidden nondeterminism in the write path.
    CHECK(render_row(cfg) == render_row(cfg));
}

TEST_CASE("activity tape: READ scans the real context, head advances") {
    const std::string prompt =
        "please explain the borrow checker in extremely simple terms";

    maya::ActivityIndicator::Config cfg;
    cfg.context = prompt;

    // Freeze at a time whose read head sits just past "extremely"
    // (bytes 37-45): head = 46, so the trailing window
    // [head-cols+1, head] contains the whole word.
    //
    // The head PING-PONGS rather than wrapping modulo the length: it walks
    // 0 -> len-1 -> 0 over a (2*len - 2) cycle, so the scan never
    // teleports from the last byte back to the first. With len = 59 the
    // cycle is 116 steps of 140 ms; head = 46 at t = 46*140 = 6440.
    {
        FrozenClock fc{6440};
        const std::string row = render_row(cfg);
        // The gutter shows a REAL window of the prompt around the head.
        CHECK(row.find("extremely") != std::string::npos);
        // Offset is the head's true position (0x00002e = 46).
        CHECK(row.find("0x00002e") != std::string::npos);
    }

    // Advance the clock by exactly 10 read steps: the head must move 10
    // bytes forward (input-side liveness — this is what makes READ a scan,
    // not a still). 46 + 10 = 56, still on the outbound leg (< 59).
    {
        FrozenClock fc{6440 + 10 * 140};
        const std::string row = render_row(cfg);
        CHECK(row.find("0x000038") != std::string::npos);   // 56 = 0x38
    }

    // The scan REVERSES at the end instead of snapping back to 0. That
    // discontinuity — last byte, then suddenly byte 0 with a window of
    // unrelated text — is what read as the indicator "resetting".
    //
    // len = 59, cycle = 116. At step 59 the ping-pong is one byte into the
    // return leg: head = 116 - 59 = 57 (0x39). The old modulo scan would
    // have given head = 0 here.
    {
        FrozenClock fc{59 * 140};
        const std::string row = render_row(cfg);
        CHECK(row.find("0x000039") != std::string::npos);   // 57, walking back
        CHECK(row.find("0x000000") == std::string::npos);   // NOT a reset to 0
    }
}

TEST_CASE("activity tape: STATIC is pinned at zero and modes differ") {
    FrozenClock fc{100000};

    maya::ActivityIndicator::Config st;          // both sources empty
    const std::string rs = render_row(st);
    CHECK(rs.find("0x000000") != std::string::npos);   // no stream, no lie

    const std::string prompt = "what is the meaning of life";
    maya::ActivityIndicator::Config rd;
    rd.context = prompt;
    const std::string rr = render_row(rd);

    const std::string out = "the meaning of life is 42, obviously";
    maya::ActivityIndicator::Config wr;
    wr.stream       = out;
    wr.stream_total = out.size();
    const std::string rw = render_row(wr);

    // Pairwise distinguishable at a glance.
    CHECK(rs != rr);
    CHECK(rs != rw);
    CHECK(rr != rw);
}

TEST_CASE("activity tape: narrow widths never wrap the row") {
    FrozenClock fc{100000};

    const std::string stream = "narrow-width honesty check";
    const std::string prompt = "some real prompt to scan through";
    for (int w : {30, 44, 60, 80, 120}) {
        for (int mode = 0; mode < 3; ++mode) {
            maya::ActivityIndicator::Config cfg;
            if (mode == 0) {
                cfg.stream       = stream;
                cfg.stream_total = stream.size();
            } else if (mode == 1) {
                cfg.context = prompt;
            }
            maya::ActivityIndicator wid{cfg};
            const std::string out =
                maya::render_to_string(wid.build(), w);
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
}
