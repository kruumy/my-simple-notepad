#pragma once

#include "gap_buffer.h"
#include "syntax.h"
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct ImFont;
struct ImFontBaked;
struct ImVec2;

namespace simple_notepad
{
    // text editor widget: wrapped layout, line numbers, minimap, smooth caret/scroll and undo
    class editor
    {
      public:
        struct view
        {
            ImFont* font = nullptr;
            float font_size = 16.0f; // unscaled; dpi is applied by imgui's FontScaleDpi
            bool wrap = true;
            bool minimap = true;
            float dpi = 1.0f;
            float height = 0.0f;
            bool show_matches = false; // highlights in the text and marks in the minimap
            float scale = 1.0f;        // an eased zoom draws the text at this size relative to font_size
            bool hold_wrap = false;    // keeps the current wrap width, e.g. while the window is dragged to a new size
        };

        static constexpr int MAX_MATCHES = 20000; // bounds time and memory for a short needle in a huge file

        void draw(const view& v);

        void set_text(std::string text);    // clears undo history
        void replace_all(std::string text); // one undoable step
        void insert(std::string_view text);
        void select(int start, int end); // leaves focus where it is
        void focus();
        void set_language(const syntax::language* lang); // nullptr for plain text
        void set_search(std::string_view needle, bool match_case);
        [[nodiscard]] std::optional<size_t> search(size_t from, bool backward) const; // wraps around
        // index of the selected match (-1 when the selection is not one) and the match count
        [[nodiscard]] std::pair<int, int> match_status();
        [[nodiscard]] bool matches_capped() const // more matches exist than match_status counted
        {
            return matches_capped_;
        }
        [[nodiscard]] bool selection_is_match() const;
        [[nodiscard]] bool contains_needle(std::string_view s) const;

        // the whole text as one run, which moves the edit gap once; views stay valid until the next edit
        [[nodiscard]] std::string_view text() const
        {
            return text_.view(0, text_.size());
        }
        [[nodiscard]] std::string_view text(int start, int end) const
        {
            return text_.view(start, end);
        }
        // the text as the two runs either side of the edit gap, for writing out without moving it
        [[nodiscard]] std::array<std::string_view, 2> text_runs() const
        {
            return text_.runs();
        }
        [[nodiscard]] size_t byte_count() const
        {
            return text_.size();
        }
        [[nodiscard]] std::pair<int, int> selection() const; // ordered byte offsets
        [[nodiscard]] int line() const;
        [[nodiscard]] int column() const;
        [[nodiscard]] int char_count() const
        {
            return char_count_;
        }
        [[nodiscard]] int line_count() const
        {
            return line_count_;
        }
        // differs after every edit and returns to earlier values on undo, so it can track the saved point
        [[nodiscard]] std::uint64_t state() const;
        [[nodiscard]] bool focused() const
        {
            return focused_;
        }
        [[nodiscard]] bool animating() const;
        [[nodiscard]] double seconds_until_blink() const;

      private:
        enum class merge : std::uint8_t
        {
            none,
            typing,
            deleting,
        };

        enum class select_mode : std::uint8_t
        {
            chars,
            words,
            lines,
        };

        struct row_shift
        {
            int bytes;
            int lines;

            friend row_shift operator+(row_shift a, row_shift b)
            {
                return {.bytes = a.bytes + b.bytes, .lines = a.lines + b.lines};
            }
            friend row_shift operator-(row_shift a)
            {
                return {.bytes = -a.bytes, .lines = -a.lines};
            }
        };

        struct row
        {
            int start;
            int end; // excludes the newline
            int line;
            bool first;  // first visual row of its line
            float width; // in font sizes when unwrapped, so zooming keeps it

            friend row operator+(row r, row_shift d)
            {
                r.start += d.bytes;
                r.end += d.bytes;
                r.line += d.lines;
                return r;
            }
        };

        struct edit
        {
            int pos;
            std::string removed;
            std::string inserted;
            int caret_before;
            int anchor_before;
            merge kind;
            double time;
            std::uint64_t id;
        };

        struct fade
        {
            int start;
            int end;
            double time;
        };

        mutable gap_buffer<std::string> text_; // mutable: making a range one run moves the gap, not the text
        std::vector<fade> fades_;              // recently inserted byte ranges that are still fading in
        std::string needle_;
        bool needle_case_ = false;
        bool matches_dirty_ = false;
        bool show_matches_ = false;
        bool matches_capped_ = false;
        std::vector<std::pair<int, int>> matches_; // sorted, non-overlapping byte ranges of needle_
        const syntax::language* language_ = nullptr;
        syntax::span_buffer spans_;
        bool spans_dirty_ = false;                                     // needs a full scan
        syntax::edit edited_{.start = -1, .old_end = 0, .new_end = 0}; // range changed since the last scan
        int scanned_size_ = 0;
        gap_buffer<std::vector<row>, row_shift> rows_{
            std::vector<row>{{.start = 0, .end = 0, .line = 0, .first = true, .width = 0.0f}}};
        int line_count_ = 1;
        int char_count_ = 0;
        // characters from the line start to column_pos_, carried along so a huge line is counted once
        mutable int column_pos_ = -1;
        mutable int column_chars_ = 0;

