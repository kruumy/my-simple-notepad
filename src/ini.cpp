#include "ini.h"
#include <fstream>

namespace simple_notepad
{
    ini ini::load(const std::filesystem::path& path)
    {
        ini file;
        std::ifstream in(path);
        std::string section;
        std::string line;
        while (std::getline(in, line))
        {
            if (line.ends_with('\r')) // edited by hand with windows line endings
            {
                line.pop_back();
            }
            if (line.starts_with('[') && line.ends_with(']'))
            {
                section = line.substr(1, line.size() - 2);
            }
            else if (const size_t eq = line.find('='); eq != std::string::npos)
            {
                file.sections_[section][line.substr(0, eq)] = line.substr(eq + 1);
            }
        }
        return file;
    }

    void ini::save(const std::filesystem::path& path) const
    {
        std::ofstream out(path, std::ios::trunc);
        for (const auto& [section, values] : sections_)
        {
            out << '[' << section << "]\n";
            for (const auto& [key, value] : values)
            {
                out << key << '=' << value << '\n';
            }
        }
    }
} // namespace simple_notepad
