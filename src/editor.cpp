#include "editor.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <numbers>

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
    constexpr float CARET_SPEED = 38.0f;
    constexpr float TEARDROP_SPEED = 14.0f;
    constexpr double BLINK_HALF_PERIOD = 0.5;
    constexpr double MERGE_SECONDS = 1.0;
    constexpr size_t MAX_UNDO = 1000;
    constexpr double FADE_SECONDS = 0.22;
    constexpr float FADE_RISE = 4.0f; // new characters float up into place by this many pixels
    constexpr size_t MAX_FADE_BYTES = 4096;

    static float approach(float current, float target, float speed)
    {
        const float next = current + ((target - current) * (1.0f - std::exp(-speed * ImGui::GetIO().DeltaTime)));
        return std::abs(target - next) < 0.5f ? target : next;
    }

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

    // ---------------------------------------------------------------- public

    void editor::set_text(std::string text)
    {
        text_ = std::move(text);
        fades_.clear();
        undo_.clear();
        redo_.clear();
        caret_ = anchor_ = 0;
        scroll_x_ = scroll_y_ = target_x_ = target_y_ = 0.0f;
        layout_dirty_ = true;
        snap_caret_ = true;
        focus_request_ = true;
    }

    void editor::replace_all(const std::string& text)
    {
        const int caret = caret_;
        apply(0, static_cast<int>(text_.size()), text, merge::none);
        caret_ = anchor_ = std::min(caret, static_cast<int>(text_.size()));
        focus_request_ = true;
    }

    void editor::insert(std::string_view text)
    {
        const auto [lo, hi] = selection();
        apply(lo, hi - lo, text, merge::none);
    }

    void editor::select(int start, int end)
    {
        const int size = static_cast<int>(text_.size());
        anchor_ = std::clamp(start, 0, size);
        caret_ = std::clamp(end, 0, size);
        preferred_x_ = -1.0f;
        scroll_to_caret_ = true;
        caret_moved_ = ImGui::GetTime();
        focus_request_ = true;
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
        const size_t found = caret_ == 0 ? std::string::npos : text_.rfind('\n', caret_ - 1);
        const auto start = text_.begin() + (found == std::string::npos ? 0 : static_cast<int>(found) + 1);
        return 1 + static_cast<int>(
                       std::count_if(start, text_.begin() + caret_, [](char c) { return (c & 0xC0) != 0x80; }));
    }

    std::uint64_t editor::state() const
    {
        return undo_.empty() ? 0 : undo_.back().id;
    }

    bool editor::animating() const
    {
        return !caret_settled_ || !fades_.empty() || scroll_x_ != target_x_ || scroll_y_ != target_y_ ||
               (teardrop_ != 0.0f && teardrop_ != 1.0f);
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
        float x = 0.0f;
        const char* s = text_.data();
        // raw loop: utf-8 decoding steps a variable number of bytes
        for (int p = start; p < end;)
        {
            unsigned int c = 0;
            p += ImTextCharFromUtf8(&c, s + p, s + end);
            x += advance(c, x);
        }
        return x;
    }

    // appends the visual rows of the line starting at line_start; appending avoids a vector per line
    void editor::layout_line(int line_start, int line, std::vector<row>& out) const
    {
        const size_t found = text_.find('\n', line_start);
        const int line_end = found == std::string::npos ? static_cast<int>(text_.size()) : static_cast<int>(found);
        if (layout_wrap_width_ < 0.0f)
        {
            out.push_back({line_start, line_end, line, true, measure(line_start, line_end)});
            return;
        }
        const char* s = text_.data();
        int row_start = line_start;
        int last_break = -1;
        bool first = true;
        float x = 0.0f;
        // raw loop: greedily breaks the line at the last space that fits
        for (int p = line_start; p < line_end;)
        {
            unsigned int c = 0;
            const int len = ImTextCharFromUtf8(&c, s + p, s + line_end);
            float w = advance(c, x);
            if (x + w > layout_wrap_width_ && p > row_start)
            {
                const int at = last_break > row_start ? last_break : p;
                out.push_back({.start = row_start, .end = at, .line = line, .first = first, .width = 0.0f});
                first = false;
                row_start = at;
                last_break = -1;
                x = measure(row_start, p);
                w = advance(c, x);
            }
            x += w;
            if (c == ' ' || c == '\t')
            {
                last_break = p + len;
            }
            p += len;
        }
        out.push_back({.start = row_start, .end = line_end, .line = line, .first = first, .width = 0.0f});
    }

    void editor::rebuild_layout(float wrap_width)
    {
        layout_dirty_ = false;
        layout_wrap_width_ = wrap_width;
        rows_.clear();
        int line = 0;
        // raw loop: walks the text line by line
        for (int line_start = 0;; ++line)
        {
            layout_line(line_start, line, rows_);
            if (std::cmp_greater_equal(rows_.back().end, text_.size()))
            {
                break;
            }
            line_start = rows_.back().end + 1;
        }
        line_count_ = line + 1;
        char_count_ = static_cast<int>(std::ranges::count_if(text_, [](char c) { return (c & 0xC0) != 0x80; }));
        update_content_width();
    }

    // edits relayout only the lines they touch and shift the rows after them
    void editor::replace_text(int pos, int remove, std::string_view insert)
    {
        const auto chars = [](std::string_view s) {
            return static_cast<int>(std::ranges::count_if(s, [](char c) { return (c & 0xC0) != 0x80; }));
        };
        std::erase_if(fades_, [&](const fade& f) { return f.end > pos && f.start < pos + remove; });
        std::ranges::for_each(fades_, [&](fade& f) {
            if (f.start >= pos)
            {
                f.start += static_cast<int>(insert.size()) - remove;
                f.end += static_cast<int>(insert.size()) - remove;
            }
        });
        if (layout_dirty_)
        {
            text_.replace(pos, remove, insert);
            return;
        }
        int r0 = row_of(pos);
        while (r0 > 0 && !rows_[r0].first)
        {
            --r0;
        }
        int r1 = row_of(pos + remove);
        while (r1 + 1 < static_cast<int>(rows_.size()) && !rows_[r1 + 1].first)
        {
            ++r1;
        }
        const int old_last_line = rows_[r1].line;
        const int delta = static_cast<int>(insert.size()) - remove;
        const int region_end = rows_[r1].end + delta;

        char_count_ += chars(insert) - chars(std::string_view(text_).substr(pos, remove));
        text_.replace(pos, remove, insert);

        std::vector<row> fresh;
        int line = rows_[r0].line;
        // raw loop: relayouts each touched line
        for (int line_start = rows_[r0].start;; ++line)
        {
            layout_line(line_start, line, fresh);
            if (fresh.back().end >= region_end)
            {
                break;
            }
            line_start = fresh.back().end + 1;
        }
        const int line_delta = line - old_last_line;
        rows_.erase(rows_.begin() + r0, rows_.begin() + r1 + 1);
        rows_.insert(rows_.begin() + r0, fresh.begin(), fresh.end());
        std::for_each(rows_.begin() + r0 + static_cast<int>(fresh.size()), rows_.end(), [&](row& r) {
            r.start += delta;
            r.end += delta;
            r.line += line_delta;
        });
        line_count_ += line_delta;
        update_content_width();
    }

    void editor::update_content_width()
    {
        content_width_ =
            layout_wrap_width_ < 0.0f ? std::ranges::max(rows_, {}, &row::width).width : layout_wrap_width_;
    }

    int editor::row_of(int pos) const
    {
        const auto it = std::ranges::upper_bound(rows_, pos, {}, &row::start);
        return std::max(0, static_cast<int>(it - rows_.begin()) - 1);
    }

    float editor::x_of(int pos) const
    {
        return measure(rows_[row_of(pos)].start, pos);
    }

    int editor::pos_at(int row_index, float x) const
    {
        const row& r = rows_[std::clamp(row_index, 0, static_cast<int>(rows_.size()) - 1)];
        const char* s = text_.data();
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
        const size_t before = pos == 0 ? std::string::npos : text_.rfind('\n', pos - 1);
        const size_t after = text_.find('\n', pos);
        return {before == std::string::npos ? 0 : static_cast<int>(before) + 1,
                after == std::string::npos ? static_cast<int>(text_.size()) : static_cast<int>(after) + 1};
    }

    // ---------------------------------------------------------------- editing

    void editor::apply(int pos, int remove, std::string_view insert, merge kind)
    {
        const double now = ImGui::GetTime();
        const std::uint64_t id = next_id_++;
        bool merged = false;
        if (kind != merge::none && !undo_.empty())
        {
            edit& last = undo_.back();
            const bool recent = last.kind == kind && now - last.time < MERGE_SECONDS;
            // typing groups per word; deleting groups runs of backspace or delete
            if (recent && kind == merge::typing && remove == 0 &&
                last.pos + static_cast<int>(last.inserted.size()) == pos &&
                (last.inserted.back() != ' ' || insert == " "))
            {
                last.inserted += insert;
                merged = true;
            }
            else if (recent && kind == merge::deleting && insert.empty() && last.inserted.empty() &&
                     (pos + remove == last.pos || pos == last.pos))
            {
                const std::string removed = text_.substr(pos, remove);
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
            undo_.push_back({pos, text_.substr(pos, remove), std::string(insert), caret_, anchor_, kind, now, id});
            if (undo_.size() > MAX_UNDO)
            {
                undo_.erase(undo_.begin());
            }
        }
        redo_.clear();
        replace_text(pos, remove, insert);
        move_caret(pos + static_cast<int>(insert.size()), false);
        if (!insert.empty() && insert.size() <= MAX_FADE_BYTES)
        {
            fades_.push_back({.start = pos, .end = pos + static_cast<int>(insert.size()), .time = now});
        }
    }

    void editor::undo()
    {
        if (undo_.empty())
        {
            return;
        }
        edit e = std::move(undo_.back());
        undo_.pop_back();
        replace_text(e.pos, static_cast<int>(e.inserted.size()), e.removed);
        move_caret(e.caret_before, false);
        anchor_ = e.anchor_before;
        redo_.push_back(std::move(e));
    }

    void editor::redo()
    {
        if (redo_.empty())
        {
            return;
        }
        edit e = std::move(redo_.back());
        redo_.pop_back();
        replace_text(e.pos, static_cast<int>(e.removed.size()), e.inserted);
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
            apply(a, b - a, std::string_view(utf8.data(), len), merge::typing);
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
                const row& r = rows_[row_of(caret_)];
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
                for (const char c : std::string_view(text_).substr(lo, hi - lo))
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
                apply(lo, hi - lo, pasted, merge::none);
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
            const size_t nl = text_.rfind('\n', lo > 0 ? lo - 1 : std::string::npos);
            const int start = lo > 0 && nl != std::string::npos ? static_cast<int>(nl) + 1 : 0;
            const size_t found = text_.find('\n', hi);
            const int end = found == std::string::npos ? static_cast<int>(text_.size()) : static_cast<int>(found);
            const int caret = caret_;
            const int anchor = anchor_;
            const int shift_by = end - start + 1;
            apply(end, 0, "\n" + text_.substr(start, end - start), merge::none);
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
        }

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
            layout_dirty_ = true;
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
        if (layout_dirty_ || wrap_width != layout_wrap_width_)
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
        scroll_x_ = v.wrap ? 0.0f : approach(scroll_x_, target_x_, SCROLL_SPEED);
        scroll_y_ = approach(scroll_y_, target_y_, SCROLL_SPEED);

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
        draw_text(origin_x, origin_y);
        ImGui::EndChild();
        ImGui::PopStyleVar();

        draw_gutter(p0.x, gutter_width, origin_y);
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

    void editor::draw_text(float origin_x, float origin_y)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImRect inner = ImGui::GetCurrentWindow()->InnerRect;
        const float lh = line_height_;
        const int last_row = static_cast<int>(rows_.size()) - 1;
        const int first = std::clamp(static_cast<int>((inner.Min.y - origin_y) / lh), 0, last_row);
        const int last = std::clamp(static_cast<int>((inner.Max.y - origin_y) / lh) + 1, 0, last_row);
        const auto [lo, hi] = selection();
        const int caret_row = row_of(caret_);
        const float text_offset = (lh - ImGui::GetFontSize()) * 0.5f;
        const ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 select_col = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
        const char* s = text_.data();
        const double now = ImGui::GetTime();
        std::erase_if(fades_, [&](const fade& f) { return now - f.time >= FADE_SECONDS; });

        // draws [p, q) starting at x, splitting it where recently typed characters fade and rise in
        const auto draw_run = [&](int p, int q, float x, float y) {
            while (p < q)
            {
                int cut = q;
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
                const float e = 1.0f - ((1.0f - t) * (1.0f - t) * (1.0f - t)); // ease out
                dl->AddText({x, y + text_offset + ((1.0f - e) * FADE_RISE * dpi_)},
                            t >= 1.0f ? text_col : ImGui::GetColorU32(ImGuiCol_Text, e), s + p, s + cut);
                x += measure(p, cut);
                p = cut;
            }
        };

        if (focused_ && lo == hi)
        {
            const float y = origin_y + (static_cast<float>(caret_row) * lh);
            dl->AddRectFilled({inner.Min.x, y}, {inner.Max.x, y + lh}, IM_COL32(255, 255, 255, 9));
        }

        for (int r = first; r <= last; ++r) // raw loop: visible rows by index
        {
            const row& w = rows_[r];
            const float y = origin_y + (static_cast<float>(r) * lh);
            if (lo != hi && lo <= w.end && hi >= w.start)
            {
                const int a = std::max(lo, w.start);
                const int b = std::min(hi, w.end);
                const float x0 = origin_x + measure(w.start, a);
                float x1 = origin_x + measure(w.start, b);
                if (hi > w.end && std::cmp_less(w.end, text_.size()) && text_[w.end] == '\n')
                {
                    x1 += ascii_advance_[' '] * 0.6f; // show that the newline is selected
                }
                if (x1 > x0)
                {
                    dl->AddRectFilled({x0, y}, {x1, y + lh}, select_col, 3.0f * dpi_);
                }
            }
            float x = 0.0f;
            // raw loop: draws tab-separated runs so tab stops line up
            for (int p = w.start; p < w.end;)
            {
                const size_t tab = text_.find('\t', p);
                const int q = tab == std::string::npos ? w.end : std::min(static_cast<int>(tab), w.end);
                if (q > p)
                {
                    draw_run(p, q, origin_x + x, y);
                    x += measure(p, q);
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

        // smooth caret: glides toward its target, then blinks once settled
        const float target_x = x_of(caret_);
        const float target_y = static_cast<float>(caret_row) * lh;
        if (snap_caret_)
        {
            caret_x_ = target_x;
            caret_y_ = target_y;
            snap_caret_ = false;
        }
        caret_x_ = approach(caret_x_, target_x, CARET_SPEED);
        caret_y_ = approach(caret_y_, target_y, CARET_SPEED);
        caret_settled_ = caret_x_ == target_x && caret_y_ == target_y;
        if (!focused_)
        {
            return;
        }
        const bool visible =
            !caret_settled_ || std::fmod(ImGui::GetTime() - caret_moved_, BLINK_HALF_PERIOD * 2) < BLINK_HALF_PERIOD;
        const ImVec2 caret{origin_x + caret_x_, origin_y + caret_y_};
        if (visible)
        {
            dl->AddRectFilled({caret.x - (1.0f * dpi_), caret.y + (2.0f * dpi_)},
                              {caret.x + (1.0f * dpi_), caret.y + lh - (2.0f * dpi_)},
                              ImGui::GetColorU32(ImGuiCol_InputTextCursor), 1.0f * dpi_);
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
        const int first = std::clamp(static_cast<int>((top_ - origin_y) / lh), 0, last_row);
        const int last = std::clamp(static_cast<int>((top_ + height_ - origin_y) / lh) + 1, 0, last_row);
        const int caret_line = rows_[row_of(caret_)].line;
        const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.7f);
        const ImU32 bright = ImGui::GetColorU32(ImGuiCol_Text, 0.85f);

        dl->PushClipRect({x, top_}, {x + width, top_ + height_}, true);
        std::array<char, 16> label{};
        for (int r = first; r <= last; ++r) // raw loop: visible rows by index
        {
            if (!rows_[r].first)
            {
                continue;
            }
            const char* const end = std::format_to_n(label.data(), label.size(), "{}", rows_[r].line + 1).out;
            const float w = ImGui::CalcTextSize(label.data(), end).x;
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
        const float drop_r = 4.5f * dpi_ * teardrop_;
        // slides in from the edge while it grows
        const ImVec2 drop_tip{x + width - (2.0f * dpi_) + ((1.0f - teardrop_) * 6.0f * dpi_),
                              view_top + (std::min(view_h, height_) * 0.5f)};
        const ImVec2 drop_c{drop_tip.x - (drop_r * 2.2f), drop_tip.y};
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
        const float left = x + (6.0f * dpi_);
        const float max_x = (width - (8.0f * dpi_)) / scale;
        const int last_row = static_cast<int>(rows_.size()) - 1;
        const int first = std::clamp(static_cast<int>(offset / row_h), 0, last_row);
        const int last = std::clamp(static_cast<int>((offset + height_) / row_h) + 1, 0, last_row);
        const char* s = text_.data();
        for (int r = first; r <= last; ++r) // raw loop: visible rows by index
        {
            const row& w = rows_[r];
            const float y = std::round(top_ + (static_cast<float>(r) * row_h) - offset);
            float cx = 0.0f;
            float run = -1.0f;
            // raw loop: merges each word into one block, like a zoomed-out view of the text
            for (int p = w.start; p <= w.end && cx < max_x;)
            {
                unsigned int c = ' ';
                int len = 1;
                if (p < w.end)
                {
                    len = ImTextCharFromUtf8(&c, s + p, s + w.end);
                }
                const bool blank = c == ' ' || c == '\t';
                if (blank && run >= 0.0f)
                {
                    dl->AddRectFilled({left + (run * scale), y}, {left + (cx * scale), y + block_h}, ink);
                    run = -1.0f;
                }
                else if (!blank && run < 0.0f)
                {
                    run = cx;
                }
                cx += advance(c, cx);
                p += len;
            }
            if (run >= 0.0f)
            {
                dl->AddRectFilled({left + (run * scale), y}, {left + (std::min(cx, max_x) * scale), y + block_h}, ink);
            }
        }
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
        // approach snaps within 0.5, so ease in percent
        teardrop_ = approach(teardrop_ * 100.0f, off_frame ? 100.0f : 0.0f, TEARDROP_SPEED) / 100.0f;
        // held, it stays up even if the view comes back on screen mid-drag
        teardrop_ = drop_held ? 1.0f : teardrop_;
        if (teardrop_ > 0.0f)
        {
            const float r = 4.5f * dpi_ * teardrop_;
            const ImVec2 tip{x + width - (2.0f * dpi_) + ((1.0f - teardrop_) * 6.0f * dpi_), drop_tip.y};
            const ImVec2 c{tip.x - (r * 2.2f), tip.y};
            // one outline (the far side of the circle, then out to the tip along its tangents) so no
            // overlapping shapes show through when it fades
            const float tangent = std::acos(1.0f / 2.2f);
            dl->PathArcTo(c, r, tangent, (2.0f * std::numbers::pi_v<float>)-tangent);
            dl->PathLineTo(tip);
            dl->PathFillConvex(ImGui::GetColorU32(ImGuiCol_Text, teardrop_));
        }
        dl->PopClipRect();
    }
} // namespace simple_notepad