        // layout cache, rebuilt when text, font size or wrap width change
        bool layout_dirty_ = true;
        float layout_size_ = 0.0f;
        float layout_wrap_width_ = -1.0f;
        float content_width_ = 0.0f;
        ImFontBaked* baked_ = nullptr;
        std::array<float, 128> ascii_advance_{};
        float tab_width_ = 0.0f;
        float mono_advance_ = 0.0f; // every printable ascii character's advance when they share one, else 0
        float line_height_ = 0.0f;

        int caret_ = 0;
        int anchor_ = 0;
        float preferred_x_ = -1.0f;
        select_mode mode_ = select_mode::chars;
        std::pair<int, int> drag_origin_{0, 0};

        std::deque<edit> undo_; // drops its oldest step once full
        std::vector<edit> redo_;
        std::uint64_t next_id_ = 1;

        float dpi_ = 1.0f;
        float top_ = 0.0f;
        float height_ = 0.0f;
        bool dragging_ = false;
        bool panning_ = false;
        float teardrop_ = 0.0f; // minimap off-frame marker, 0 hidden to 1 shown
        struct minimap_block
        {
            float x0, y0, x1, y1;
            std::uint32_t col;
        };
        struct minimap_key // everything the blocks are drawn from
        {
            std::uint64_t revision;
            int first, last;
            float offset, left, top, scale, max_x, block_h;
            std::uint32_t ink;
            bool operator==(const minimap_key&) const = default;
        };
        std::vector<minimap_block> minimap_blocks_;
        minimap_key minimap_key_{};
        std::uint64_t revision_ = 0; // changes with the text, rows or spans
        static constexpr float MINIMAP_WIDTH = 110.0f;
        float minimap_width_ = MINIMAP_WIDTH; // unscaled, dragged by its left edge
        bool snap_caret_ = true;
        bool focused_ = false;
        bool focus_request_ = true;
        bool scroll_to_caret_ = false;
        float scroll_x_ = 0.0f;
        float scroll_y_ = 0.0f;
        float target_x_ = 0.0f;
        float target_y_ = 0.0f;
        float view_left_ = 0.0f; // screen x of the text view
        float view_width_ = 0.0f;
        float view_height_ = 0.0f;
        float zoom_scale_ = 1.0f;
        float zoom_x_ = 0.0f; // screen point an eased zoom scales about: the mouse, or else the caret
        float zoom_y_ = 0.0f;
        float caret_x_ = 0.0f; // animated, content space
        float caret_y_ = 0.0f;
        bool caret_settled_ = true;
        double caret_moved_ = 0.0;

        [[nodiscard]] float advance(unsigned int c, float x) const;
        [[nodiscard]] float measure(int start, int end) const;
        [[nodiscard]] row next_row(int start, int line, bool first, int max_end) const;
        [[nodiscard]] row row_after(const row& r, int max_end) const; // r must not end the text
        void rebuild_layout(float wrap_width);
        void replace_text(int pos, int remove, std::string_view insert);
        void update_content_width();
        void find_matches();
        void update_matches(int pos, int remove, int inserted);
        [[nodiscard]] bool same_char(char a, char b) const; // honours needle_case_
        [[nodiscard]] int row_of(int pos) const;
        [[nodiscard]] float x_of(int pos) const;
        [[nodiscard]] ImVec2 unscale(ImVec2 p) const;
        [[nodiscard]] int pos_at(int row_index, float x) const;

        [[nodiscard]] int next_char(int pos) const;
        [[nodiscard]] int prev_char(int pos) const;
        [[nodiscard]] int word_edge(int pos, bool forward) const;
        [[nodiscard]] std::pair<int, int> word_at(int pos) const;
        [[nodiscard]] std::pair<int, int> line_at(int pos) const;
        [[nodiscard]] int line_start(int pos) const;
        [[nodiscard]] int line_end(int pos) const; // excludes the newline

        void apply(int pos, int remove, std::string insert, merge kind);
        void undo();
        void redo();
        void move_caret(int pos, bool extend);
        void erase_selection_or(int from, int to, merge kind);

        void handle_keyboard();
        void handle_mouse(float origin_x, float origin_y, bool hovered);
        void draw_text(float origin_x, float origin_y);
        void draw_gutter(float x, float width, float origin_y);
        void draw_minimap(float x, float width);
    };
} // namespace simple_notepad
