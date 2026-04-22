#include "moha/diff/diff.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace moha::diff {

namespace {
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

// Myers-style LCS-based diff, good enough for hunking.
struct Edit { enum K { Keep, Del, Ins } k; int a_idx, b_idx; };

std::vector<Edit> compute_edits(const std::vector<std::string>& a,
                                const std::vector<std::string>& b) {
    int n = (int)a.size(), m = (int)b.size();
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (int i = 1; i <= n; ++i)
        for (int j = 1; j <= m; ++j)
            dp[i][j] = (a[i-1] == b[j-1]) ? dp[i-1][j-1] + 1
                                          : std::max(dp[i-1][j], dp[i][j-1]);
    std::vector<Edit> edits;
    int i = n, j = m;
    while (i > 0 && j > 0) {
        if (a[i-1] == b[j-1]) { edits.push_back({Edit::Keep, i-1, j-1}); --i; --j; }
        else if (dp[i-1][j] >= dp[i][j-1]) { edits.push_back({Edit::Del, i-1, -1}); --i; }
        else { edits.push_back({Edit::Ins, -1, j-1}); --j; }
    }
    while (i > 0) { edits.push_back({Edit::Del, --i, -1}); }
    while (j > 0) { edits.push_back({Edit::Ins, -1, --j}); }
    std::reverse(edits.begin(), edits.end());
    return edits;
}
} // namespace

FileChange compute(const std::string& path,
                   const std::string& before,
                   const std::string& after) {
    FileChange c;
    c.path = path;
    c.original_contents = before;
    c.new_contents = after;

    auto a = split_lines(before);
    auto b = split_lines(after);
    auto edits = compute_edits(a, b);

    const int ctx = 3;
    int added = 0, removed = 0;
    std::vector<bool> is_change(edits.size(), false);
    for (size_t k = 0; k < edits.size(); ++k)
        if (edits[k].k != Edit::Keep) is_change[k] = true;

    size_t k = 0;
    while (k < edits.size()) {
        while (k < edits.size() && !is_change[k]) ++k;
        if (k >= edits.size()) break;
        size_t start = (k > (size_t)ctx) ? k - ctx : 0;
        size_t end = k;
        while (end < edits.size()) {
            size_t last_change = end;
            size_t probe = end;
            size_t gap = 0;
            while (probe < edits.size() && gap <= (size_t)(2 * ctx)) {
                if (is_change[probe]) { last_change = probe; gap = 0; }
                else gap++;
                probe++;
            }
            if (last_change == end) break;
            end = last_change;
        }
        end = std::min(edits.size() - 1, end + ctx);

        Hunk h;
        int old_start = -1, new_start = -1;
        int old_len = 0, new_len = 0;
        std::ostringstream patch;
        for (size_t i2 = start; i2 <= end; ++i2) {
            const auto& e = edits[i2];
            if (e.k == Edit::Keep) {
                if (old_start < 0) old_start = e.a_idx + 1;
                if (new_start < 0) new_start = e.b_idx + 1;
                old_len++; new_len++;
                patch << " " << a[e.a_idx] << "\n";
            } else if (e.k == Edit::Del) {
                if (old_start < 0) old_start = e.a_idx + 1;
                old_len++;
                patch << "-" << a[e.a_idx] << "\n";
                removed++;
            } else {
                if (new_start < 0) new_start = e.b_idx + 1;
                new_len++;
                patch << "+" << b[e.b_idx] << "\n";
                added++;
            }
        }
        h.old_start = std::max(1, old_start);
        h.new_start = std::max(1, new_start);
        h.old_len = old_len;
        h.new_len = new_len;
        h.patch = patch.str();
        c.hunks.push_back(std::move(h));
        k = end + 1;
    }

    c.added = added;
    c.removed = removed;
    return c;
}

std::string render_unified(const FileChange& c) {
    std::ostringstream oss;
    oss << "--- a/" << c.path << "\n";
    oss << "+++ b/" << c.path << "\n";
    for (const auto& h : c.hunks) {
        oss << "@@ -" << h.old_start << "," << h.old_len
            << " +" << h.new_start << "," << h.new_len << " @@\n";
        oss << h.patch;
    }
    return oss.str();
}

std::string apply_accepted(const FileChange& c) {
    // If all hunks accepted -> return new_contents. Otherwise rebuild by
    // iterating hunks and choosing which side to emit.
    bool all_accepted = !c.hunks.empty() &&
        std::all_of(c.hunks.begin(), c.hunks.end(),
                    [](const Hunk& h){ return h.status == Hunk::Status::Accepted; });
    if (all_accepted) return c.new_contents;
    bool none_accepted = std::all_of(c.hunks.begin(), c.hunks.end(),
        [](const Hunk& h){ return h.status != Hunk::Status::Accepted; });
    if (none_accepted) return c.original_contents;

    // Partial acceptance: reconstruct from original, applying only accepted hunks.
    auto orig_lines = split_lines(c.original_contents);
    std::vector<std::string> out;
    size_t orig_cursor = 0;
    for (const auto& h : c.hunks) {
        size_t h_start = (size_t)std::max(0, h.old_start - 1);
        while (orig_cursor < h_start && orig_cursor < orig_lines.size())
            out.push_back(orig_lines[orig_cursor++]);
        if (h.status == Hunk::Status::Accepted) {
            std::istringstream iss(h.patch);
            std::string line;
            while (std::getline(iss, line)) {
                if (line.empty()) continue;
                char tag = line[0];
                std::string body = line.substr(1);
                if (tag == ' ' || tag == '+') out.push_back(body);
            }
            orig_cursor = h_start + (size_t)h.old_len;
        } else {
            size_t take = (size_t)h.old_len;
            for (size_t i = 0; i < take && orig_cursor < orig_lines.size(); ++i)
                out.push_back(orig_lines[orig_cursor++]);
        }
    }
    while (orig_cursor < orig_lines.size())
        out.push_back(orig_lines[orig_cursor++]);

    std::string joined;
    for (size_t i = 0; i < out.size(); ++i) {
        joined += out[i];
        if (i + 1 < out.size()) joined += "\n";
    }
    return joined;
}

} // namespace moha::diff
