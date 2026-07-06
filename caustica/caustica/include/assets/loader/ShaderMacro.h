#pragma once

#include <string>

namespace caustica
{

struct ShaderMacro
{
    std::string name;
    std::string definition;

    ShaderMacro(const std::string& name_, const std::string& definition_)
        : name(name_)
        , definition(definition_)
    { }

    bool operator==(const ShaderMacro& other) const
    {
        return name == other.name && definition == other.definition;
    }
};

} // namespace caustica
