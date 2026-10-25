#pragma once

#include <charconv>
#include <filesystem>
#include <format>
#include <map>
#include <string>
#include <type_traits>

namespace simple_notepad
{
    // minimal ini: [section] headers and key=value lines; anything else is ignored
    class ini
    {
      public:
        static ini load(const std::filesystem::path& path);
        void save(const std::filesystem::path& path) const;

        // leaves out unchanged when the key is missing or doesn't parse, so it keeps its default
        template <class T>
            requires std::is_arithmetic_v<T>
        void get(const std::string& section, const std::string& key, T& out) const
        {
            const auto s = sections_.find(section);
            if (s == sections_.end())
            {
                return;
            }
            const auto v = s->second.find(key);
            if (v == s->second.end())
            {
                return;
            }
            const std::string& text = v->second;
            if constexpr (std::is_same_v<T, bool>)
            {
                int value = 0;
                if (std::from_chars(text.data(), text.data() + text.size(), value).ec == std::errc{})
                {
                    out = value != 0;
                }
            }
            else
            {
                T value{};
                if (std::from_chars(text.data(), text.data() + text.size(), value).ec == std::errc{})
                {
                    out = value;
                }
            }
        }

        template <class T>
            requires std::is_arithmetic_v<T>
        void set(const std::string& section, const std::string& key, T value)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                sections_[section][key] = value ? "1" : "0";
            }
            else
            {
                sections_[section][key] = std::format("{}", value);
            }
        }

      private:
        std::map<std::string, std::map<std::string, std::string>> sections_;
    };
} // namespace simple_notepad
