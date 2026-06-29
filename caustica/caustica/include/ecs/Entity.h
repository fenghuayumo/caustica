#pragma once

#include <entt/entity/entity.hpp>

namespace caustica::ecs
{

using Entity = entt::entity;

inline constexpr Entity NullEntity = entt::null;

[[nodiscard]] inline bool isValid(Entity entity)
{
    return entity != entt::null;
}

} // namespace caustica::ecs
