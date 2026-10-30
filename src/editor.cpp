#include "editor.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "motion.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <numbers>
#include <span>
#include <tuple>

namespace simple_notepad
{
    constexpr float PAD = 12.0f;
    constexpr float LINE_SPACING = 1.35f;
    constexpr float MINIMAP_MIN_WIDTH = 40.0f;
    constexpr float MINIMAP_MIN_TEXT_WIDTH = 120.0f; // room the text keeps when the minimap is dragged wide
    constexpr float MINIMAP_GRIP = 5.0f;
    constexpr float MINIMAP_MIN_EDITOR_WIDTH = 520.0f;
    constexpr float MINIMAP_ROW = 2.0f;
    constexpr float MINIMAP_CHAR = 1.1f;
    constexpr float SCROLL_SPEED = 22.0f;
    // neovide-style caret: each corner springs separately, leading corners snap and trailing ones lag by rank
    constexpr float CARET_SECONDS = 0.125f;
    constexpr float CARET_SHORT_SECONDS = 0.05f; // typing and short hops along a row
    constexpr float CARET_SHORT_CHARS = 8.0f;
    constexpr float CARET_SNAP_SECONDS = 0.02f;
    constexpr float CARET_LEADING = 0.5f; // alignment with the move that makes a corner lead
    constexpr std::array<float, 4> CARET_TRAIL = {1.0f, 0.9f, 0.5f, 0.3f}; // by rank, most trailing first
    // each corner's place in the caret box, -0.5 to 0.5, clockwise from top left
    constexpr std::array<std::pair<float, float>, 4> CARET_CORNERS = {
        {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}}};
    constexpr float CARET_RESET_SECONDS = 0.075f; // slower springs drop their old momentum on a new move
    constexpr float TEARDROP_SPEED = 14.0f;
    constexpr double BLINK_HALF_PERIOD = 0.5;
    constexpr double MERGE_SECONDS = 1.0;
    constexpr size_t MAX_UNDO = 1000;
    constexpr double FADE_SECONDS = 0.22;
    constexpr float FADE_RISE = 4.0f; // new characters float up into place by this many pixels
    constexpr size_t MAX_FADE_BYTES = 4096;
    constexpr float PIXEL_SNAP = 0.5f;
    constexpr int MAX_ROW_BYTES = 10000;                      // unwrapped lines break here too, like windows notepad
    constexpr int NO_LIMIT = std::numeric_limits<int>::max(); // max_end for a row free to run to its line end
    constexpr size_t MAX_REFLOW_ROWS = 64;                    // rows an edit rewraps before it settles for a short one
    constexpr ImU32 MATCH_COLOR = IM_COL32(234, 92, 0, 85);   // vs code's find highlight
    constexpr ImU32 MATCH_MARK_COLOR = IM_COL32(209, 134, 22, 255);
    constexpr float SELECT_SPEED = 30.0f;
    constexpr double FLASH_SECONDS = 0.6;
    constexpr int FLASH_ALPHA = 60;
    constexpr ImU32 WORD_COLOR = IM_COL32(255, 255, 255, 22);
    constexpr ImU32 WORD_MARK_COLOR = IM_COL32(200, 200, 200, 170);
    constexpr int MAX_WORD_BYTES = 200;
    constexpr size_t MAX_WORD_SCAN_BYTES =
        size_t{16} * 1024 * 1024;                 // past this every selection change would stall on a scan
    constexpr int MAX_BRACKET_SCAN = 1024 * 1024; // bytes searched for a partner before giving up
    constexpr double BRACKET_PULSE_SECONDS = 0.45;
    constexpr ImU32 BRACKET_COLOR = IM_COL32(255, 255, 255, 70);
    constexpr int MAX_HIGHLIGHT_BYTES = 200 * 1024 * 1024; // notepad++'s default large file restriction
    // vs code dark modern, indexed by syntax::token
    constexpr std::array<ImU32, 8> TOKEN_COLORS = {
        IM_COL32(106, 153, 85, 255),  // comment
        IM_COL32(206, 145, 120, 255), // string
        IM_COL32(181, 206, 168, 255), // number
        IM_COL32(86, 156, 214, 255),  // keyword
        IM_COL32(220, 220, 170, 255), // function
        IM_COL32(86, 156, 214, 255),  // heading
        IM_COL32(197, 134, 192, 255), // emphasis
        IM_COL32(78, 201, 176, 255),  // link
    };

    // 0 space, 1 word, 2 punctuation, 3 newline
    static int char_class(char c)
    {
        const auto u = static_cast<unsigned char>(c);
        if (c == '\n')
        {
            return 3;
        }
        if (c == ' ' || c == '\t')
        {
            return 0;
        }
        return (std::isalnum(u) != 0 || c == '_' || u >= 0x80) ? 1 : 2;
    }

    static int utf8_length(std::string_view s)
    {
        return static_cast<int>(std::ranges::count_if(s, [](char c) { return (c & 0xC0) != 0x80; }));
    }

    // ---------------------------------------------------------------- public

    void editor::set_text(std::string text, const syntax::language* lang, std::vector<syntax::span> spans)
    {
        text_ = gap_buffer<std::string>(std::move(text));
        language_ = lang;
        spans_ = syntax::span_buffer(std::move(spans));
        spans_dirty_ = false;
        edited_ = {.start = -1, .old_end = 0, .new_end = 0};
        scanned_size_ = static_cast<int>(text_.size());
        ++revision_;
        ++content_revision_;
        column_pos_ = -1;
        fades_.clear();
        flashes_.clear();
        last_selection_ = {0, 0};
        matches_dirty_ = true;
        undo_.clear();
        redo_.clear();
        caret_ = anchor_ = 0;
        scroll_x_ = scroll_y_ = target_x_ = target_y_ = 0.0f;
        layout_dirty_ = true;
        snap_caret_ = true;
        focus_request_ = true;
    }

    std::vector<syntax::span> editor::scan(const syntax::language* lang, std::string_view text)
    {
        return lang != nullptr && std::cmp_less_equal(text.size(), MAX_HIGHLIGHT_BYTES) ? syntax::scan(*lang, text)
                                                                                        : std::vector<syntax::span>{};
    }

    void editor::replace_all(std::string text)
    {
        const int caret = caret_;
        apply(0, static_cast<int>(text_.size()), std::move(text), merge::none);
        caret_ = anchor_ = std::min(caret, static_cast<int>(text_.size()));
        focus_request_ = true;
    }

    void editor::insert(std::string_view text)
    {
        const auto [lo, hi] = selection();
        const std::uint64_t before = state();
        apply(lo, hi - lo, std::string(text), merge::none);
        if (state() != before) // apply refuses edits when read-only or too large
        {
            flash(lo, lo + static_cast<int>(text.size()));
        }
    }

    void editor::select(int start, int end)
    {
        const int size = static_cast<int>(text_.size());
        anchor_ = std::clamp(start, 0, size);
        caret_ = std::clamp(end, 0, size);
        preferred_x_ = -1.0f;
        scroll_to_caret_ = true;
        caret_moved_ = ImGui::GetTime();
    }

    void editor::focus()
    {
        focus_request_ = true;
    }

    std::pair<int, int> editor::selection() const
    {
        return {std::min(caret_, anchor_), std::max(caret_, anchor_)};
    }

    int editor::line() const
    {
        return 1 + rows_[row_of(caret_)].line;
    }

    int editor::column() const
    {
        if (column_pos_ >= 0)
        {
            const std::string_view between = text_.view(std::min(column_pos_, caret_), std::max(column_pos_, caret_));
            if (between.find('\n') == std::string_view::npos)
            {
                column_chars_ += caret_ >= column_pos_ ? utf8_length(between) : -utf8_length(between);
                column_pos_ = caret_;
                return 1 + column_chars_;
            }
        }
        column_chars_ = utf8_length(text_.view(line_start(caret_), caret_));
        column_pos_ = caret_;
        return 1 + column_chars_;
    }

    void editor::set_search(std::string_view needle, bool match_case)
    {
        if (needle != needle_ || match_case != needle_case_)
        {
            needle_ = needle;
            needle_case_ = match_case;
            matches_dirty_ = true;
        }
    }

    void editor::set_language(const syntax::language* lang)
    {
        if (lang != language_)
        {
            language_ = lang;
            spans_dirty_ = true;
        }
    }

    bool editor::same_char(char a, char b) const
    {
        const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        return needle_case_ ? a == b : fold(a) == fold(b);
    }

    bool editor::selection_is_match() const
    {
        const auto [lo, hi] = selection();
        const auto equal = [this](char a, char b) { return same_char(a, b); };
        return !needle_.empty() && std::ranges::equal(text_.view(lo, hi), needle_, equal);
    }

    bool editor::contains_needle(std::string_view s) const
    {
        const auto equal = [this](char a, char b) { return same_char(a, b); };
        return !needle_.empty() && !std::ranges::search(s, needle_, equal).empty();
    }

    // each half searches only its own range, so the gap at the caret seldom has to move
    std::optional<size_t> editor::search(size_t from, bool backward) const
    {
        const size_t size = text_.size();
        const size_t n = needle_.size();
        from = std::min(from, size);
        const auto equal = [this](char a, char b) { return same_char(a, b); };
        const auto find_in = [&](size_t a, size_t b) -> std::optional<size_t> {
            const std::string_view range = text_.view(a, b);
            const auto hit =
                backward ? std::ranges::find_end(range, needle_, equal) : std::ranges::search(range, needle_, equal);
            return hit.empty() ? std::nullopt : std::optional(a + static_cast<size_t>(hit.begin() - range.begin()));
        };
        // wrapping around: no hit lies wholly on the searched side, so one ending past from is the only kind left
        if (backward)
        {
            const auto hit = find_in(0, from);
            return hit ? hit : find_in(from + 1 > n ? from + 1 - n : 0, size);
        }
        const auto hit = find_in(from, size);
        return hit ? hit : find_in(0, std::min(size, from + n - 1));
    }

    std::pair<int, int> editor::match_status()
    {
        if (matches_dirty_)
        {
            find_matches();
        }
        const auto [lo, hi] = selection();
        const auto it = std::ranges::lower_bound(matches_, lo, {}, &std::pair<int, int>::first);
        const bool on_match = it != matches_.end() && it->first == lo && it->second == hi;
        return {on_match ? static_cast<int>(it - matches_.begin()) : -1, static_cast<int>(matches_.size())};
    }

    std::uint64_t editor::state() const
    {
        return undo_.empty() ? 0 : undo_.back().id;
    }

    bool editor::animating() const
    {
        return !caret_settled_ || !select_settled_ || !fades_.empty() || !flashes_.empty() ||
               ImGui::GetTime() - bracket_time_ < BRACKET_PULSE_SECONDS || scroll_x_ != target_x_ ||
               scroll_y_ != target_y_ || (teardrop_ != 0.0f && teardrop_ != 1.0f);
    }

    double editor::seconds_until_blink() const
    {
        return BLINK_HALF_PERIOD - std::fmod(ImGui::GetTime() - caret_moved_, BLINK_HALF_PERIOD);
    }

    // ---------------------------------------------------------------- layout

    float editor::advance(unsigned int c, float x) const
    {
        if (c == '\t')
        {
            return ((std::floor(x / tab_width_) + 1.0f) * tab_width_) - x;
        }
        return c < 128 ? ascii_advance_[c] : baked_->GetCharAdvance(static_cast<ImWchar>(c));
    }

    float editor::measure(int start, int end) const
    {
        const char* s = text_.contiguous(start, end);
        // monospace printable ascii, most big data files, is a count; summing advances one by one is a serial chain
        // counted rather than all_of: no early exit lets the compiler check many bytes per instruction
        const auto unprintable = [](char c) { return static_cast<unsigned char>(c - ' ') > '~' - ' '; };
        if (mono_advance_ > 0.0f && std::count_if(s + start, s + end, unprintable) == 0)
        {
            return mono_advance_ * static_cast<float>(end - start);
        }
        float x = 0.0f;
        // raw loop: utf-8 decoding steps a variable number of bytes
        for (int p = start; p < end;)
        {
            const auto b = static_cast<unsigned char>(s[p]);
            if (b < 128 && b != '\t')
            {
                x += ascii_advance_[b]; // most text: skips the decoder, same result as advance()
                ++p;
                continue;
            }
            unsigned int c = 0;
            p += ImTextCharFromUtf8(&c, s + p, s + end);
            x += advance(c, x);
        }
        return x;
    }

    // huge unwrapped lines split into rows too, since float x loses whole pixels far out
    editor::row editor::next_row(int start, int line, bool first, int max_end) const
    {
        const int size = static_cast<int>(text_.size());
        const int reach = std::min({size, start + (2 * MAX_ROW_BYTES), max_end > start ? max_end : NO_LIMIT});
        const int window_end = std::min(size, reach + 3); // room to finish a character cut at the limit
        const char* s = text_.contiguous(start, window_end);
        row r{.start = start, .end = reach, .line = line, .first = first, .width = 0.0f};
        if (layout_wrap_width_ < 0.0f)
        {
            const auto* newline = static_cast<const char*>(std::memchr(s + start, '\n', reach - start));
            // a row takes up to two rows' worth before splitting, so typing into a full row never leaves a sliver
            const int limit = reach < start + (2 * MAX_ROW_BYTES) ? reach : start + MAX_ROW_BYTES;
            r.end = newline != nullptr ? static_cast<int>(newline - s) : limit;
            while (newline == nullptr && r.end < window_end && (s[r.end] & 0xC0) == 0x80) // whole characters only
            {
                ++r.end;
            }
            r.width = measure(start, r.end) / layout_size_;
            return r;
        }

        // wrapped rows end long before reach, so the scans below find the newline themselves
        if (mono_advance_ > 0.0f)
        {
            // monospace ascii: what fits is a count, so only the bytes need checking
            const int stop = std::min(reach, start + std::max(1, static_cast<int>(layout_wrap_width_ / mono_advance_)));
            int last_space = -1;
            int p = start;
            // raw loop: stops at the first byte that needs the full measure below
            for (; p < stop; ++p)
            {
                const char c = s[p];
                if (c == '\n')
                {
                    r.end = p;
                    return r;
                }
                if (c < ' ' || c > '~')
                {
                    break;
                }
                last_space = c == ' ' ? p : last_space;
            }
            if (p == stop && (stop == reach || s[stop] == '\n'))
            {
                r.end = stop;
                return r;
            }
            if (p == stop && s[stop] >= ' ' && s[stop] <= '~')
            {
                r.end = last_space >= start ? last_space + 1 : stop; // the overflowing character starts the next row
                return r;
            }
        }
        int last_break = -1;
        float x = 0.0f;
        int p = start;
        // raw loop: greedily breaks the row at the last space that fits
        while (p < reach)
        {
            unsigned int c = static_cast<unsigned char>(s[p]);
            if (c == '\n')
            {
                break;
            }
            const int len = c < 128 ? 1 : ImTextCharFromUtf8(&c, s + p, s + window_end); // ascii skips the decoder
            const float w = advance(c, x);
            if (x + w > layout_wrap_width_ && p > start)
            {
                r.end = last_break > start ? last_break : p;
                return r;
            }
            x += w;
            if (c == ' ' || c == '\t')
            {
                last_break = p + len;
            }
            p += len;
        }
        r.end = p;
        return r;
    }

    editor::row editor::row_after(const row& r, int max_end) const
    {
        const bool first = text_[r.end] == '\n';
        return next_row(first ? r.end + 1 : r.end, first ? r.line + 1 : r.line, first, max_end);
    }

    void editor::rebuild_layout(float wrap_width)
    {
        layout_dirty_ = false;
        layout_wrap_width_ = wrap_width;
        ++revision_;
        const int size = static_cast<int>(text_.size());
        std::vector<row> rows{next_row(0, 0, true, NO_LIMIT)};
        while (rows.back().end < size)
        {
            rows.push_back(row_after(rows.back(), NO_LIMIT));
        }
        line_count_ = rows.back().line + 1;
        rows_ = decltype(rows_)(std::move(rows));
        char_count_ = utf8_length(text_.view(0, text_.size()));
        update_content_width();
    }

    // edits relayout only the rows they touch, until a row starts where an old one did; the rest shift in one step
    void editor::replace_text(int pos, int remove, std::string_view insert)
    {
        ++revision_;
        ++content_revision_;
        const int delta = static_cast<int>(insert.size()) - remove;
        const int new_size = static_cast<int>(text_.size()) + delta;
        if (std::max(static_cast<int>(text_.size()), new_size) > MAX_HIGHLIGHT_BYTES)
        {
            spans_dirty_ = true; // crossing the limit either way needs a full pass
        }
        // untouched bytes between two edits join the range too, and a line break among them can't be tracked
        edited_.joined =
            edited_.joined || (edited_.start >= 0 && (pos > edited_.new_end || pos + remove < edited_.start));
        // widen the range edited since the last scan; bytes after it only shifted
        const int inserted_end = pos + static_cast<int>(insert.size());
        edited_.new_end = edited_.start < 0 || edited_.new_end < pos + remove
                              ? inserted_end
                              : edited_.new_end + inserted_end - (pos + remove);
        edited_.start = edited_.start < 0 ? pos : std::min(edited_.start, pos);
        edited_.old_end = edited_.new_end - (new_size - scanned_size_);
        shift_ranges(fades_, pos, remove, delta);
        shift_ranges(flashes_, pos, remove, delta);
        const std::string_view removed = text_.view(pos, pos + remove);
        edited_.joined = edited_.joined || removed.find('\n') != std::string_view::npos;
        char_count_ += utf8_length(insert) - utf8_length(removed);
        // the counted column holds through edits after it and follows ones that end at it on its line
        if (column_pos_ > pos)
        {
            const bool follows = pos + remove == column_pos_ && removed.find('\n') == std::string_view::npos &&
                                 insert.find('\n') == std::string_view::npos;
            column_chars_ += follows ? utf8_length(insert) - utf8_length(removed) : 0;
            column_pos_ = follows ? column_pos_ + delta : -1;
        }
        if (layout_dirty_)
        {
            text_.replace(pos, remove, insert);
            update_matches(pos, remove, static_cast<int>(insert.size()));
            return;
        }

        const bool unwrapped = layout_wrap_width_ < 0.0f;
        int r0 = row_of(pos);
        // greedy wrap: a shortened word may now fit one row up; rarer ripples further back wait for a full layout
        if (!unwrapped && r0 > 0 && !rows_[r0].first)
        {
            --r0;
        }
        // unwrapped rows don't depend on each other, so new ones stop where the last edited row ended
        const int max_end = unwrapped ? rows_[row_of(pos + remove)].end + delta : NO_LIMIT;
        const row old = rows_[r0];
        text_.replace(pos, remove, insert);
        update_matches(pos, remove, static_cast<int>(insert.size()));

        const int size = static_cast<int>(text_.size());
        std::vector<row> fresh{next_row(old.start, old.line, old.first, max_end)};
        size_t sync = rows_.size(); // first old row that still holds, once shifted
        int line_delta = 0;
        // raw loop: lays out rows until one starts past the edit where an old row did
        while (fresh.back().end < size)
        {
            int cap = max_end;
            if (std::cmp_greater_equal(fresh.size(), MAX_REFLOW_ROWS) && fresh.back().end >= inserted_end)
            {
                // a wrapped line with no spaces would reflow to its end: one row ends short at an old break instead
                const int was = fresh.back().end - delta;
                const size_t k = rows_.partition_point([&](const row& r) { return r.start <= was; });
                cap = k < rows_.size() ? rows_[k].start + delta : NO_LIMIT;
            }
            const row next = row_after(fresh.back(), cap);
            if (next.start >= inserted_end)
            {
                const int was = next.start - delta;
                const size_t k = rows_.partition_point([&](const row& r) { return r.start < was; });
                if (k < rows_.size() && rows_[k].start == was && rows_[k].first == next.first)
                {
                    sync = k;
                    line_delta = next.line - rows_[k].line;
                    break;
                }
            }
            fresh.push_back(next);
        }
        if (sync == rows_.size())
        {
            line_delta = fresh.back().line + 1 - line_count_;
        }
        float removed_width = 0.0f;
        for (size_t r = r0; unwrapped && r < sync; ++r) // raw loop: the gap buffer has no iterators
        {
            removed_width = std::max(removed_width, rows_[r].width * layout_size_);
        }
        const float fresh_width = unwrapped ? std::ranges::max(fresh, {}, &row::width).width * layout_size_ : 0.0f;
        rows_.replace(r0, sync - r0, fresh);
        rows_.shift_tail({.bytes = delta, .lines = line_delta});
        line_count_ += line_delta;
        // only a full rescan finds the next widest row once the widest one shrinks
        if (!unwrapped || fresh_width >= content_width_)
        {
            content_width_ = unwrapped ? fresh_width : layout_wrap_width_;
        }
        else if (removed_width >= content_width_)
        {
            update_content_width();
        }
    }

    void editor::find_matches()
    {
        matches_dirty_ = false;
        matches_.clear();
        matches_capped_ = false;
        // raw loop: search() wraps around, so stop once a hit lands before the cursor
        for (size_t from = 0;;)
        {
            const auto hit = search(from, false);
            if (!hit || *hit < from)
            {
                break;
            }
            if (std::cmp_equal(matches_.size(), MAX_MATCHES))
            {
                matches_capped_ = true; // found one more than it keeps
                break;
            }
            matches_.emplace_back(static_cast<int>(*hit), static_cast<int>(*hit + needle_.size()));
            from = *hit + needle_.size();
        }
    }

    // known limit: with a self-overlapping needle ("aa") this can keep another valid set than a full search
    void editor::update_matches(int pos, int remove, int inserted)
    {
        if (matches_dirty_ || needle_.empty())
        {
            return;
        }
        if (matches_capped_)
        {
            matches_dirty_ = true; // the edit moves where the cap cuts off
            return;
        }
        const int n = static_cast<int>(needle_.size());
        const int delta = inserted - remove;
        // matches starting from n - 1 bytes before the edit up to its end overlap the edited bytes
        const auto first = std::ranges::lower_bound(matches_, pos - n + 1, {}, &std::pair<int, int>::first);
        const auto last =
            std::ranges::lower_bound(first, matches_.end(), pos + remove, {}, &std::pair<int, int>::first);
        std::for_each(last, matches_.end(), [&](std::pair<int, int>& m) {
            m.first += delta;
            m.second += delta;
        });
        const int from = std::max(pos - n + 1, first == matches_.begin() ? 0 : std::prev(first)->second);
        const int to =
            std::min(last == matches_.end() ? static_cast<int>(text_.size()) : last->first, pos + inserted + n - 1);
        const std::string_view window = from < to ? text_.view(from, to) : std::string_view{};
        const auto equal = [this](char a, char b) { return same_char(a, b); };
        std::vector<std::pair<int, int>> found;
        // raw loop: each hit resumes the search after itself
        for (size_t at = 0;;)
        {
            const std::string_view rest = window.substr(at);
            const auto hit = std::ranges::search(rest, needle_, equal);
            if (hit.empty())
            {
                break;
            }
            const int start = from + static_cast<int>(at + (hit.begin() - rest.begin()));
            found.emplace_back(start, start + n);
            at = static_cast<size_t>(start + n - from);
        }
        matches_.insert(matches_.erase(first, last), found.begin(), found.end());
        if (std::cmp_greater(matches_.size(), MAX_MATCHES))
        {
            matches_.resize(MAX_MATCHES);
            matches_capped_ = true;
        }
    }

    void editor::update_content_width()
    {
        content_width_ = layout_wrap_width_;
        if (layout_wrap_width_ < 0.0f)
        {
            content_width_ = 0.0f;
            for (size_t r = 0; r < rows_.size(); ++r) // raw loop: the gap buffer has no iterators
            {
                content_width_ = std::max(content_width_, rows_[r].width * layout_size_);
            }
        }
    }

    int editor::row_of(int pos) const
    {
        const size_t after = rows_.partition_point([&](const row& r) { return r.start <= pos; });
        return std::max(0, static_cast<int>(after) - 1);
    }

    float editor::x_of(int pos) const
    {
        return measure(rows_[row_of(pos)].start, pos);
    }

    int editor::pos_at(int row_index, float x) const
    {
        const row r = rows_[std::clamp(row_index, 0, static_cast<int>(rows_.size()) - 1)];
        const char* s = text_.contiguous(r.start, r.end);
        float cx = 0.0f;
        // raw loop: utf-8 decoding steps a variable number of bytes
        for (int p = r.start; p < r.end;)
        {
            unsigned int c = 0;
            const int len = ImTextCharFromUtf8(&c, s + p, s + r.end);
            const float w = advance(c, cx);
            if (cx + (w * 0.5f) > x)
            {
                return p;
            }
            cx += w;
            p += len;
        }
        return r.end;
    }

    // ---------------------------------------------------------------- navigation

    int editor::next_char(int pos) const
    {
        const int size = static_cast<int>(text_.size());
        if (pos >= size)
        {
            return size;
        }
        ++pos;
        while (pos < size && (text_[pos] & 0xC0) == 0x80)
        {
            ++pos;
        }
        return pos;
    }

    int editor::prev_char(int pos) const
    {
        if (pos <= 0)
        {
            return 0;
        }
        --pos;
        while (pos > 0 && (text_[pos] & 0xC0) == 0x80)
        {
            --pos;
        }
        return pos;
    }

    int editor::word_edge(int pos, bool forward) const
    {
        const int size = static_cast<int>(text_.size());
        if (forward)
        {
            if (pos >= size)
            {
                return size;
            }
            if (text_[pos] == '\n')
            {
                return pos + 1;
            }
            while (pos < size && char_class(text_[pos]) == 0)
            {
                ++pos;
            }
            if (pos < size && text_[pos] != '\n')
            {
                const int k = char_class(text_[pos]);
                while (pos < size && char_class(text_[pos]) == k)
                {
                    ++pos;
                }
            }
            return pos;
        }
        if (pos <= 0)
        {
            return 0;
        }
        if (text_[pos - 1] == '\n')
        {
            return pos - 1;
        }
        while (pos > 0 && char_class(text_[pos - 1]) == 0)
        {
            --pos;
        }
        if (pos > 0 && text_[pos - 1] != '\n')
        {
            const int k = char_class(text_[pos - 1]);
            while (pos > 0 && char_class(text_[pos - 1]) == k)
            {
                --pos;
            }
        }
        return pos;
    }

    std::pair<int, int> editor::word_at(int pos) const
    {
        const int size = static_cast<int>(text_.size());
        if (pos >= size || text_[pos] == '\n')
        {
            return {pos, pos};
        }
        const int k = char_class(text_[pos]);
        int a = pos;
        int b = pos;
        while (a > 0 && char_class(text_[a - 1]) == k)
        {
            --a;
        }
        while (b < size && char_class(text_[b]) == k)
        {
            ++b;
        }
        return {a, b};
    }

    std::pair<int, int> editor::line_at(int pos) const
    {
        const int end = line_end(pos);
        return {line_start(pos), std::cmp_less(end, text_.size()) ? end + 1 : end};
    }

    // the rows already know where lines break, so no byte scan is needed
    int editor::line_start(int pos) const
    {
        int r = row_of(pos);
        while (r > 0 && !rows_[r].first)
        {
            --r;
        }
        return rows_[r].start;
    }

    int editor::line_end(int pos) const
    {
        int r = row_of(pos);
        while (r + 1 < static_cast<int>(rows_.size()) && !rows_[r + 1].first)
        {
            ++r;
        }
        return rows_[r].end;
    }

    // ---------------------------------------------------------------- editing

    void editor::apply(int pos, int remove, std::string insert, merge kind)
    {
        if (read_only_)
        {
            return;
        }
        if (text_.size() - remove + insert.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return; // offsets are int: refusing the edit beats corrupting the text
        }
        const double now = ImGui::GetTime();
        const std::uint64_t id = next_id_++;
        const int inserted = static_cast<int>(insert.size());
        std::string removed(text_.view(pos, pos + remove));
        replace_text(pos, remove, insert);
        bool merged = false;
        if (kind != merge::none && !undo_.empty())
        {
            edit& last = undo_.back();
            const bool recent = last.kind == kind && now - last.time < MERGE_SECONDS;
            // typing groups per word; deleting groups runs of backspace or delete
            if (recent && kind == merge::typing && remove == 0 &&
                last.pos + static_cast<int>(last.inserted.size()) == pos && !last.inserted.empty() &&
                (last.inserted.back() != ' ' || insert == " "))
            {
                last.inserted += insert;
                merged = true;
            }
            else if (recent && kind == merge::deleting && insert.empty() && last.inserted.empty() &&
                     (pos + remove == last.pos || pos == last.pos))
            {
                last.removed = pos == last.pos ? last.removed + removed : removed + last.removed;
                last.pos = pos;
                merged = true;
            }
            if (merged)
            {
                last.time = now;
                last.id = id;
            }
        }
        if (!merged)
        {
            undo_.push_back({pos, std::move(removed), std::move(insert), caret_, anchor_, kind, now, id});
            if (undo_.size() > MAX_UNDO)
            {
                undo_.pop_front();
            }
        }
        redo_.clear();
        move_caret(pos + inserted, false);
        if (inserted > 0 && std::cmp_less_equal(inserted, MAX_FADE_BYTES))
        {
            fades_.push_back({.start = pos, .end = pos + inserted, .time = now});
        }
    }

    void editor::undo()
    {
        if (read_only_ || undo_.empty())
        {
            return;
        }
        edit e = std::move(undo_.back());
        undo_.pop_back();
        replace_text(e.pos, static_cast<int>(e.inserted.size()), e.removed);
        flash(e.pos, e.pos + static_cast<int>(e.removed.size()));
        move_caret(e.caret_before, false);
        anchor_ = e.anchor_before;
        redo_.push_back(std::move(e));
    }

    void editor::redo()
    {
        if (read_only_ || redo_.empty())
        {
            return;
        }
        edit e = std::move(redo_.back());
        redo_.pop_back();
        replace_text(e.pos, static_cast<int>(e.removed.size()), e.inserted);
        flash(e.pos, e.pos + static_cast<int>(e.inserted.size()));
        move_caret(e.pos + static_cast<int>(e.inserted.size()), false);
        e.kind = merge::none; // never merge new typing into a redone step
        undo_.push_back(std::move(e));
    }

    void editor::move_caret(int pos, bool extend)
    {
        caret_ = std::clamp(pos, 0, static_cast<int>(text_.size()));
        if (!extend)
        {
            anchor_ = caret_;
        }
        preferred_x_ = -1.0f;
        scroll_to_caret_ = true;
        caret_moved_ = ImGui::GetTime();
    }

    void editor::erase_selection_or(int from, int to, merge kind)
    {
        const auto [lo, hi] = selection();
        if (lo != hi)
        {
            apply(lo, hi - lo, {}, merge::none);
        }
        else if (from < to)
        {
            apply(from, to - from, {}, kind);
        }
    }

    // ---------------------------------------------------------------- input

    void editor::handle_keyboard()
    {
        ImGuiIO& io = ImGui::GetIO();
        const bool ctrl = io.KeyCtrl;
        const bool shift = io.KeyShift;
        const auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
        const auto [lo, hi] = selection();

        for (const ImWchar c : io.InputQueueCharacters)
        {
            if (c < 32 || c == 127 || (ctrl && !io.KeyAlt))
            {
                continue;
            }
            std::array<char, 5> utf8{};
            const int len = ImTextCharToUtf8(utf8.data(), c);
            const auto [a, b] = selection();
            apply(a, b - a, std::string(utf8.data(), len), merge::typing);
        }
        io.InputQueueCharacters.resize(0);

        // vertical moves keep the column the caret started from
        const auto move_rows = [&](int delta) {
            if (preferred_x_ < 0.0f)
            {
                preferred_x_ = x_of(caret_);
            }
            const float keep = preferred_x_;
            const int target = row_of(caret_) + delta;
            const int last = static_cast<int>(rows_.size()) - 1;
            const int pos = target < 0 ? 0 : target > last ? static_cast<int>(text_.size()) : pos_at(target, keep);
            move_caret(pos, shift);
            preferred_x_ = keep;
        };
        const int page = std::max(1, static_cast<int>(view_height_ / line_height_) - 1);

        if (pressed(ImGuiKey_LeftArrow))
        {
            move_caret(!shift && lo != hi ? lo : ctrl ? word_edge(caret_, false) : prev_char(caret_), shift);
        }
        else if (pressed(ImGuiKey_RightArrow))
        {
            move_caret(!shift && lo != hi ? hi : ctrl ? word_edge(caret_, true) : next_char(caret_), shift);
        }
        else if (pressed(ImGuiKey_UpArrow))
        {
            move_rows(-1);
        }
        else if (pressed(ImGuiKey_DownArrow))
        {
            move_rows(1);
        }
        else if (pressed(ImGuiKey_PageUp))
        {
            move_rows(-page);
            target_y_ -= static_cast<float>(page) * line_height_;
        }
        else if (pressed(ImGuiKey_PageDown))
        {
            move_rows(page);
            target_y_ += static_cast<float>(page) * line_height_;
        }
        else if (pressed(ImGuiKey_Home))
        {
            if (ctrl)
            {
                move_caret(0, shift);
            }
            else
            {
                // smart home: first press goes to the indentation, the next to the row start
                const row r = rows_[row_of(caret_)];
                int indent = r.start;
                while (r.first && indent < r.end && (text_[indent] == ' ' || text_[indent] == '\t'))
                {
                    ++indent;
                }
                move_caret(caret_ == indent ? r.start : indent, shift);
            }
        }
        else if (pressed(ImGuiKey_End))
        {
            move_caret(ctrl ? static_cast<int>(text_.size()) : rows_[row_of(caret_)].end, shift);
        }
        else if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter))
        {
            apply(lo, hi - lo, "\n", merge::none);
        }
        else if (pressed(ImGuiKey_Tab))
        {
            apply(lo, hi - lo, "\t", merge::typing);
        }
        else if (pressed(ImGuiKey_Backspace))
        {
            erase_selection_or(ctrl ? word_edge(caret_, false) : prev_char(caret_), caret_, merge::deleting);
        }
        else if (pressed(ImGuiKey_Delete) && !shift)
        {
            erase_selection_or(caret_, ctrl ? word_edge(caret_, true) : next_char(caret_), merge::deleting);
        }
        else if (ctrl && pressed(ImGuiKey_A))
        {
            anchor_ = 0;
            caret_ = static_cast<int>(text_.size());
            caret_moved_ = ImGui::GetTime();
        }
        else if ((ctrl && (pressed(ImGuiKey_C) || pressed(ImGuiKey_X) || pressed(ImGuiKey_Insert))) ||
                 (shift && pressed(ImGuiKey_Delete)))
        {
            if (lo != hi)
            {
                std::string copied;
                // raw loop: expands line endings for other windows apps in one pass
                for (const char c : text_.view(lo, hi))
                {
                    if (c == '\n')
                    {
                        copied += '\r';
                    }
                    copied += c;
                }
                ImGui::SetClipboardText(copied.c_str());
                if (!ImGui::IsKeyPressed(ImGuiKey_C) && !ImGui::IsKeyPressed(ImGuiKey_Insert))
                {
                    apply(lo, hi - lo, {}, merge::none);
                }
            }
        }
        else if ((ctrl && pressed(ImGuiKey_V)) || (shift && pressed(ImGuiKey_Insert)))
        {
            if (const char* clip = ImGui::GetClipboardText())
            {
                std::string pasted = clip;
                std::erase(pasted, '\r');
                insert(pasted);
            }
        }
        else if (ctrl && (pressed(ImGuiKey_Y) || (shift && pressed(ImGuiKey_Z))))
        {
            redo();
        }
        else if (ctrl && pressed(ImGuiKey_Z))
        {
            undo();
        }
        else if (ctrl && pressed(ImGuiKey_D))
        {
            // duplicates every line the selection touches below itself and moves the selection onto the copy
            const int start = line_start(lo);
            const int end = line_end(hi);
            const int caret = caret_;
            const int anchor = anchor_;
            const int shift_by = end - start + 1;
            std::string copy(1, '\n');
            copy += text_.view(start, end);
            apply(end, 0, std::move(copy), merge::none);
            caret_ = caret + shift_by;
            anchor_ = anchor + shift_by;
        }
    }

    void editor::handle_mouse(float origin_x, float origin_y, bool hovered)
    {
        const ImGuiIO& io = ImGui::GetIO();
        const ImRect inner = ImGui::GetCurrentWindow()->InnerRect;
        const bool over_text = hovered && inner.Contains(io.MousePos);
        if (over_text)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        }

        const auto range_at_mouse = [&] {
            const int r = static_cast<int>(std::floor((io.MousePos.y - origin_y) / line_height_));
            const int last = static_cast<int>(rows_.size()) - 1;
            const int pos = r < 0 ? 0 : r > last ? static_cast<int>(text_.size()) : pos_at(r, io.MousePos.x - origin_x);
            switch (mode_)
            {
            case select_mode::words:
                return word_at(pos);
            case select_mode::lines:
                return line_at(pos);
            case select_mode::chars:
                break;
            }
            return std::pair{pos, pos};
        };

        if (over_text && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const int clicks = io.MouseClickedCount[ImGuiMouseButton_Left];
            mode_ = clicks >= 3 ? select_mode::lines : clicks == 2 ? select_mode::words : select_mode::chars;
            const auto range = range_at_mouse();
            if (io.KeyShift && clicks == 1)
            {
                drag_origin_ = {anchor_, anchor_};
                caret_ = range.second;
            }
            else
            {
                drag_origin_ = range;
                anchor_ = range.first;
                caret_ = range.second;
            }
            preferred_x_ = -1.0f;
            caret_moved_ = ImGui::GetTime();
            dragging_ = true;
        }
        if (!dragging_)
        {
            return;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            dragging_ = false;
            return;
        }
        const auto [a, b] = range_at_mouse();
        if (a < drag_origin_.first)
        {
            anchor_ = drag_origin_.second;
            caret_ = a;
        }
        else
        {
            anchor_ = drag_origin_.first;
            caret_ = b;
        }
        caret_moved_ = ImGui::GetTime();
        // dragging past the edges keeps scrolling toward the caret
        scroll_to_caret_ = !inner.Contains(io.MousePos);
    }

    // ---------------------------------------------------------------- drawing

    void editor::draw(const view& v)
    {
        const ImGuiIO& io = ImGui::GetIO();
        dpi_ = v.dpi;
        ImGui::PushFont(v.font, v.font_size);
        const float font_size = ImGui::GetFontSize();

        // zooming keeps the point under the mouse, or else the caret, at the same spot in the view.
        // measured with the old layout before it is rebuilt, then restored after
        int anchor = -1;
        float anchor_x = 0.0f; // view-space position the anchor should stay at
        float anchor_y = 0.0f;
        float anchor_frac = 0.0f; // how far down its row the mouse was
        // unwrapped rows don't reflow, so the exact point scales instead, even over empty space
        bool point_anchor = false;
        float anchor_row = 0.0f;
        float anchor_text_x = 0.0f;
        const float old_space = ascii_advance_[' '];
        if (layout_size_ > 0.0f && layout_size_ != font_size)
        {
            const float old_pad = PAD * dpi_;
            const bool over_text = io.MousePos.x >= view_left_ && io.MousePos.x < view_left_ + view_width_ &&
                                   io.MousePos.y >= top_ && io.MousePos.y < top_ + view_height_;
            if (over_text)
            {
                anchor_x = io.MousePos.x - view_left_;
                anchor_y = io.MousePos.y - top_;
                const float row_f = (anchor_y + scroll_y_ - old_pad) / line_height_;
                if (v.wrap)
                {
                    const int r =
                        std::clamp(static_cast<int>(std::floor(row_f)), 0, static_cast<int>(rows_.size()) - 1);
                    anchor = pos_at(r, anchor_x + scroll_x_ - old_pad);
                    anchor_frac = std::clamp(row_f - static_cast<float>(r), 0.0f, 1.0f);
                }
                else
                {
                    point_anchor = true;
                    anchor_row = row_f;
                    anchor_text_x = anchor_x + scroll_x_ - old_pad;
                }
            }
            else
            {
                anchor = caret_;
                anchor_x = x_of(caret_) + old_pad - scroll_x_;
                anchor_y = (static_cast<float>(row_of(caret_)) * line_height_) + old_pad - scroll_y_;
            }
            zoom_x_ = view_left_ + anchor_x;
            zoom_y_ = top_ + anchor_y;
        }
        zoom_scale_ = v.scale;

        line_height_ = std::round(font_size * LINE_SPACING);
        if (ImFontBaked* baked = ImGui::GetFontBaked(); baked != baked_ || layout_size_ != font_size)
        {
            baked_ = baked;
            layout_size_ = font_size;
            for (unsigned int c = 0; c < 128; ++c) // raw loop: fills a lookup table by index
            {
                ascii_advance_[c] = baked->GetCharAdvance(static_cast<ImWchar>(c));
            }
            tab_width_ = ascii_advance_[' '] * 4.0f;
            const auto [narrowest, widest] = std::minmax_element(&ascii_advance_[' '], &ascii_advance_['~'] + 1);
            mono_advance_ = *narrowest == *widest ? *narrowest : 0.0f;
            // unwrapped rows break at fixed byte counts and keep font-relative widths, so zoom only rescales
            if (layout_wrap_width_ < 0.0f && !layout_dirty_)
            {
                update_content_width();
            }
            else
            {
                layout_dirty_ = true;
            }
        }

        const float pad = PAD * dpi_;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float total_width = ImGui::GetContentRegionAvail().x;
        top_ = p0.y;
        height_ = v.height;

        const int digits = std::max(3, static_cast<int>(std::to_string(line_count_).size()));
        const float gutter_width = (static_cast<float>(digits) * ascii_advance_['0']) + (pad * 2.0f);
        const float scrollbar = ImGui::GetStyle().ScrollbarSize;
        const float minimap_scale = MINIMAP_CHAR * dpi_ / ascii_advance_[' '];
        const float minimap_margin = 8.0f * dpi_;
        float minimap_width = 0.0f;
        if (v.minimap && total_width > MINIMAP_MIN_EDITOR_WIDTH * dpi_)
        {
            if (v.wrap)
            {
                // sized to hold a full wrapped row, which itself depends on the minimap's width:
                // m = (total - gutter - padding - m) * scale + margin, solved for m
                const float avail = total_width - gutter_width - (pad * 2.0f) - scrollbar;
                minimap_width = ((avail * minimap_scale) + minimap_margin) / (1.0f + minimap_scale);
            }
            else
            {
                // dragging stops at the widest line, so the minimap never grows past what it can show
                const float fit = (content_width_ * minimap_scale) + minimap_margin;
                minimap_width =
                    std::clamp(minimap_width_ * dpi_, MINIMAP_MIN_WIDTH * dpi_,
                               std::max(MINIMAP_MIN_WIDTH * dpi_,
                                        std::min(std::max(fit, MINIMAP_WIDTH * dpi_),
                                                 total_width - gutter_width - (MINIMAP_MIN_TEXT_WIDTH * dpi_))));
            }
        }
        const float text_width = total_width - gutter_width - minimap_width;
        const float wrap_width =
            v.wrap ? std::max(text_width - (pad * 2.0f) - scrollbar, ascii_advance_['M'] * 8.0f) : -1.0f;
        // a window being dragged to a new size rewraps once, when the drag ends
        const bool held = v.hold_wrap && wrap_width >= 0.0f && layout_wrap_width_ >= 0.0f;
        if (layout_dirty_ || (wrap_width != layout_wrap_width_ && !held))
        {
            rebuild_layout(wrap_width);
        }
        if (point_anchor)
        {
            snap_caret_ = true; // its animated position is in old-zoom pixels
            scroll_y_ = target_y_ = (anchor_row * line_height_) + pad - anchor_y;
            scroll_x_ = target_x_ = (anchor_text_x * ascii_advance_[' '] / old_space) + pad - anchor_x;
        }
        else if (anchor >= 0)
        {
            snap_caret_ = true; // its animated position is in old-zoom pixels
            const float row_y = static_cast<float>(row_of(anchor)) + anchor_frac;
            scroll_y_ = target_y_ = (row_y * line_height_) + pad - anchor_y;
            if (!v.wrap)
            {
                scroll_x_ = target_x_ = x_of(anchor) + pad - anchor_x;
            }
        }

        // allow scrolling half a page past the end, like most code editors
        const auto content_size = [&] {
            return ImVec2(v.wrap ? 0.0f : content_width_ + (pad * 2.0f) + (ascii_advance_[' '] * 2.0f),
                          (static_cast<float>(rows_.size()) * line_height_) + (pad * 2.0f) + (v.height * 0.5f));
        };
        const auto clamp_targets = [&] {
            const ImVec2 content = content_size();
            target_x_ = std::clamp(target_x_, 0.0f, std::max(0.0f, content.x - view_width_));
            target_y_ = std::clamp(target_y_, 0.0f, std::max(0.0f, content.y - view_height_));
        };
        clamp_targets();
        scroll_x_ = v.wrap ? 0.0f : motion::approach(scroll_x_, target_x_, SCROLL_SPEED, PIXEL_SNAP);
        scroll_y_ = motion::approach(scroll_y_, target_y_, SCROLL_SPEED, PIXEL_SNAP);

        ImGui::SetCursorScreenPos({p0.x + gutter_width, p0.y});
        ImGui::SetNextWindowContentSize(content_size());
        ImGui::SetNextWindowScroll({scroll_x_, scroll_y_});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::BeginChild("##text", {text_width, v.height}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav |
                              (v.wrap ? 0 : ImGuiWindowFlags_HorizontalScrollbar));
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        // keys from the frame that moved focus here (e.g. enter in the find field) belong to the other widget
        const bool just_focused = focus_request_;
        if (focus_request_)
        {
            ImGui::FocusWindow(window);
            focus_request_ = false;
        }
        focused_ = ImGui::IsWindowFocused();
        view_left_ = window->Pos.x;
        view_width_ = window->InnerRect.GetWidth();
        view_height_ = window->InnerRect.GetHeight();

        // a scrollbar drag moved the view: follow it instead of animating back
        if (std::abs(window->Scroll.y - scroll_y_) > 1.0f)
        {
            scroll_y_ = target_y_ = window->Scroll.y;
        }
        if (std::abs(window->Scroll.x - scroll_x_) > 1.0f)
        {
            scroll_x_ = target_x_ = window->Scroll.x;
        }
        const float origin_x = window->Pos.x - window->Scroll.x + pad;
        const float origin_y = window->Pos.y - window->Scroll.y + pad;

        const bool hovered = ImGui::IsWindowHovered();
        if (hovered && !io.KeyCtrl)
        {
            const float wheel_x = io.KeyShift ? io.MouseWheel : io.MouseWheelH;
            const float wheel_y = io.KeyShift ? 0.0f : io.MouseWheel;
            target_x_ -= wheel_x * ascii_advance_[' '] * 8.0f;
            target_y_ -= wheel_y * line_height_ * 3.0f;
        }
        // middle drag pans; the view follows the hand directly instead of easing
        panning_ = ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
                   (panning_ || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)));
        if (panning_)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            scroll_x_ = target_x_ -= io.MouseDelta.x;
            scroll_y_ = target_y_ -= io.MouseDelta.y;
        }
        handle_mouse(origin_x, origin_y, hovered);
        if (focused_ && !just_focused)
        {
            handle_keyboard();
        }
        if (layout_dirty_)
        {
            rebuild_layout(wrap_width);
        }
        if (scroll_to_caret_)
        {
            scroll_to_caret_ = false;
            const float y = (static_cast<float>(row_of(caret_)) * line_height_) + pad;
            target_y_ = std::clamp(target_y_, y + line_height_ + pad - view_height_, y - pad);
            if (!v.wrap)
            {
                const float x = x_of(caret_) + pad;
                if (x < target_x_ + (pad * 2.0f) || x > target_x_ + view_width_ - (pad * 2.0f))
                {
                    target_x_ = std::max(0.0f, x - (view_width_ * (x < target_x_ + (pad * 2.0f) ? 0.25f : 0.75f)));
                }
            }
            caret_moved_ = ImGui::GetTime();
        }
        clamp_targets();
        show_matches_ = v.show_matches;
        if (matches_dirty_ && show_matches_)
        {
            find_matches();
        }
        const bool highlight = language_ != nullptr && std::cmp_less_equal(text_.size(), MAX_HIGHLIGHT_BYTES);
        if (spans_dirty_)
        {
            ++revision_;
            ++content_revision_;
            spans_ = syntax::span_buffer(scan(language_, text_.view(0, text_.size())));
        }
        else if (highlight && edited_.start >= 0)
        {
            syntax::rescan(*language_, text_, edited_, spans_);
        }
        spans_dirty_ = false;
        edited_.start = -1;
        edited_.joined = false;
        scanned_size_ = static_cast<int>(text_.size());
        // eased zoom: drawn under a widened clip so text scaling into view isn't culled, then the clip is restored
        const auto scale_drawn = [&](ImDrawList* dl, std::pair<int, int> since, ImVec2 pivot, ImVec4 clip) {
            std::ranges::for_each(std::span(dl->VtxBuffer.Data, dl->VtxBuffer.Size).subspan(since.first),
                                  [&](ImDrawVert& vert) { vert.pos = pivot + ((vert.pos - pivot) * zoom_scale_); });
            std::ranges::for_each(
                std::span(dl->CmdBuffer.Data, dl->CmdBuffer.Size).subspan(std::max(since.second - 1, 0)),
                [&](ImDrawCmd& cmd) {
                    // the last command still has the outer clip and takes whatever is drawn next
                    if (cmd.ClipRect != dl->_CmdHeader.ClipRect)
                    {
                        cmd.ClipRect = clip;
                    }
                });
        };
        const ImVec2 zoom_at{zoom_x_, zoom_y_};
        ImDrawList* text_dl = ImGui::GetWindowDrawList();
        const std::pair text_since{text_dl->VtxBuffer.Size, text_dl->CmdBuffer.Size};
        if (zoom_scale_ != 1.0f)
        {
            text_dl->PushClipRect(unscale(window->InnerClipRect.Min), unscale(window->InnerClipRect.Max));
        }
        draw_text(origin_x, origin_y);
        if (zoom_scale_ != 1.0f)
        {
            text_dl->PopClipRect();
            scale_drawn(text_dl, text_since, zoom_at, window->InnerClipRect.ToVec4());
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImDrawList* gutter_dl = ImGui::GetWindowDrawList();
        const std::pair gutter_since{gutter_dl->VtxBuffer.Size, gutter_dl->CmdBuffer.Size};
        draw_gutter(p0.x, gutter_width, origin_y);
        if (zoom_scale_ != 1.0f)
        {
            // numbers stay right-aligned, so they scale about the gutter's right edge
            scale_drawn(gutter_dl, gutter_since, {p0.x + gutter_width - (PAD * 0.75f * dpi_), zoom_at.y},
                        {p0.x, top_, p0.x + gutter_width, top_ + height_});
        }
        if (minimap_width > 0.0f)
        {
            draw_minimap(p0.x + gutter_width + text_width, minimap_width);
        }
        else
        {
            teardrop_ = 0.0f; // a fade left half-done would keep animating() true and the app awake
        }
        ImGui::SetCursorScreenPos(p0);
        ImGui::Dummy({total_width, v.height});
        ImGui::PopFont();
    }

    // the layout-space point an eased zoom draws at screen point p
    ImVec2 editor::unscale(ImVec2 p) const
    {
        const ImVec2 at{zoom_x_, zoom_y_};
        return at + ((p - at) / zoom_scale_);
    }

    // ranges an edit overlaps are dropped; ones after it move with the text
    void editor::shift_ranges(std::vector<fade>& ranges, int pos, int remove, int delta)
    {
        std::erase_if(ranges, [&](const fade& f) { return f.end > pos && f.start < pos + remove; });
        std::ranges::for_each(ranges, [&](fade& f) {
            if (f.start >= pos)
            {
                f.start += delta;
                f.end += delta;
            }
        });
    }

    // empty ranges draw nothing but would keep frames coming
    void editor::flash(int start, int end)
    {
        if (end > start)
        {
            flashes_.push_back({.start = start, .end = end, .time = ImGui::GetTime()});
        }
    }

    // whole-word occurrences of the selection when it is exactly one word
    void editor::find_word_matches(int lo, int hi)
    {
        word_matches_.clear();
        if (lo == hi || hi - lo > MAX_WORD_BYTES || text_.size() > MAX_WORD_SCAN_BYTES || char_class(text_[lo]) != 1 ||
            word_at(lo) != std::pair{lo, hi})
        {
            return;
        }
        const size_t size = text_.size();
        const auto n = static_cast<size_t>(hi - lo);
        const std::string word(text_.view(lo, hi)); // a copy: the views below move the gap
        const auto is_word = [&](size_t i) { return i < size && char_class(text_[i]) == 1; };
        const auto find_in = [&](size_t a, size_t b) {
            const std::string_view range = text_.view(a, b);
            // raw loop: each find starts past the last hit
            for (size_t at = range.find(word); at != std::string_view::npos && word_matches_.size() < MAX_MATCHES;
                 at = range.find(word, at + 1))
            {
                if ((a + at == 0 || !is_word(a + at - 1)) && !is_word(a + at + n))
                {
                    word_matches_.emplace_back(static_cast<int>(a + at), static_cast<int>(a + at + n));
                }
            }
        };
        // either side of the edit gap, then the hits straddling it, so the gap moves a word's length, not the file
        const size_t split = text_runs()[0].size();
        find_in(0, split);
        find_in(split + 1 > n ? split + 1 - n : 0, std::min(size, split + n - 1));
        find_in(split, size);
    }

    bool editor::in_literal(int pos) const
    {
        const size_t i = spans_.partition_point([&](const syntax::span& sp) { return sp.end <= pos; });
        return i < spans_.size() && spans_[i].start <= pos &&
               (spans_[i].kind == syntax::token::comment || spans_[i].kind == syntax::token::string);
    }

    // the bracket right of pos, else left of it, and its partner; brackets in comments and strings don't count
    std::pair<int, int> editor::match_bracket(int pos) const
    {
        static constexpr std::string_view OPEN = "([{";
        static constexpr std::string_view CLOSE = ")]}";
        const int size = static_cast<int>(text_.size());
        for (const int at : {pos, pos - 1})
        {
            if (at < 0 || at >= size || in_literal(at))
            {
                continue;
            }
            const char c = text_[at];
            const size_t open = OPEN.find(c);
            const size_t close = CLOSE.find(c);
            if (open == std::string_view::npos && close == std::string_view::npos)
            {
                continue;
            }
            const bool forward = open != std::string_view::npos;
            const char partner = forward ? CLOSE[open] : OPEN[close];
            const int step = forward ? 1 : -1;
            int depth = 0;
            // raw loop: walks byte by byte from the bracket, counting nesting
            for (int p = at + step, n = 0; p >= 0 && p < size && n < MAX_BRACKET_SCAN; p += step, ++n)
            {
                const char d = text_[p];
                if ((d != c && d != partner) || in_literal(p))
                {
                    continue;
                }
                if (d == c)
                {
                    ++depth;
                }
                else if (depth-- == 0)
                {
                    return {at, p};
                }
            }
        }
        return {-1, -1};
    }

    // the target moved by dx, dy: corners keep their drawn spots and pick springs by how they face the move
    void editor::retarget_caret(float dx, float dy)
    {
        const bool short_move = dy == 0.0f && std::abs(dx) <= CARET_SHORT_CHARS * advance(' ', 0.0f);
        const float base = short_move ? CARET_SHORT_SECONDS : CARET_SECONDS;
        std::array<float, 4> alignment{};
        // raw loop: corners, their places and alignments share an index
        for (size_t i = 0; i < caret_corners_.size(); ++i)
        {
            caret_corner& c = caret_corners_[i];
            const auto [rel_x, rel_y] = CARET_CORNERS[i];
            c.off_x += dx;
            c.off_y += dy;
            const float travel = std::hypot(c.off_x, c.off_y) * std::hypot(rel_x, rel_y);
            alignment[i] = travel > 0.0f ? ((c.off_x * rel_x) + (c.off_y * rel_y)) / travel : 0.0f;
        }
        std::array<size_t, 4> by_rank{0, 1, 2, 3};
        std::ranges::sort(by_rank, {}, [&](size_t i) { return alignment[i]; });
        // raw loop: the rank is the index into CARET_TRAIL
        for (size_t rank = 0; rank < by_rank.size(); ++rank)
        {
            caret_corner& c = caret_corners_[by_rank[rank]];
            c.seconds = alignment[by_rank[rank]] > CARET_LEADING ? CARET_SNAP_SECONDS : base * CARET_TRAIL[rank];
            if (c.seconds > CARET_RESET_SECONDS)
            {
                c.vel_x = c.vel_y = 0.0f;
            }
        }
    }

    // critically damped spring toward zero offset; false once it has landed
    bool editor::spring(float& offset, float& velocity, float seconds, float dt)
    {
        if (seconds <= dt || std::abs(offset) < PIXEL_SNAP)
        {
            offset = velocity = 0.0f;
            return false;
        }
        const float omega = 4.0f / seconds;
        const float a = offset;
        const float b = (offset * omega) + velocity;
        const float decay = std::exp(-omega * dt);
        offset = (a + (b * dt)) * decay;
        velocity = decay * (b - (a * omega) - (b * dt * omega));
        return true;
    }

    void editor::draw_text(float origin_x, float origin_y)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImRect view = ImGui::GetCurrentWindow()->InnerRect;
        const ImRect inner{unscale(view.Min), unscale(view.Max)}; // the part of the layout that lands on the view
        const float lh = line_height_;
        const int last_row = static_cast<int>(rows_.size()) - 1;
        const int first = std::clamp(static_cast<int>((inner.Min.y - origin_y) / lh), 0, last_row);
        const int last = std::clamp(static_cast<int>((inner.Max.y - origin_y) / lh) + 1, 0, last_row);
        const auto [lo, hi] = selection();
        const int caret_row = row_of(caret_);
        const float text_offset = (lh - ImGui::GetFontSize()) * 0.5f;
        const ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);
        const char* s = nullptr; // the row being drawn, made whole one row at a time so the gap moves at most a row
        const double now = ImGui::GetTime();
        std::erase_if(fades_, [&](const fade& f) { return now - f.time >= FADE_SECONDS; });
        std::erase_if(flashes_, [&](const fade& f) { return now - f.time >= FLASH_SECONDS; });

        // selection: its ends glide like the caret
        const auto end_at = [&](int pos) { return std::pair{x_of(pos), static_cast<float>(row_of(pos)) * lh}; };
        select_settled_ = true;
        if (lo != hi)
        {
            const auto [lo_x, lo_y] = end_at(lo);
            const auto [hi_x, hi_y] = end_at(hi);
            const std::array<float, 4> target{lo_x, lo_y, hi_x, hi_y};
            if (snap_caret_)
            {
                select_ends_ = target;
            }
            else if (last_selection_.first == last_selection_.second || hi < last_selection_.first ||
                     lo > last_selection_.second)
            {
                // new, or jumped away (find next): grows from its anchor rather than sweeping across the rows between
                const auto [x, y] = end_at(anchor_);
                select_ends_ = {x, y, x, y};
            }
            // raw loop: ends and targets share an index
            for (size_t i = 0; i < select_ends_.size(); ++i)
            {
                select_ends_[i] = motion::approach(select_ends_[i], target[i], SELECT_SPEED, PIXEL_SNAP);
                select_settled_ = select_settled_ && select_ends_[i] == target[i];
            }
        }
        last_selection_ = {lo, hi};
        const ImU32 select_col = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
        const int select_lo_row = static_cast<int>(std::lround(select_ends_[1] / lh));
        const int select_hi_row = static_cast<int>(std::lround(select_ends_[3] / lh));

        if (word_key_ != std::tuple{lo, hi, content_revision_})
        {
            word_key_ = {lo, hi, content_revision_};
            find_word_matches(lo, hi);
        }
        if (bracket_key_ != std::pair{caret_, content_revision_})
        {
            bracket_key_ = {caret_, content_revision_};
            const std::pair<int, int> found = lo == hi ? match_bracket(caret_) : std::pair{-1, -1};
            if (found != brackets_ && found.first >= 0)
            {
                bracket_time_ = now;
            }
            brackets_ = found;
        }

        // draws [p, q) from x, returning the end x; split at syntax colour changes and fading new characters
        const auto draw_run = [&](int p, int q, float x, float y) {
            while (p < q && x <= inner.Max.x)
            {
                // long tokens go in pieces so drawing can stop at the right edge; whole characters only
                int cut = std::min(q, p + 256);
                while (cut < q && (s[cut] & 0xC0) == 0x80)
                {
                    ++cut;
                }
                ImU32 col = text_col;
                if (const size_t i = spans_.partition_point([&](const syntax::span& sp) { return sp.end <= p; });
                    i < spans_.size())
                {
                    const syntax::span sp = spans_[i];
                    if (sp.start <= p)
                    {
                        cut = std::min(cut, sp.end);
                        col = TOKEN_COLORS[static_cast<size_t>(sp.kind)];
                    }
                    else
                    {
                        cut = std::min(cut, sp.start);
                    }
                }
                float t = 1.0f;
                for (const fade& f : fades_)
                {
                    if (f.start <= p && p < f.end)
                    {
                        cut = std::min(cut, f.end);
                        t = std::min(t, static_cast<float>((now - f.time) / FADE_SECONDS));
                    }
                    else if (f.start > p)
                    {
                        cut = std::min(cut, f.start);
                    }
                }
                const float e = motion::ease_out(t);
                dl->AddText({x, y + text_offset + ((1.0f - e) * FADE_RISE * dpi_)},
                            t >= 1.0f ? col : ImGui::GetColorU32(col, e), s + p, s + cut);
                x += measure(p, cut);
                p = cut;
            }
            return x;
        };

        if (focused_ && lo == hi)
        {
            const float y = origin_y + (static_cast<float>(caret_row) * lh);
            dl->AddRectFilled({inner.Min.x, y}, {inner.Max.x, y + lh}, IM_COL32(255, 255, 255, 9));
        }

        // x span of [a, b) on row w, reaching past the row end when it takes the newline too
        const auto span_x = [&](const row& w, int a, int b) {
            float x1 = measure(w.start, std::min(b, w.end));
            if (b > w.end && std::cmp_less(w.end, text_.size()) && text_[w.end] == '\n')
            {
                x1 += ascii_advance_[' '] * 0.6f;
            }
            return std::pair{origin_x + measure(w.start, std::max(a, w.start)), origin_x + x1};
        };
        // raw loop: walks sorted ranges alongside the rows; one wrapped across rows shows on each
        const auto draw_ranges = [&](const std::vector<std::pair<int, int>>& ranges, auto& it, const row& w, float y,
                                     ImU32 col) {
            for (; it != ranges.end() && it->first < w.end; ++it)
            {
                const auto [x0, x1] = span_x(w, it->first, std::min(it->second, w.end));
                dl->AddRectFilled({x0, y}, {x1, y + lh}, col, 3.0f * dpi_);
                if (it->second > w.end)
                {
                    break; // continues on the next row
                }
            }
        };
        const auto first_range = [&](const std::vector<std::pair<int, int>>& ranges, bool shown) {
            return shown ? std::ranges::upper_bound(ranges, rows_[first].start, {}, &std::pair<int, int>::second)
                         : ranges.end();
        };
        auto match = first_range(matches_, show_matches_);
        auto word = first_range(word_matches_, focused_);
        for (int r = first; r <= last; ++r) // raw loop: visible rows by index
        {
            const row w = rows_[r];
            s = text_.contiguous(w.start, w.end);
            const float y = origin_y + (static_cast<float>(r) * lh);
            draw_ranges(matches_, match, w, y, MATCH_COLOR);
            draw_ranges(word_matches_, word, w, y, WORD_COLOR);
            for (const fade& f : flashes_)
            {
                if (f.end > f.start && f.start <= w.end && f.end > w.start)
                {
                    // linear, so it lingers long enough to notice
                    const auto t = static_cast<float>((now - f.time) / FLASH_SECONDS);
                    const auto [x0, x1] = span_x(w, f.start, f.end);
                    dl->AddRectFilled({x0, y}, {x1, y + lh},
                                      IM_COL32(255, 255, 255, static_cast<int>(FLASH_ALPHA * (1.0f - t))), 3.0f * dpi_);
                }
            }
            // the band between the gliding ends' rows; partly covered rows show it sliding in
            const float row_top = static_cast<float>(r) * lh;
            const float top = std::max(row_top, select_ends_[1]);
            const float bottom = std::min(row_top + lh, select_ends_[3] + lh);
            if (lo != hi && bottom > top)
            {
                const float x0 = r <= select_lo_row ? origin_x + select_ends_[0] : origin_x;
                const float x1 = r >= select_hi_row ? origin_x + select_ends_[2] : span_x(w, w.end, w.end + 1).second;
                if (x1 > x0)
                {
                    dl->AddRectFilled({x0, origin_y + top}, {x1, origin_y + bottom}, select_col, 3.0f * dpi_);
                }
            }
            for (const int p : {brackets_.first, brackets_.second})
            {
                if (focused_ && p >= w.start && p < w.end)
                {
                    const float x0 = origin_x + measure(w.start, p);
                    const float x1 = x0 + ascii_advance_[static_cast<unsigned char>(text_[p])];
                    const float pulse =
                        1.0f - motion::ease_out(static_cast<float>((now - bracket_time_) / BRACKET_PULSE_SECONDS));
                    dl->AddRectFilled({x0, y}, {x1, y + lh}, IM_COL32(255, 255, 255, static_cast<int>(60 * pulse)),
                                      2.0f * dpi_);
                    dl->AddRect({x0, y}, {x1, y + lh}, BRACKET_COLOR, 2.0f * dpi_, 0, 1.0f * dpi_);
                }
            }
            float x = 0.0f;
            int p = w.start;
            // raw loop: walks past text left of the view instead of drawing it
            while (p < w.end)
            {
                unsigned int c = static_cast<unsigned char>(s[p]);
                const int len = c < 128 ? 1 : ImTextCharFromUtf8(&c, s + p, s + w.end);
                const float step = advance(c, x);
                if (origin_x + x + step >= inner.Min.x)
                {
                    break;
                }
                x += step;
                p += len;
            }
            // raw loop: draws tab-separated runs so tab stops line up, until past the view's right edge
            while (p < w.end && origin_x + x <= inner.Max.x)
            {
                const size_t tab = std::string_view(s + p, w.end - p).find('\t');
                const int q = tab == std::string_view::npos ? w.end : p + static_cast<int>(tab);
                if (q > p)
                {
                    x = draw_run(p, q, origin_x + x, y) - origin_x;
                }
                if (q < w.end)
                {
                    x += advance('\t', x);
                    p = q + 1;
                }
                else
                {
                    p = q;
                }
            }
        }

        // smooth caret: its corners spring toward the target, then it blinks once settled
        const float target_x = x_of(caret_);
        const float target_y = static_cast<float>(caret_row) * lh;
        if (snap_caret_)
        {
            for (caret_corner& c : caret_corners_)
            {
                c.off_x = c.off_y = c.vel_x = c.vel_y = 0.0f;
            }
            snap_caret_ = false;
        }
        else if (target_x != caret_x_ || target_y != caret_y_)
        {
            retarget_caret(target_x - caret_x_, target_y - caret_y_);
        }
        caret_x_ = target_x;
        caret_y_ = target_y;
        const float dt = std::min(ImGui::GetIO().DeltaTime, 1.0f / 30.0f);
        caret_settled_ = true;
        for (caret_corner& c : caret_corners_)
        {
            // both axes step even when one settles first
            const bool moving_x = spring(c.off_x, c.vel_x, c.seconds, dt);
            const bool moving_y = spring(c.off_y, c.vel_y, c.seconds, dt);
            caret_settled_ = caret_settled_ && !moving_x && !moving_y;
        }
        if (!focused_)
        {
            return;
        }
        const bool visible =
            !caret_settled_ || std::fmod(ImGui::GetTime() - caret_moved_, BLINK_HALF_PERIOD * 2) < BLINK_HALF_PERIOD;
        const ImVec2 caret{origin_x + caret_x_, origin_y + caret_y_};
        const ImVec2 box_min{caret.x - (1.0f * dpi_), caret.y + (2.0f * dpi_)};
        const ImVec2 box_max{caret.x + (1.0f * dpi_), caret.y + lh - (2.0f * dpi_)};
        const ImU32 caret_color = ImGui::GetColorU32(ImGuiCol_InputTextCursor);
        if (visible && caret_settled_)
        {
            dl->AddRectFilled(box_min, box_max, caret_color, 1.0f * dpi_);
        }
        else if (visible)
        {
            std::array<ImVec2, 4> quad;
            std::ranges::transform(
                caret_corners_, CARET_CORNERS, quad.begin(), [&](const caret_corner& c, std::pair<float, float> rel) {
                    return ImVec2{((box_min.x + box_max.x) * 0.5f) + (rel.first * (box_max.x - box_min.x)) - c.off_x,
                                  ((box_min.y + box_max.y) * 0.5f) + (rel.second * (box_max.y - box_min.y)) - c.off_y};
                });
            dl->AddQuadFilled(quad[0], quad[1], quad[2], quad[3], caret_color);
        }
        ImGuiPlatformImeData& ime = GImGui->PlatformImeData;
        ime.WantVisible = ime.WantTextInput = true;
        ime.InputPos = caret;
        ime.InputLineHeight = lh;
        ime.ViewportId = ImGui::GetMainViewport()->ID;
    }

    void editor::draw_gutter(float x, float width, float origin_y)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float lh = line_height_;
        const float text_offset = (lh - ImGui::GetFontSize()) * 0.5f;
        const int last_row = static_cast<int>(rows_.size()) - 1;
        const float view_top = unscale({0.0f, top_}).y;
        const float view_bottom = unscale({0.0f, top_ + height_}).y;
        const int first = std::clamp(static_cast<int>((view_top - origin_y) / lh), 0, last_row);
        const int last = std::clamp(static_cast<int>((view_bottom - origin_y) / lh) + 1, 0, last_row);
        const int caret_line = rows_[row_of(caret_)].line;
        const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.7f);
        const ImU32 bright = ImGui::GetColorU32(ImGuiCol_Text, 0.85f);

        dl->PushClipRect({x, view_top}, {x + width, view_bottom}, zoom_scale_ == 1.0f);
        std::array<char, 16> label{};
        for (int r = first; r <= last; ++r) // raw loop: visible rows by index
        {
            if (!rows_[r].first)
            {
                continue;
            }
            const char* const end = std::format_to_n(label.data(), label.size(), "{}", rows_[r].line + 1).out;
            // consolas and both fallbacks (segoe ui, proggy clean) have tabular digits
            const float w = static_cast<float>(end - label.data()) * ascii_advance_['0'];
            dl->AddText({x + width - (PAD * 0.75f * dpi_) - w, origin_y + (static_cast<float>(r) * lh) + text_offset},
                        rows_[r].line == caret_line ? bright : dim, label.data(), end);
        }
        dl->PopClipRect();
    }

    void editor::draw_minimap(float x, float width)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImGuiIO& io = ImGui::GetIO();
        const float row_h = MINIMAP_ROW * dpi_;
        const float scale = MINIMAP_CHAR * dpi_ / ascii_advance_[' '];
        const float total = static_cast<float>(rows_.size()) * row_h;
        const float scroll_max = std::max(1.0f, (static_cast<float>(rows_.size()) * line_height_) +
                                                    (PAD * 2.0f * dpi_) + (height_ * 0.5f) - view_height_);
        // when the whole minimap doesn't fit it scrolls in proportion to the editor
        // whole pixels: blocks under a pixel tall would flicker between one and two rows as it scrolls
        const float offset =
            total > height_ ? std::round(std::clamp(scroll_y_ / scroll_max, 0.0f, 1.0f) * (total - height_)) : 0.0f;
        const float block_h = std::max(1.0f, std::round(row_h * 0.7f));
        const float view_top = top_ + (scroll_y_ / line_height_ * row_h) - offset;
        const float view_h = view_height_ / line_height_ * row_h;

        // off-frame teardrop, sized by last frame's animation so it can be grabbed before the minimap
        // a lambda so the draw below picks up this frame's updated teardrop_; slides in from the edge while it grows
        const float drop_y = view_top + (std::min(view_h, height_) * 0.5f);
        const auto drop_shape = [&] {
            const float r = 4.5f * dpi_ * teardrop_;
            const ImVec2 tip{x + width - (2.0f * dpi_) + ((1.0f - teardrop_) * 6.0f * dpi_), drop_y};
            return std::tuple{r, tip, ImVec2{tip.x - (r * 2.2f), tip.y}};
        };
        const auto [drop_r, drop_tip, drop_c] = drop_shape();
        bool drop_held = false;
        if (teardrop_ > 0.0f)
        {
            // dragging it scrolls vertically like a scrollbar thumb: the view's centre follows the pointer
            ImGui::SetCursorScreenPos({drop_c.x - drop_r, drop_c.y - drop_r - (2.0f * dpi_)});
            ImGui::InvisibleButton("##teardrop", {drop_tip.x - drop_c.x + drop_r, (drop_r + (2.0f * dpi_)) * 2.0f});
            drop_held = ImGui::IsItemActive();
            if (drop_held)
            {
                target_y_ = ((io.MousePos.y - top_ + offset) / row_h * line_height_) - (view_height_ * 0.5f);
            }
            if (ImGui::IsItemDeactivated())
            {
                focus_request_ = true;
            }
        }

        // submitted first so it wins the hover over the minimap button it overlaps; wrapping sizes the
        // minimap itself, so only unwrapped text can be resized
        if (layout_wrap_width_ < 0.0f)
        {
            ImGui::SetCursorScreenPos({x, top_});
            ImGui::InvisibleButton("##minimap_grip", {MINIMAP_GRIP * dpi_, height_});
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (ImGui::IsItemActive())
            {
                // from the mouse, not its delta, so the edge stays under the pointer after hitting a limit
                minimap_width_ = (x + width - io.MousePos.x) / dpi_;
            }
            if (ImGui::IsItemDeactivated())
            {
                minimap_width_ = width / dpi_; // keep the clamped width, not wherever the drag overshot
            }
        }

        ImGui::SetCursorScreenPos({x, top_});
        ImGui::InvisibleButton("##minimap", {width, height_});
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemActive())
        {
            target_y_ = ((io.MousePos.y - top_ + offset) / row_h * line_height_) - (view_height_ * 0.5f);
            // inverse of the text blocks' mapping below; clamped to the content later, and to 0 when wrapping
            target_x_ = ((io.MousePos.x - x - (6.0f * dpi_)) / scale) + (PAD * dpi_) - (view_width_ * 0.5f);
        }
        if (ImGui::IsItemDeactivated())
        {
            focus_request_ = true;
        }
        if (hovered && io.MouseWheel != 0.0f && !io.KeyCtrl)
        {
            target_y_ -= io.MouseWheel * line_height_ * 3.0f;
        }

        dl->PushClipRect({x, top_}, {x + width, top_ + height_}, true);
        dl->AddLine({x, top_}, {x, top_ + height_}, ImGui::GetColorU32(ImGuiCol_Border));
        const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text, 0.32f);
        std::array<ImU32, TOKEN_COLORS.size()> faded{};
        std::ranges::transform(TOKEN_COLORS, faded.begin(), [](ImU32 c) { return ImGui::GetColorU32(c, 0.6f); });
        const float left = x + (6.0f * dpi_);
        const float max_x = (width - (8.0f * dpi_)) / scale;
        const int last_row = static_cast<int>(rows_.size()) - 1;
        const int first = std::clamp(static_cast<int>(offset / row_h), 0, last_row);
        const int last = std::clamp(static_cast<int>((offset + height_) / row_h) + 1, 0, last_row);
        // animation frames (caret, fades) redraw the same blocks, so they are only rebuilt when an input changes
        const minimap_key key{.revision = revision_,
                              .first = first,
                              .last = last,
                              .offset = offset,
                              .left = left,
                              .top = top_,
                              .scale = scale,
                              .max_x = max_x,
                              .block_h = block_h,
                              .ink = ink};
        const bool rebuild = key != minimap_key_;
        if (rebuild)
        {
            minimap_key_ = key;
            minimap_blocks_.clear();
        }
        for (int r = first; rebuild && r <= last; ++r) // raw loop: visible rows by index
        {
            const row w = rows_[r];
            const char* s = text_.contiguous(w.start, w.end);
            const float y = std::round(top_ + (static_cast<float>(r) * row_h) - offset);
            float cx = 0.0f;
            float run = -1.0f;
            ImU32 run_col = ink;
            // walked along with the text, so the spans are only read where the colour changes
            size_t span = spans_.partition_point([&](const syntax::span& sp) { return sp.end <= w.start; });
            ImU32 col = ink;
            int col_until = w.start; // where col stops applying
            // raw loop: merges each word into one block, like a zoomed-out view of the text; colours split blocks
            for (int p = w.start; p <= w.end && cx < max_x;)
            {
                unsigned int c = p < w.end ? static_cast<unsigned char>(s[p]) : ' ';
                const int len = c < 128 ? 1 : ImTextCharFromUtf8(&c, s + p, s + w.end); // ascii skips the decoder
                if (p >= col_until)
                {
                    while (span < spans_.size() && spans_[span].end <= p)
                    {
                        ++span;
                    }
                    const syntax::span next =
                        span < spans_.size()
                            ? spans_[span]
                            : syntax::span{.start = NO_LIMIT, .end = NO_LIMIT, .kind = syntax::token::comment};
                    col = next.start <= p ? faded[static_cast<size_t>(next.kind)] : ink;
                    col_until = next.start <= p ? next.end : next.start;
                }
                const bool blank = c == ' ' || c == '\t';
                if (run >= 0.0f && (blank || col != run_col))
                {
                    minimap_blocks_.push_back({left + (run * scale), y, left + (cx * scale), y + block_h, run_col});
                    run = -1.0f;
                }
                if (!blank && run < 0.0f)
                {
                    run = cx;
                    run_col = col;
                }
                cx += advance(c, cx);
                p += len;
            }
            if (run >= 0.0f)
            {
                minimap_blocks_.push_back(
                    {left + (run * scale), y, left + (std::min(cx, max_x) * scale), y + block_h, run_col});
            }
        }
        std::ranges::for_each(minimap_blocks_,
                              [&](const minimap_block& b) { dl->AddRectFilled({b.x0, b.y0}, {b.x1, b.y1}, b.col); });
        const auto draw_marks = [&](const std::vector<std::pair<int, int>>& ranges, bool shown, ImU32 col) {
            auto match = shown ? std::ranges::upper_bound(ranges, rows_[first].start, {}, &std::pair<int, int>::second)
                               : ranges.end();
            for (int r = first; r <= last && match != ranges.end(); ++r) // raw loop: visible rows by index
            {
                const row w = rows_[r];
                const float y = std::round(top_ + (static_cast<float>(r) * row_h) - offset);
                // raw loop: as in draw_text; at least 2 px wide so a short match still shows
                for (; match != ranges.end() && match->first < w.end; ++match)
                {
                    const float x0 =
                        left + (std::min(measure(w.start, std::max(match->first, w.start)), max_x) * scale);
                    const float x1 = left + (std::min(measure(w.start, std::min(match->second, w.end)), max_x) * scale);
                    dl->AddRectFilled({x0, y}, {std::max(x1, x0 + (2.0f * dpi_)), y + row_h}, col);
                    if (match->second > w.end)
                    {
                        break;
                    }
                }
            }
        };
        draw_marks(word_matches_, focused_, WORD_MARK_COLOR);
        draw_marks(matches_, show_matches_, MATCH_MARK_COLOR);
        // spans the full width unless the text is wider than the view
        const float pad = PAD * dpi_;
        float view_left = x + 1.0f;
        float view_right = x + width;
        if (content_width_ + (pad * 2.0f) > view_width_)
        {
            view_left = std::max(view_left, left + ((scroll_x_ - pad) * scale));
            view_right = left + ((scroll_x_ + view_width_ - pad) * scale);
        }
        dl->AddRectFilled({view_left, view_top}, {view_right, view_top + view_h},
                          IM_COL32(255, 255, 255, hovered || ImGui::IsItemActive() ? 26 : 14));
        dl->AddRect({view_left, view_top}, {view_right, view_top + view_h}, IM_COL32(255, 255, 255, 40), 0.0f,
                    ImDrawFlags_None, dpi_);
        // the view is almost entirely past the minimap's right edge (long lines): a teardrop points toward it
        const bool off_frame = x + width - view_left < (view_right - view_left) * 0.01f;
        teardrop_ = motion::approach(teardrop_, off_frame ? 1.0f : 0.0f, TEARDROP_SPEED, 0.005f);
        // held, it stays up even if the view comes back on screen mid-drag
        teardrop_ = drop_held ? 1.0f : teardrop_;
        if (teardrop_ > 0.0f)
        {
            const auto [r, tip, c] = drop_shape();
            // one outline (the far side of the circle, then out to the tip along its tangents) so no
            // overlapping shapes show through when it fades
            const float tangent = std::acos(1.0f / 2.2f);
            dl->PathArcTo(c, r, tangent, (2.0f * std::numbers::pi_v<float>)-tangent);
            dl->PathLineTo(tip);
            dl->PathFillConvex(ImGui::GetColorU32(ImVec4{0.000f, 0.471f, 0.831f, teardrop_})); // logo blue
        }
        dl->PopClipRect();
    }
} // namespace simple_notepad
