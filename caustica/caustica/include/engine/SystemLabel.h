#pragma once

#include <typeinfo>
#include <vector>

namespace caustica
{

// Compile-time system identity for schedule ordering (Bevy-style labels).
// Display name is diagnostic-only; graph edges use type identity.
struct SystemLabel
{
    const std::type_info* type = nullptr;
    const char* name = "";

    [[nodiscard]] bool valid() const { return type != nullptr; }

    [[nodiscard]] bool operator==(const SystemLabel& other) const
    {
        return type && other.type && *type == *other.type;
    }

    [[nodiscard]] bool operator!=(const SystemLabel& other) const
    {
        return !(*this == other);
    }
};

template<typename Tag>
[[nodiscard]] SystemLabel systemLabel()
{
    return SystemLabel{ &typeid(Tag), Tag::name };
}

struct AppSystemOrdering
{
    std::vector<SystemLabel> before;
    std::vector<SystemLabel> after;
    SystemLabel set{};

    template<typename... Tags>
    AppSystemOrdering& runAfter()
    {
        (after.push_back(systemLabel<Tags>()), ...);
        return *this;
    }

    template<typename... Tags>
    AppSystemOrdering& runBefore()
    {
        (before.push_back(systemLabel<Tags>()), ...);
        return *this;
    }

    template<typename SetTag>
    AppSystemOrdering& inSet()
    {
        set = systemLabel<SetTag>();
        return *this;
    }
};

} // namespace caustica
