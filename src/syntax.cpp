#include "syntax.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <string>
#include <utility>

namespace simple_notepad
{
    using token = syntax::token;

    // backticks are js and go's multi-line strings
    constexpr syntax::language C_LIKE{.line_comments = {"//"},
                                      .blocks = {{{.open = "/*", .close = "*/", .kind = token::comment},
                                                  {.open = "`", .close = "`", .kind = token::string}}}};
    constexpr syntax::language HASH{.line_comments = {"#"}};
    constexpr syntax::language PYTHON{.line_comments = {"#"},
                                      .blocks = {{{.open = R"(""")", .close = R"(""")", .kind = token::string},
                                                  {.open = "'''", .close = "'''", .kind = token::string}}}};
    constexpr syntax::language POWERSHELL{.line_comments = {"#"},
                                          .blocks = {{{.open = "<#", .close = "#>", .kind = token::comment}}}};
    constexpr syntax::language LUA{.line_comments = {"--"},
                                   .blocks = {{{.open = "--[[", .close = "]]", .kind = token::comment}}}};
    constexpr syntax::language SQL{.line_comments = {"--"},
                                   .blocks = {{{.open = "/*", .close = "*/", .kind = token::comment}}}};
    constexpr syntax::language INI{.line_comments = {";", "#"}, .sections = true};
    constexpr syntax::language BATCH{.line_comments = {"::"}};
    constexpr syntax::language MARKUP{
        .blocks = {{{.open = "<!--", .close = "-->", .kind = token::comment}}}, .code = false, .tags = true};
    constexpr syntax::language MARKDOWN{.code = false, .markdown = true};
    constexpr syntax::language CSV{.code = false, .csv = true};

    // lowercase extensions, or whole names for files that have none
    constexpr auto LANGUAGES = std::to_array<std::pair<std::wstring_view, const syntax::language*>>({
        {L".c", &C_LIKE},      {L".h", &C_LIKE},          {L".cpp", &C_LIKE},     {L".hpp", &C_LIKE},
        {L".cc", &C_LIKE},     {L".cxx", &C_LIKE},        {L".hxx", &C_LIKE},     {L".inl", &C_LIKE},
        {L".cs", &C_LIKE},     {L".java", &C_LIKE},       {L".kt", &C_LIKE},      {L".kts", &C_LIKE},
        {L".scala", &C_LIKE},  {L".js", &C_LIKE},         {L".mjs", &C_LIKE},     {L".cjs", &C_LIKE},
        {L".jsx", &C_LIKE},    {L".ts", &C_LIKE},         {L".tsx", &C_LIKE},     {L".go", &C_LIKE},
        {L".rs", &C_LIKE},     {L".swift", &C_LIKE},      {L".dart", &C_LIKE},    {L".php", &C_LIKE},
        {L".zig", &C_LIKE},    {L".glsl", &C_LIKE},       {L".hlsl", &C_LIKE},    {L".css", &C_LIKE},
        {L".scss", &C_LIKE},   {L".less", &C_LIKE},       {L".json", &C_LIKE},    {L".csv", &CSV},
        {L".jsonc", &C_LIKE},  {L".py", &PYTHON},         {L".pyw", &PYTHON},     {L".sh", &HASH},
        {L".bash", &HASH},     {L".zsh", &HASH},          {L".rb", &HASH},        {L".pl", &HASH},
        {L".r", &HASH},        {L".yaml", &HASH},         {L".yml", &HASH},       {L".toml", &HASH},
        {L".cmake", &HASH},    {L".conf", &HASH},         {L".gitignore", &HASH}, {L"cmakelists.txt", &HASH},
        {L"makefile", &HASH},  {L"dockerfile", &HASH},    {L".ps1", &POWERSHELL}, {L".psm1", &POWERSHELL},
        {L".lua", &LUA},       {L".sql", &SQL},           {L".ini", &INI},        {L".cfg", &INI},
        {L".reg", &INI},       {L".bat", &BATCH},         {L".cmd", &BATCH},      {L".html", &MARKUP},
        {L".htm", &MARKUP},    {L".xml", &MARKUP},        {L".svg", &MARKUP},     {L".xaml", &MARKUP},
        {L".csproj", &MARKUP}, {L".vcxproj", &MARKUP},    {L".props", &MARKUP},   {L".targets", &MARKUP},
        {L".md", &MARKDOWN},   {L".markdown", &MARKDOWN},
    });

    // the common keywords of the c family, python, rust, go, js, lua and shell; sorted for binary search
    constexpr auto KEYWORDS = std::to_array<std::string_view>({
        "False",      "None",     "True",     "abstract",  "and",     "as",      "async",     "auto",      "await",
        "bool",       "break",    "case",     "catch",     "char",    "class",   "const",     "constexpr", "continue",
        "def",        "default",  "defer",    "define",    "del",     "delete",  "do",        "done",      "double",
        "elif",       "else",     "elseif",   "end",       "endif",   "enum",    "esac",      "export",    "extends",
        "extern",     "false",    "fi",       "final",     "finally", "float",   "fn",        "for",       "foreach",
        "from",       "func",     "function", "global",    "goto",    "if",      "ifdef",     "ifndef",    "impl",
        "implements", "import",   "in",       "include",   "inline",  "int",     "interface", "is",        "lambda",
        "let",        "local",    "long",     "loop",      "match",   "mod",     "module",    "mut",       "namespace",
        "new",        "nil",      "not",      "null",      "nullptr", "of",      "or",        "override",  "package",
        "pass",       "pragma",   "private",  "protected", "pub",     "public",  "raise",     "return",    "self",
        "short",      "signed",   "sizeof",   "static",    "struct",  "super",   "switch",    "template",  "then",
        "this",       "throw",    "throws",   "true",      "try",     "type",    "typedef",   "typename",  "typeof",
        "union",      "unsigned", "use",      "using",     "var",     "virtual", "void",      "volatile",  "where",
        "while",      "with",     "yield",
    });
    static_assert(std::ranges::is_sorted(KEYWORDS));

    const syntax::language* syntax::find(const std::filesystem::path& filename)
    {
        const auto lower = [](std::wstring s) {
            std::ranges::transform(s, s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return s;
        };
        const std::wstring name = lower(filename.filename().wstring());
        const std::wstring ext = lower(filename.extension().wstring());
        const auto it = std::ranges::find_if(
            LANGUAGES, [&](const auto& entry) { return entry.first == name || (!ext.empty() && entry.first == ext); });
        return it == LANGUAGES.end() ? nullptr : it->second;
    }

    bool syntax::is_word(char c)
    {
        const auto u = static_cast<unsigned char>(c);
        return std::isalnum(u) != 0 || c == '_' || u >= 0x80; // utf-8 bytes, so a word never splits a character
    }

    // end of the string at i, or npos if unclosed on its line; never looking past it keeps edits local
    size_t syntax::string_end(std::string_view text, size_t i)
    {
        const char quote = text[i];
        for (size_t j = i + 1; j < text.size(); ++j) // raw loop: skips escaped characters
        {
            if (text[j] == '\\' && j + 1 < text.size() && text[j + 1] != '\n')
            {
                ++j;
            }
            else if (text[j] == quote)
            {
                return j + 1;
            }
            else if (text[j] == '\n')
            {
                return std::string_view::npos;
            }
        }
        return std::string_view::npos;
    }

    std::vector<syntax::span> syntax::scan(const language& lang, std::string_view text)
    {
        job j{.text = text, .stop = [](size_t) { return false; }};
        scan_any(lang, j);
        return std::move(j.out);
    }

    // a line start outside every span holds no scanner state, so the old spans hold again once both runs agree
    void syntax::rescan(const language& lang, std::string_view text, edit e, std::vector<span>& spans)
    {
        const int delta = e.new_end - e.old_end;
        // npos + 1 wraps to 0 for the first line
        size_t from = e.start == 0 ? 0 : text.rfind('\n', e.start - 1) + 1;
        // lower bound: a span ending exactly at a line start can only be one left open to the end of the text
        auto first = std::ranges::lower_bound(spans, static_cast<int>(from), {}, &span::end);
        if (first != spans.end() && std::cmp_less(first->start, from))
        {
            from = first->start; // a block comment or fence runs through the edited line
        }
        if (!lang.markdown && !lang.csv)
        {
            // code holds no state at a span's start either, so a long line resumes at its last span before the edit
            // strictly before the edit: a word ending at an edited byte may now run on into it
            const auto last = std::ranges::upper_bound(first, spans.end(), e.start - 1, {}, &span::start);
            if (last != first && std::cmp_greater(std::prev(last)->start, from))
            {
                first = std::prev(last);
                from = first->start;
            }
        }

        // spans from first on are still in offsets from before the edit
        auto next = first; // first old span ending after the line being checked
        job j{.text = text, .from = from, .stop = [&](size_t pos) {
                  if (std::cmp_less(pos, e.new_end + 2))
                  {
                      return false; // the scanner looks back two bytes, and those must be unedited too
                  }
                  const int old_pos = static_cast<int>(pos) - delta;
                  next = std::ranges::upper_bound(next, spans.end(), old_pos, {}, &span::end);
                  if (text[pos - 1] == '\n')
                  {
                      return next == spans.end() || next->start >= old_pos;
                  }
                  return next != spans.begin() && std::prev(next)->end == old_pos; // mid-line: a span ended here too
              }};
        if (!scan_any(lang, j))
        {
            next = spans.end();
        }
        std::ranges::for_each(next, spans.end(), [&](span& s) {
            s.start += delta;
            s.end += delta;
        });
        if (std::cmp_equal(j.out.size(), next - first))
        {
            std::ranges::copy(j.out, first); // usual keystroke: no tail to move
        }
        else
        {
            spans.insert(spans.erase(first, next), j.out.begin(), j.out.end());
        }
    }

    bool syntax::scan_code(const language& lang, job& j)
    {
        const std::string_view text = j.text;
        size_t span_end = std::string_view::npos;
        const auto add = [&](size_t a, size_t b, token kind) {
            j.out.push_back({.start = static_cast<int>(a), .end = static_cast<int>(b), .kind = kind});
            span_end = b;
        };
        const auto starts = [&](size_t i, std::string_view s) { return !s.empty() && text.substr(i).starts_with(s); };
        size_t i = j.from;
        while (i < text.size()) // raw loop: each token decides how far to advance
        {
            // line starts, and in code the end of a span, are where the scanner holds no state
            if (i > j.from && (text[i - 1] == '\n' || i == span_end) && j.stop(i))
            {
                return true;
            }
            const char c = text[i];
            if (lang.sections && c == '[' &&
                text.find_first_not_of(" \t", i == 0 ? 0 : text.rfind('\n', i - 1) + 1) == i) // npos + 1 wraps to 0
            {
                const size_t end = std::min(text.find('\n', i), text.size());
                add(i, end, token::heading);
                i = end;
                continue;
            }
            const bool after_word = i > 0 && is_word(text[i - 1]);
            if (const auto b = std::ranges::find_if(lang.blocks, [&](const block& b) { return starts(i, b.open); });
                b != lang.blocks.end())
            {
                const size_t close = text.find(b->close, i + b->open.size());
                const size_t end = close == std::string_view::npos ? text.size() : close + b->close.size();
                add(i, end, b->kind);
                i = end;
                continue;
            }
            if (std::ranges::any_of(lang.line_comments, [&](std::string_view s) { return starts(i, s); }))
            {
                const size_t end = std::min(text.find('\n', i), text.size());
                add(i, end, token::comment);
                i = end;
                continue;
            }
            // an apostrophe inside a word ("don't") opens nothing
            if (c == '"' || c == '`' || (c == '\'' && !after_word))
            {
                // unclosed runs to the line end, so no token looks ahead past a later span's start
                const size_t close = string_end(text, i);
                const size_t end = close != std::string_view::npos ? close : std::min(text.find('\n', i), text.size());
                add(i, end, token::string);
                i = end;
                continue;
            }
            if (!is_word(c))
            {
                ++i;
                continue;
            }
            size_t end = i;
            while (end < text.size() &&
                   (is_word(text[end]) || (text[end] == '.' && std::isdigit(static_cast<unsigned char>(c)) != 0)))
            {
                ++end;
            }
            const std::string_view word = text.substr(i, end - i);
            const bool tag = lang.tags && i > 0 && (text[i - 1] == '<' || (i > 1 && text.substr(i - 2, 2) == "</"));
            // s.end() and p->type are names, not the keywords end and type
            const bool member = i > 0 && (text[i - 1] == '.' || (i > 1 && text.substr(i - 2, 2) == "->"));
            if (tag)
            {
                add(i, end, token::keyword);
            }
            else if (lang.code && std::isdigit(static_cast<unsigned char>(c)) != 0)
            {
                add(i, end, token::number);
            }
            else if (lang.code && !member && std::ranges::binary_search(KEYWORDS, word))
            {
                add(i, end, token::keyword);
            }
            else if (lang.code)
            {
                const size_t next = text.find_first_not_of(" \t", end);
                if (next != std::string_view::npos && text[next] == '(')
                {
                    add(i, end, token::function);
                }
            }
            i = end;
        }
        return false;
    }

    bool syntax::scan_markdown(job& j)
    {
        const std::string_view text = j.text;
        const auto add = [&](size_t a, size_t b, token kind) {
            if (b > a)
            {
                j.out.push_back({.start = static_cast<int>(a), .end = static_cast<int>(b), .kind = kind});
            }
        };
        // a fence is one span from its opening line to its closing one, so a resumed scan can find where it began
        size_t fence = std::string_view::npos;
        for (size_t a = j.from; a < text.size();) // raw loop: line by line
        {
            if (a > j.from && fence == std::string_view::npos && j.stop(a))
            {
                return true;
            }
            const size_t b = std::min(text.find('\n', a), text.size());
            const std::string_view line = text.substr(a, b - a);
            const size_t indent = std::min(line.find_first_not_of(' '), line.size());
            const std::string_view s = line.substr(indent);
            const size_t hashes = std::min(s.find_first_not_of('#'), s.size());
            const bool fence_line = s.starts_with("```") || s.starts_with("~~~");
            if (fence == std::string_view::npos && fence_line)
            {
                fence = a;
            }
            else if (fence != std::string_view::npos)
            {
                if (fence_line)
                {
                    add(fence, b, token::string);
                    fence = std::string_view::npos;
                }
            }
            else if (hashes >= 1 && hashes <= 6 && (hashes == s.size() || s[hashes] == ' '))
            {
                add(a, b, token::heading);
            }
            else if (s.starts_with('>'))
            {
                add(a, b, token::comment);
            }
            else
            {
                size_t from = a + indent;
                const size_t digits = std::min(s.find_first_not_of("0123456789"), s.size());
                if (s.starts_with("- ") || s.starts_with("* ") || s.starts_with("+ "))
                {
                    add(from, from + 1, token::keyword);
                    from += 1;
                }
                else if (digits > 0 && s.substr(digits).starts_with(". "))
                {
                    add(from, from + digits + 1, token::keyword);
                    from += digits + 1;
                }
                scan_markdown_inline(j, from, b);
            }
            a = b + 1;
        }
        if (fence != std::string_view::npos)
        {
            add(fence, text.size(), token::string); // unclosed: runs to the end like an unclosed block comment
        }
        return false;
    }

    bool syntax::scan_any(const language& lang, job& j)
    {
        return lang.markdown ? scan_markdown(j) : lang.csv ? scan_csv(j) : scan_code(lang, j);
    }

    // rainbow columns: each field takes the next colour, and a quoted field may hold commas
    bool syntax::scan_csv(job& j)
    {
        constexpr std::array COLUMN_COLORS{token::keyword,  token::string,   token::number,
                                           token::function, token::emphasis, token::link};
        const std::string_view text = j.text;
        size_t column = 0;
        for (size_t i = j.from; i < text.size();) // raw loop: each field decides how far to advance
        {
            if (text[i] == '\n')
            {
                column = 0;
                ++i;
                if (i < text.size() && j.stop(i))
                {
                    return true;
                }
                continue;
            }
            size_t end = i;
            if (text[i] == '"')
            {
                end = string_end(text, i); // "" inside reads as a close and reopen, which ends at the same place
                end = end == std::string_view::npos ? std::min(text.find('\n', i), text.size()) : end;
            }
            end = std::min({text.find(',', end), text.find('\n', end), text.size()});
            if (end > i)
            {
                j.out.push_back({.start = static_cast<int>(i),
                                 .end = static_cast<int>(end),
                                 .kind = COLUMN_COLORS[column % COLUMN_COLORS.size()]});
            }
            ++column;
            i = end < text.size() && text[end] == ',' ? end + 1 : end;
        }
        return false;
    }

    // `code`, **bold**, *italic*, _italic_ and [links](url) that close on the same line
    void syntax::scan_markdown_inline(job& j, size_t from, size_t to)
    {
        const std::string_view line = j.text.substr(0, to);
        for (size_t i = from; i < to;) // raw loop: each marker decides how far to advance
        {
            const char c = line[i];
            size_t end = std::string_view::npos;
            token kind = token::emphasis;
            if (c == '`')
            {
                const size_t close = line.find('`', i + 1);
                end = close == std::string_view::npos ? close : close + 1;
                kind = token::string;
            }
            else if (c == '*' || (c == '_' && (i == from || !is_word(line[i - 1]))))
            {
                // a marker followed by a space is arithmetic or a bullet, not emphasis
                const std::string_view marker = line.substr(i, i + 1 < to && line[i + 1] == c ? 2 : 1);
                if (i + marker.size() < to && line[i + marker.size()] != ' ')
                {
                    const size_t close = line.find(marker, i + marker.size());
                    end = close == std::string_view::npos ? close : close + marker.size();
                }
            }
            else if (c == '[')
            {
                const size_t mid = line.find("](", i + 1);
                const size_t close = mid == std::string_view::npos ? mid : line.find(')', mid + 2);
                end = close == std::string_view::npos ? close : close + 1;
                kind = token::link;
            }
            if (end == std::string_view::npos)
            {
                ++i;
                continue;
            }
            j.out.push_back({.start = static_cast<int>(i), .end = static_cast<int>(end), .kind = kind});
            i = end;
        }
    }
} // namespace simple_notepad
