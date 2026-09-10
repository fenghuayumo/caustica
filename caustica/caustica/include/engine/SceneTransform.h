#pragma once

#include <ecs/Entity.h>
#include <math/math.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace caustica
{

class App;

// App-facing transform / visibility edits. Logic-thread ECS only; Extract publishes proxies.
// Typed entity TRS (EntityPose) lives in scene/ScenePoseAccess.h.
bool setEntityLocalTransform(
    App& app,
    ecs::Entity entity,
    const std::optional<math::double3>& translation = std::nullopt,
    const std::optional<math::dquat>& rotation = std::nullopt,
    const std::optional<math::double3>& scaling = std::nullopt);
bool setEntityTranslation(App& app, ecs::Entity entity, const math::double3& translation);
bool setEntityVisible(App& app, ecs::Entity entity, bool visible);
// Null parent attaches under the scene root (world).
bool setParent(App& app, ecs::Entity entity, ecs::Entity parent);

// Batch world TRS. translations is N*3, rotationsXyzw is N*4 Hamilton xyzw,
// scales is N*3 or null (then scale stays 1). Returns how many names resolved.
// firstMissingName is set when a name cannot be found.
size_t setWorldPoses(
    App& app,
    const std::vector<std::string>& names,
    const float* translationsNx3,
    const float* rotationsXyzwNx4,
    const float* scalesNx3,
    std::string* firstMissingName = nullptr);

} // namespace caustica
