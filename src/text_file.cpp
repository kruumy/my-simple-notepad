#include "text_file.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>

namespace simple_notepad
{
    static std::wstring to_wide(std::string_view bytes, UINT code_page)
    {
        if (bytes.empty())
        {
            return {};
        }
        const int len = MultiByteToWideChar(code_page, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (len == 0)
        {
            throw std::runtime_error("The text is too large to convert"); // an empty result would lose it
        }
        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(code_page, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), len);
        return out;
    }

    static std::string from_wide(std::wstring_view wide, UINT code_page)
    {
        if (wide.empty())
        {
            return {};
        }
        const int len =
            WideCharToMultiByte(code_page, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (len == 0)
        {
            throw std::runtime_error("The text is too large to convert"); // an empty result would lose it
        }
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(code_page, 0, wide.data(), static_cast<int>(wide.size()), out.data(), len, nullptr,
                            nullptr);
        return out;
    }

    static bool is_valid_utf8(std::string_view bytes)
    {
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr,
                                   0) != 0 ||
               bytes.empty();
    }

    static std::wstring swap_bytes(std::wstring wide)
    {
        std::ranges::transform(wide, wide.begin(),
                               [](wchar_t c) { return static_cast<wchar_t>(((c & 0xFF) << 8) | ((c >> 8) & 0xFF)); });
        return wide;
    }

    using handle = std::unique_ptr<void, decltype(&CloseHandle)>;
    constexpr DWORD MAX_IO_BYTES = 1U << 30; // ReadFile and WriteFile take a DWORD count
    constexpr size_t STAGE_BYTES = 1U << 20; // text expanded per write when saving with crlf

    static std::uint64_t join(DWORD high, DWORD low)
    {
        return (static_cast<std::uint64_t>(high) << 32) | low;
    }

    // null instead of INVALID_HANDLE_VALUE, so the owner closes only real handles
    static handle owned(HANDLE h)
    {
        return {h == INVALID_HANDLE_VALUE ? nullptr : h, &CloseHandle};
    }

    static std::runtime_error file_error(const char* what, const std::filesystem::path& path)
    {
        return std::runtime_error(std::format("{} \"{}\"", what, from_wide(path.filename().wstring(), CP_UTF8)));
    }

    text_file text_file::load(const std::filesystem::path& path)
    {
        // others may keep reading and writing it; a later change shows up in the stamp
        const handle in =
            owned(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!in || GetFileInformationByHandle(in.get(), &info) == FALSE)
        {
            throw file_error("Cannot open", path);
        }
        // sized without zero-filling first, since ReadFile writes every byte kept; the callback must not throw
        bool read_ok = true;
        std::string bytes;
        bytes.resize_and_overwrite(
            static_cast<size_t>(join(info.nFileSizeHigh, info.nFileSizeLow)), [&](char* data, size_t size) {
                size_t read = 0;
                while (read < size) // raw loop: one ReadFile per gigabyte
                {
                    DWORD got = 0;
                    const auto want = static_cast<DWORD>(std::min<size_t>(size - read, MAX_IO_BYTES));
                    read_ok = ReadFile(in.get(), data + read, want, &got, nullptr) != FALSE;
                    if (!read_ok || got == 0)
                    {
                        break; // got 0: the file shrank since its size was read
                    }
                    read += got;
                }
                return read;
            });
        if (!read_ok)
        {
            throw file_error("Cannot read", path);
        }

        text_file file;
        file.stamp =
            file_stamp{.write_time = join(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime),
                       .size = join(info.nFileSizeHigh, info.nFileSizeLow)};
        const std::string_view view = bytes;
        if (view.starts_with("\xEF\xBB\xBF"))
        {
            file.encoding = text_encoding::utf8_bom;
            file.text = std::move(bytes); // moved, not copied: big files would briefly need twice the memory
            file.text.erase(0, 3);
        }
        else if (view.starts_with("\xFF\xFE") || view.starts_with("\xFE\xFF"))
        {
            file.encoding = view[0] == '\xFF' ? text_encoding::utf16_le : text_encoding::utf16_be;
            const std::string_view body = view.substr(2);
            std::wstring wide(body.size() / 2, L'\0');
            std::memcpy(wide.data(), body.data(), wide.size() * 2);
            if (file.encoding == text_encoding::utf16_be)
            {
                wide = swap_bytes(std::move(wide));
            }
            if (body.size() % 2 != 0)
            {
                wide += L'�'; // a truncated last unit shows up instead of vanishing
            }
            file.text = from_wide(wide, CP_UTF8);
        }
        else if (is_valid_utf8(view))
        {
            file.text = std::move(bytes);
        }
        else
        {
            file.encoding = text_encoding::ansi;
            file.text = from_wide(to_wide(view, CP_ACP), CP_UTF8);
        }

        // new files default to crlf; existing files keep whatever they used
        file.crlf = file.text.find('\n') == std::string::npos || file.text.find("\r\n") != std::string::npos;
        std::erase(file.text, '\r');
        return file;
    }

