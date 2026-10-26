#include "text_file.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
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

    text_file text_file::load(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            throw std::runtime_error(std::format("Cannot open \"{}\"", from_wide(path.filename().wstring(), CP_UTF8)));
        }
        std::string bytes(static_cast<size_t>(std::filesystem::file_size(path)), '\0');
        in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<size_t>(in.gcount())); // the file may have shrunk since its size was read

        text_file file;
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

    void text_file::save(const std::filesystem::path& path) const
    {
        std::string body;
        if (crlf)
        {
            body.reserve(text.size() + (text.size() / 32));
            for (const char c : text) // raw loop: one-pass expansion is clearer than an algorithm here
            {
                if (c == '\n')
                {
                    body += '\r';
                }
                body += c;
            }
        }
        else
        {
            body = text;
        }

        std::string bytes;
        switch (encoding)
        {
        case text_encoding::utf8:
            bytes = std::move(body);
            break;
        case text_encoding::utf8_bom:
            bytes = "\xEF\xBB\xBF" + body;
            break;
        case text_encoding::ansi:
            bytes = from_wide(to_wide(body, CP_UTF8), CP_ACP);
            break;
        case text_encoding::utf16_le:
        case text_encoding::utf16_be: {
            std::wstring wide = to_wide(body, CP_UTF8);
            if (encoding == text_encoding::utf16_be)
            {
                wide = swap_bytes(std::move(wide));
            }
            bytes = encoding == text_encoding::utf16_le ? "\xFF\xFE" : "\xFE\xFF";
            bytes.append(reinterpret_cast<const char*>(wide.data()), wide.size() * 2);
            break;
        }
        }

        // write to a temp file first so a failed save never truncates the original
        std::filesystem::path temp = path;
        temp += L".~tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!out.flush())
            {
                out.close();
                std::error_code ec;
                std::filesystem::remove(temp, ec);
                throw std::runtime_error(
                    std::format("Cannot write \"{}\"", from_wide(path.filename().wstring(), CP_UTF8)));
            }
        }
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            throw std::runtime_error(
                std::format("Cannot replace \"{}\"", from_wide(path.filename().wstring(), CP_UTF8)));
        }
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
