#pragma once

#include "gap_buffer.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace simple_notepad
{
    // one tokenizer for all languages: each names only its comments, and keywords come from one shared list
    class syntax
    {
      public:
        enum class token : std::uint8_t
        {
            comment,
            string,
            number,
            keyword,
            function,
            heading,
            emphasis,
            link,
        };

        struct span
        {
            int start;
            int end;
            token kind;

            friend span operator+(span s, int delta)
            {
                return {.start = s.start + delta, .end = s.end + delta, .kind = s.kind};
            }
        };
        using span_buffer = gap_buffer<std::vector<span>, int>;

        struct block
        {
            std::string_view open;
            std::string_view close;
            token kind;
        };

        struct language
        {
            std::array<std::string_view, 2> line_comments; // empty entries are unused
            std::array<block, 2> blocks;                   // tried before line comments, so "--[[" beats "--"
            bool code = true;                              // keywords, numbers and calls; off for markup prose
            bool tags = false;                             // the word after < or </ is a tag name
            bool markdown = false;
            bool csv = false;      // columns cycle through colours
            bool sections = false; // a line starting with [ is an ini section header
        };

        struct edit
        {
            int start;
            int old_end;         // end of the replaced bytes before the edit
            int new_end;         // end of the inserted bytes
            bool joined = false; // a line break was removed
        };

        // bytes highlighted per line, so an edit in a huge line rescans a bounded stretch (vs code stops at 20k)
        static constexpr size_t LINE_CAP = 64 * 1024;

        [[nodiscard]] static const language* find(const std::filesystem::path& filename); // nullptr for plain text
        [[nodiscard]] static std::vector<span> scan(const language& lang, std::string_view text); // sorted, disjoint
        // updates spans of the text before e: rescans from the edited line until the old spans line up again
        static void rescan(const language& lang, gap_buffer<std::string>& text, edit e, span_buffer& spans);

      private:
        struct job
        {
            std::string_view text;
            size_t from = 0;
            size_t cap = LINE_CAP; // where the line at from stops being highlighted
            // asked where the scan holds no state: line starts and span ends, with that line's cap
            std::function<bool(size_t, size_t)> stop;
            std::function<bool(size_t, size_t)> capped; // asked when the scan reaches a line's cap
            std::vector<span> out;
        };

        [[nodiscard]] static bool is_word(char c);
        [[nodiscard]] static size_t string_end(std::string_view text, size_t i);
        static bool scan_code(const language& lang, job& j); // true when stop ended it
        static bool scan_markdown(job& j);
        static bool scan_csv(job& j);
        static bool scan_any(const language& lang, job& j);
        static void scan_markdown_inline(job& j, size_t from, size_t to);
    };
} // namespace simple_notepad
