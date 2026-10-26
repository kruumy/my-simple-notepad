#include "syntax.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
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
        // ascii by hand: std::isalnum looks up the locale on every call, the bulk of a scan
        return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || c == '_' ||
               u >= 0x80; // utf-8 bytes, so a word never splits a character
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
        job j{
            .text = text, .stop = [](size_t, size_t) { return false; }, .capped = [](size_t, size_t) { return false; }};
        scan_any(lang, j);
        return std::move(j.out);
    }

    // a line start outside every span holds no scanner state, so the old spans hold again once both runs agree
    void syntax::rescan(const language& lang, gap_buffer<std::string>& text, edit e, span_buffer& spans)
    {
        const int delta = e.new_end - e.old_end;
        // the line start only matters within LINE_CAP bytes: an edit further into its line is past highlighting
        const size_t back = std::cmp_greater(e.start, LINE_CAP) ? e.start - LINE_CAP : 0;
        const size_t newline = text.view(back, e.start).rfind('\n');
        const size_t line_start = newline == std::string_view::npos ? 0 : back + newline + 1;
        const bool past_cap = (newline == std::string_view::npos && back > 0) || e.start >= line_start + LINE_CAP;
        // past the cap, an edit that no span crosses and that moves no line break only shifts the spans after it
        size_t first = spans.partition_point([&](const span& s) { return s.end <= e.start; });
        const bool crossed = first < spans.size() && spans[first].start < e.start;
        if (past_cap && !crossed && !e.joined && text.view(e.start, e.new_end).find('\n') == std::string_view::npos)
        {
            spans.replace(first, 0, {});
            spans.shift_tail(delta);
            return;
        }

        size_t from = past_cap ? e.start : line_start;
        if (past_cap && (lang.markdown || lang.csv))
        {
            // these scanners start on a line, and the rest of this one is past the cap anyway
            const size_t newline_after = text.view(from, text.size()).find('\n');
            from = newline_after == std::string_view::npos ? text.size() : from + newline_after + 1;
        }
        // the scan starts skipping once it reaches cap
        size_t cap = !past_cap ? line_start + LINE_CAP : lang.markdown || lang.csv ? from + LINE_CAP : from;
        if (!past_cap)
        {
            // lower bound: a span ending exactly at a line start can only be one left open to the end of the text
            first = spans.partition_point([&](const span& s) { return std::cmp_less(s.end, from); });
            // code holds no state at a span's start either, so a long line resumes at its last span before the edit
            // strictly before the edit: a word ending at an edited byte may now run on into it
            const size_t before = spans.partition_point([&](const span& s) { return s.start < e.start; });
            if (!lang.markdown && !lang.csv && before > first && std::cmp_greater(spans[before - 1].start, from))
            {
                first = before - 1;
                from = spans[first].start;
            }
        }
        if (first < spans.size() && std::cmp_less(spans[first].start, from))
        {
            // a block comment or fence runs through the edit's line; it began within its own line's cap
            from = spans[first].start;
            const size_t from_back = from > LINE_CAP ? from - LINE_CAP : 0;
            const size_t from_newline = text.view(from_back, from).rfind('\n');
            cap = from_newline != std::string_view::npos ? from_back + from_newline + 1 + LINE_CAP
                  : from_back == 0                       ? LINE_CAP
                                                         : from;
        }

        // one run from just before the scan start: the scanner reads back two bytes, ini sections to their line start
        const size_t window = past_cap ? from : std::min(from, line_start);
        const std::string_view all(text.contiguous(window < 2 ? 0 : window - 2, text.size()), text.size());
        // spans from first on are still in offsets from before the edit
        size_t next = first; // first old span ending after the line being checked
        size_t checked_cap = std::string_view::npos;
        bool reached = false;
        const auto reaches_cap = [&](size_t pos, size_t line_cap) {
            if (checked_cap != line_cap)
            {
                checked_cap = line_cap; // once per line, so each span end on a long line doesn't search again
                reached = line_cap <= pos ||
                          (line_cap <= all.size() && std::memchr(all.data() + pos, '\n', line_cap - pos) == nullptr);
            }
            return reached;
        };
        job j{.text = all,
              .from = from,
              .cap = cap,
              .stop =
                  [&](size_t pos, size_t line_cap) {
                      if (std::cmp_less(pos, e.new_end + 2))
                      {
                          return false; // the scanner looks back two bytes, and those must be unedited too
                      }
                      const int old_pos = static_cast<int>(pos) - delta;
                      next = spans.partition_point([&](const span& s) { return s.end <= old_pos; }, next);
                      if (all[pos - 1] == '\n')
                      {
                          return next == spans.size() || spans[next].start >= old_pos;
                      }
                      // mid-line: the old tail only shifts if neither run of this line reached a cap that stayed put
                      const bool start_moved = line_cap >= static_cast<size_t>(e.new_end) + 1 + LINE_CAP;
                      const size_t line_begin = line_cap - LINE_CAP;
                      // the old line's cap in new offsets: unknown when the line start came from the edited bytes
                      const size_t old_cap = line_begin <= static_cast<size_t>(e.start) ? line_cap + delta
                                             : !e.joined && !past_cap                   ? line_start + LINE_CAP + delta
                                                                                        : 0;
                      if (!start_moved && reaches_cap(pos, std::min(line_cap, old_cap)))
                      {
                          return false;
                      }
                      return next != 0 && spans[next - 1].end == old_pos;
                  },
              .capped =
                  [&](size_t pos, size_t line_cap) {
                      if (std::cmp_less(pos, e.new_end + 2))
                      {
                          return false;
                      }
                      // no spans past either run's cap, so old ones resume after the old line's cap (moved if its start
                      // did)
                      size_t old_from = static_cast<size_t>(static_cast<int>(pos) - delta);
                      if (line_cap < static_cast<size_t>(e.new_end) + 1 + LINE_CAP)
                      {
                          if (e.joined)
                          {
                              return false; // the old line may have been a later one: scan on to the line end
                          }
                          const size_t old_cap = past_cap ? 0 : line_start + LINE_CAP;
                          if (old_cap > old_from)
                          {
                              // the old line may have ended short of its cap, a stretch no longer than the edit
                              const size_t until = std::min(old_cap + delta, all.size());
                              const auto* newline =
                                  static_cast<const char*>(std::memchr(all.data() + pos, '\n', until - pos));
                              old_from = newline == nullptr ? old_cap : newline - all.data() - delta;
                          }
                      }
                      next =
                          spans.partition_point([&](const span& s) { return std::cmp_less(s.start, old_from); }, next);
                      return true;
                  }};
        if (!scan_any(lang, j))
        {
            next = spans.size();
        }
        spans.replace(first, next - first, j.out);
        spans.shift_tail(delta);
    }

    bool syntax::scan_code(const language& lang, job& j)
    {
        const std::string_view text = j.text;
        size_t span_end = std::string_view::npos;
        const auto add = [&](size_t a, size_t b, token kind) {
            j.out.push_back({.start = static_cast<int>(a), .end = static_cast<int>(b), .kind = kind});
            span_end = b;
        };
        // bytes that can begin a token; any other byte is skipped without trying each opener
        std::array<bool, 256> opens{};
        for (int b = 0; b < 256; ++b) // raw loop: fills a lookup table by index
        {
            opens[b] = is_word(static_cast<char>(b));
        }
        for (const char b : {'"', '`', '\''})
        {
            opens[static_cast<unsigned char>(b)] = true;
        }
        opens['['] = lang.sections;
        for (const std::string_view s :
             {lang.blocks[0].open, lang.blocks[1].open, lang.line_comments[0], lang.line_comments[1]})
        {
            if (!s.empty())
            {
                opens[static_cast<unsigned char>(s[0])] = true;
            }
        }

        size_t cap = j.cap; // where the current line stops being highlighted
        size_t i = j.from;
        while (i < text.size()) // raw loop: each token decides how far to advance
        {
            if (i > 0 && text[i - 1] == '\n')
            {
                cap = i + LINE_CAP;
            }
            // line starts, and in code the end of a span, are where the scanner holds no state
            if (i > j.from && (text[i - 1] == '\n' || i == span_end) && j.stop(i, cap))
            {
                return true;
            }
            if (i >= cap)
            {
                if (j.capped(i, cap))
                {
                    return true;
                }
                const size_t newline = text.find('\n', i); // nothing more is highlighted on this line
                i = newline == std::string_view::npos ? text.size() : newline + 1;
                continue;
            }
            if (!opens[static_cast<unsigned char>(text[i])])
            {
                ++i;
                continue;
            }
            // single-line tokens end at the cap
            const std::string_view line = text.substr(0, std::min(cap, text.size()));
            const auto starts = [&](std::string_view s) { return !s.empty() && line.substr(i).starts_with(s); };
            const char c = text[i];
            if (lang.sections && c == '[' &&
                text.find_first_not_of(" \t", i == 0 ? 0 : text.rfind('\n', i - 1) + 1) == i) // npos + 1 wraps to 0
            {
                const size_t end = std::min(line.find('\n', i), line.size());
                add(i, end, token::heading);
                i = end;
                continue;
            }
            const bool after_word = i > 0 && is_word(text[i - 1]);
            if (const auto b = std::ranges::find_if(lang.blocks, [&](const block& b) { return starts(b.open); });
                b != lang.blocks.end())
            {
                // searched line by line, each only up to its cap, so a block never reads across a huge line
                size_t end = i + b->open.size();
                // raw loop: one line of the block per pass
                for (;;)
                {
                    const std::string_view capped_line = text.substr(0, std::min(cap, text.size()));
                    const size_t newline = capped_line.find('\n', end);
                    const size_t close = capped_line.substr(0, newline).find(b->close, end);
                    if (close != std::string_view::npos)
                    {
                        end = close + b->close.size();
                        break;
                    }
                    if (newline == std::string_view::npos)
                    {
                        end = capped_line.size(); // unclosed: runs to the cap or the end of the text
                        break;
                    }
                    end = newline + 1;
                    cap = end + LINE_CAP;
                }
                add(i, end, b->kind);
                i = end;
                continue;
            }
            if (std::ranges::any_of(lang.line_comments, starts))
            {
                const size_t end = std::min(line.find('\n', i), line.size());
                add(i, end, token::comment);
                i = end;
                continue;
            }
            // an apostrophe inside a word ("don't") opens nothing
            if (c == '"' || c == '`' || (c == '\'' && !after_word))
            {
                // unclosed runs to the line end, so no token looks ahead past a later span's start
                const size_t close = string_end(line, i);
                const size_t end = close != std::string_view::npos ? close : std::min(line.find('\n', i), line.size());
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
            while (end < line.size() &&
                   (is_word(line[end]) || (line[end] == '.' && std::isdigit(static_cast<unsigned char>(c)) != 0)))
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
                const size_t next = line.find_first_not_of(" \t", end);
                if (next != std::string_view::npos && line[next] == '(')
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
            if (a > j.from && fence == std::string_view::npos && j.stop(a, a + LINE_CAP))
            {
                return true;
            }
            const size_t b = std::min(text.find('\n', a), text.size());
            const size_t shown = std::min(b, a + LINE_CAP); // highlighting stops at the cap
            const std::string_view line = text.substr(a, shown - a);
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
                    add(fence, shown, token::string);
                    fence = std::string_view::npos;
                }
            }
            else if (hashes >= 1 && hashes <= 6 && (hashes == s.size() || s[hashes] == ' '))
            {
                add(a, shown, token::heading);
            }
            else if (s.starts_with('>'))
            {
                add(a, shown, token::comment);
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
                scan_markdown_inline(j, from, shown);
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
        size_t cap = j.cap;
        for (size_t i = j.from; i < text.size();) // raw loop: each field decides how far to advance
        {
            if (text[i] == '\n')
            {
                column = 0;
                ++i;
                cap = i + LINE_CAP;
                if (i < text.size() && j.stop(i, cap))
                {
                    return true;
                }
                continue;
            }
            if (i >= cap)
            {
                i = std::min(text.find('\n', i), text.size()); // nothing more is highlighted on this line
                continue;
            }
            const std::string_view line = text.substr(0, std::min(cap, text.size())); // fields end at the cap
            size_t end = i;
            if (text[i] == '"')
            {
                end = string_end(line, i); // "" inside reads as a close and reopen, which ends at the same place
                end = end == std::string_view::npos ? std::min(line.find('\n', i), line.size()) : end;
            }
            end = std::min({line.find(',', end), line.find('\n', end), line.size()});
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
