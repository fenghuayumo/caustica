#pragma once

#include <ecs/Entity.h>
#include <math/math.h>

#include <optional>

namespace caustica
{

class App;

// App-facing transform / visibility edits. Logic-thread ECS only; Extract publishes proxies.
bool setEntityLocalTransform(
    App& app,
    ecs::Entity entity,
    const std::optional<dm::double3>& translation = std::nullopt,
    const std::optional<dm::dquat>& rotation = std::nullopt,
    const std::optional<dm::double3>& scaling = std::nullopt);
bool setEntityTranslation(App& app, ecs::Entity entity, const dm::double3& translation);
bool setEntityVisible(App& app, ecs::Entity entity, bool visible);

} // namespace caustica