    // appends text, writing each '\n' as "\r\n" when crlf is set
    static void append_lines(std::string& out, std::string_view text, bool crlf)
    {
        if (!crlf)
        {
            out.append(text);
            return;
        }
        for (size_t from = 0;;) // raw loop: copies the stretches between newlines whole
        {
            const size_t newline = text.find('\n', from);
            out.append(text.substr(from, newline - from));
            if (newline == std::string_view::npos)
            {
                return;
            }
            out += "\r\n";
            from = newline + 1;
        }
    }

    // conversions need the text as one run, with its line breaks in their saved form
    static std::string joined_lines(std::array<std::string_view, 2> body, bool crlf)
    {
        std::string joined;
        joined.reserve(body[0].size() + body[1].size());
        append_lines(joined, body[0], crlf);
        append_lines(joined, body[1], crlf);
        return joined;
    }

    // utf-8 writes the runs as they are, expanding crlf through a small buffer, so a big file is never copied whole
    std::optional<file_stamp> text_file::save(const std::filesystem::path& path, std::array<std::string_view, 2> body,
                                              text_encoding encoding, bool crlf)
    {
        std::string_view prefix;
        std::string converted;
        switch (encoding)
        {
        case text_encoding::utf8:
            break;
        case text_encoding::utf8_bom:
            prefix = "\xEF\xBB\xBF";
            break;
        case text_encoding::ansi:
            converted = from_wide(to_wide(joined_lines(body, crlf), CP_UTF8), CP_ACP);
            body = {converted, {}};
            crlf = false; // already expanded
            break;
        case text_encoding::utf16_le:
        case text_encoding::utf16_be: {
            std::wstring wide = to_wide(joined_lines(body, crlf), CP_UTF8);
            if (encoding == text_encoding::utf16_be)
            {
                wide = swap_bytes(std::move(wide));
            }
            prefix = encoding == text_encoding::utf16_le ? "\xFF\xFE" : "\xFE\xFF";
            converted.assign(reinterpret_cast<const char*>(wide.data()), wide.size() * 2);
            body = {converted, {}};
            crlf = false; // already expanded
            break;
        }
        }

        // write to a temp file first so a failed save never truncates the original
        std::filesystem::path temp = path;
        temp += L".~tmp";
        handle out =
            owned(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!out)
        {
            throw file_error("Cannot write", path);
        }
        bool written = true;
        const auto put = [&](std::string_view bytes) {
            while (written && !bytes.empty()) // raw loop: one WriteFile per gigabyte
            {
                DWORD done = 0;
                written =
                    WriteFile(out.get(), bytes.data(), static_cast<DWORD>(std::min<size_t>(bytes.size(), MAX_IO_BYTES)),
                              &done, nullptr) != FALSE;
                bytes.remove_prefix(done);
            }
        };
        put(prefix);
        std::string staged;
        for (const std::string_view run : body)
        {
            if (!crlf)
            {
                put(run);
                continue;
            }
            for (size_t at = 0; at < run.size(); at += STAGE_BYTES) // raw loop: one staged chunk at a time
            {
                staged.clear();
                append_lines(staged, run.substr(at, STAGE_BYTES), true);
                put(staged);
            }
        }
        // on disk before the rename makes it the file, or a crash just after could leave it empty
        written = written && FlushFileBuffers(out.get()) != FALSE;
        out.reset();
        std::error_code ec;
        if (!written)
        {
            std::filesystem::remove(temp, ec);
            throw file_error("Cannot write", path);
        }
        if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
        {
            std::filesystem::remove(temp, ec);
            throw file_error("Cannot replace", path);
        }
        return stamp_of(path);
    }

    std::optional<file_stamp> text_file::stamp_of(const std::filesystem::path& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == FALSE)
        {
            return std::nullopt;
        }
        return file_stamp{.write_time = join(data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime),
                          .size = join(data.nFileSizeHigh, data.nFileSizeLow)};
    }

    const char* text_file::encoding_name(text_encoding encoding)
    {
        switch (encoding)
        {
        case text_encoding::utf8:
            return "UTF-8";
        case text_encoding::utf8_bom:
            return "UTF-8 BOM";
        case text_encoding::utf16_le:
            return "UTF-16 LE";
        case text_encoding::utf16_be:
            return "UTF-16 BE";
        case text_encoding::ansi:
            return "ANSI";
        }
        return "";
    }

    std::string text_file::to_utf8(std::wstring_view wide)
    {
        return from_wide(wide, CP_UTF8);
    }
} // namespace simple_notepad
