#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace simple_notepad
{
    enum class text_encoding : std::uint8_t
    {
        utf8,
        utf8_bom,
        utf16_le,
        utf16_be,
        ansi,
    };

    // text is always utf-8 with '\n' line endings in memory; the original format is restored on save
    struct text_file
    {
        std::string text;
        text_encoding encoding = text_encoding::utf8;
        bool crlf = true;

        static text_file load(const std::filesystem::path& path);
        void save(const std::filesystem::path& path) const;

        [[nodiscard]] static const char* encoding_name(text_encoding encoding);
        [[nodiscard]] static std::string to_utf8(std::wstring_view wide);
    };
} // namespace simple_notepad
