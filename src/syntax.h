#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
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
        };

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
            int old_end; // end of the replaced bytes before the edit
            int new_end; // end of the inserted bytes
        };

        [[nodiscard]] static const language* find(const std::filesystem::path& filename); // nullptr for plain text
        [[nodiscard]] static std::vector<span> scan(const language& lang, std::string_view text); // sorted, disjoint
        // updates spans of the text before e: rescans from the edited line until the old spans line up again
        static void rescan(const language& lang, std::string_view text, edit e, std::vector<span>& spans);

      private:
        struct job
        {
            std::string_view text;
            size_t from = 0;
            std::function<bool(size_t)> stop; // asked where the scan holds no state: line starts, span ends
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
