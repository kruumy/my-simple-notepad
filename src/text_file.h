#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
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

    // when another program changes a file, at least one of these changes
    struct file_stamp
    {
        std::uint64_t write_time = 0;
        std::uint64_t size = 0;

        bool operator==(const file_stamp&) const = default;
    };

    // text is always utf-8 with '\n' line endings in memory; the original format is restored on save
    struct text_file
    {
        std::string text;
        text_encoding encoding = text_encoding::utf8;
        bool crlf = true;
        std::optional<file_stamp> stamp; // of the file on disk as last loaded or saved; none if unknown

        static text_file load(const std::filesystem::path& path);
        // body is written in order; two runs let the editor save without joining its text first.
        // returns the stamp of the written file, or none if it can't be read back
        static std::optional<file_stamp> save(const std::filesystem::path& path, std::array<std::string_view, 2> body,
                                              text_encoding encoding, bool crlf);
        [[nodiscard]] static std::optional<file_stamp> stamp_of(const std::filesystem::path& path); // none if missing

        [[nodiscard]] static const char* encoding_name(text_encoding encoding);
        [[nodiscard]] static std::string to_utf8(std::wstring_view wide);
    };
} // namespace simple_notepad
