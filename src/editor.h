#pragma once

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
        };

        static constexpr int MAX_MATCHES = 20000; // bounds time and memory for a short needle in a huge file

        void draw(const view& v);

        void set_text(std::string text);           // clears undo history
        void replace_all(const std::string& text); // one undoable step
        void insert(std::string_view text);
        void select(int start, int end); // leaves focus where it is
        void focus();
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

        [[nodiscard]] const std::string& text() const
        {
            return text_;
        }
        [[nodiscard]] std::pair<int, int> selection() const; // ordered byte offsets
        [[nodiscard]] int line() const;
        [[nodiscard]] int column() const;
        [[nodiscard]] int char_count() const
        {
            return char_count_;
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

        struct row
        {
            int start;
            int end; // excludes the newline
            int line;
            bool first; // first visual row of its line
            float width;
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

        std::string text_;
        std::vector<fade> fades_; // recently inserted byte ranges that are still fading in
        std::string needle_;
        bool needle_case_ = false;
        bool matches_dirty_ = false;
        bool show_matches_ = false;
        bool matches_capped_ = false;
        std::vector<std::pair<int, int>> matches_; // sorted, non-overlapping byte ranges of needle_
        std::vector<row> rows_{{.start = 0, .end = 0, .line = 0, .first = true, .width = 0.0f}};
        int line_count_ = 1;
        int char_count_ = 0;

        // layout cache, rebuilt when text, font size or wrap width change
        bool layout_dirty_ = true;
        float layout_size_ = 0.0f;
        float layout_wrap_width_ = -1.0f;
        float content_width_ = 0.0f;
        ImFontBaked* baked_ = nullptr;
        std::array<float, 128> ascii_advance_{};
        float tab_width_ = 0.0f;
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
        float caret_x_ = 0.0f; // animated, content space
        float caret_y_ = 0.0f;
        bool caret_settled_ = true;
        double caret_moved_ = 0.0;

        [[nodiscard]] float advance(unsigned int c, float x) const;
        [[nodiscard]] float measure(int start, int end) const;
        void layout_line(int line_start, int line, std::vector<row>& out) const;
        void rebuild_layout(float wrap_width);
        void replace_text(int pos, int remove, std::string_view insert);
        void update_content_width();
        void find_matches();
        [[nodiscard]] bool same_char(char a, char b) const; // honours needle_case_
        [[nodiscard]] int row_of(int pos) const;
        [[nodiscard]] float x_of(int pos) const;
        [[nodiscard]] int pos_at(int row_index, float x) const;

        [[nodiscard]] int next_char(int pos) const;
        [[nodiscard]] int prev_char(int pos) const;
        [[nodiscard]] int word_edge(int pos, bool forward) const;
        [[nodiscard]] std::pair<int, int> word_at(int pos) const;
        [[nodiscard]] std::pair<int, int> line_at(int pos) const;

        void apply(int pos, int remove, std::string_view insert, merge kind);
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
