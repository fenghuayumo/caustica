#pragma once

#include <ecs/Entity.h>
#include <math/math.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace caustica
{

class App;

// Visual URDF FK (scheme A). jointNames lists revolute / continuous / prismatic
// joints in URDF order. q is radians (revolute/continuous) or metres (prismatic).
[[nodiscard]] std::vector<std::string> jointNames(App& app, ecs::Entity robot);
bool setJointPositions(App& app, ecs::Entity robot, const float* positions, size_t count);
bool getLinkPose(
    App& app,
    ecs::Entity robot,
    std::string_view linkName,
    math::double3& translation,
    math::dquat& rotation);
[[nodiscard]] ecs::Entity attachCamera(
    App& app,
    ecs::Entity robot,
    const std::string& name,
    const std::string& link,
    const math::double3& localTranslation,
    const math::dquat& localRotation);

} // namespace caustica
