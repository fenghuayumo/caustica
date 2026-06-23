/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "core/command_line.h"

#include <algorithm>
#include <cctype>

namespace caustica
{

static std::string toLower(const std::string& s)
{
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

void CommandLine::parse(int argc, const char* const* argv)
{
    m_Options.clear();
    m_Positional.clear();

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i] ? argv[i] : "";

        if (arg.empty())
            continue;

        // Is this a key?
        if (arg[0] == '-')
        {
            // Strip leading dashes
            size_t start = (arg.size() > 1 && arg[1] == '-') ? 2 : 1;
            std::string key = arg.substr(start);

            // Check for =value form
            auto eqPos = key.find('=');
            if (eqPos != std::string::npos)
            {
                std::string value = key.substr(eqPos + 1);
                key = key.substr(0, eqPos);
                m_Options[toLower(key)] = value;
            }
            else if (i + 1 < argc && argv[i + 1] && argv[i + 1][0] != '-')
            {
                // Next arg is the value
                m_Options[toLower(key)] = argv[++i];
            }
            else
            {
                // Flag-only option
                m_Options[toLower(key)] = "";
            }
        }
        else
        {
            m_Positional.push_back(arg);
        }
    }
}

bool CommandLine::hasOption(const std::string& name) const
{
    return m_Options.find(toLower(name)) != m_Options.end();
}

const std::string& CommandLine::getOption(const std::string& name) const
{
    static const std::string s_Empty;
    auto it = m_Options.find(toLower(name));
    return (it != m_Options.end()) ? it->second : s_Empty;
}

bool CommandLine::getOptionBool(const std::string& name) const
{
    auto it = m_Options.find(toLower(name));
    if (it == m_Options.end())
        return false;

    const std::string& val = it->second;
    return val.empty()
        || val == "1"
        || val == "true"
        || val == "yes"
        || val == "on";
}

} // namespace caustica
