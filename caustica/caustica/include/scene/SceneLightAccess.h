#pragma once

#include <ecs/Entity.h>
#include <ecs/World.h>
#include <math/math.h>
#include <scene/SceneEcs.h>

#include <string>
#include <vector>

struct LightConstants;

namespace caustica::scene
{

struct LightRenderProxy;

[[nodiscard]] int getLightType(const LightData& data);
[[nodiscard]] int getLightType(const LightComponent& component);
[[nodiscard]] int getLightType(const LightRenderProxy& proxy);
[[nodiscard]] SceneContentFlags getLightContentFlags();

[[nodiscard]] dm::double3 getLightPosition(const dm::daffine3& globalTransform);
[[nodiscard]] dm::double3 getLightDirection(const dm::daffine3& globalTransform);
[[nodiscard]] bool isInfiniteLight(const LightData& data);
[[nodiscard]] bool isInfiniteLight(const LightComponent& component);
[[nodiscard]] bool isInfiniteLight(const LightRenderProxy& proxy);

void fillLightConstants(
    dm::float3 color, const LightData& data, const dm::daffine3& globalTransform, LightConstants& lightConstants);
void fillLightConstants(
    const LightComponent& component, const dm::daffine3& globalTransform, LightConstants& lightConstants);
void fillLightConstants(const LightRenderProxy& proxy, LightConstants& lightConstants);
[[nodiscard]] bool setLightProperty(LightComponent& component, const std::string& propName, const dm::float4& value);

[[nodiscard]] const LightComponent* tryGetLight(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] LightComponent* tryGetLight(ecs::World& world, ecs::Entity entity);

[[nodiscard]] DirectionalLightData* tryGetDirectionalLightData(LightData& data);
[[nodiscard]] SpotLightData* tryGetSpotLightData(LightData& data);
[[nodiscard]] PointLightData* tryGetPointLightData(LightData& data);
[[nodiscard]] EnvironmentLightData* tryGetEnvironmentLightData(LightData& data);

[[nodiscard]] const DirectionalLightData* tryGetDirectionalLightData(const LightData& data);
[[nodiscard]] const SpotLightData* tryGetSpotLightData(const LightData& data);
[[nodiscard]] const PointLightData* tryGetPointLightData(const LightData& data);
[[nodiscard]] const EnvironmentLightData* tryGetEnvironmentLightData(const LightData& data);

[[nodiscard]] DirectionalLightData* tryGetDirectionalLightData(LightComponent& component);
[[nodiscard]] SpotLightData* tryGetSpotLightData(LightComponent& component);
[[nodiscard]] PointLightData* tryGetPointLightData(LightComponent& component);
[[nodiscard]] EnvironmentLightData* tryGetEnvironmentLightData(LightComponent& component);

[[nodiscard]] const DirectionalLightData* tryGetDirectionalLightData(const LightComponent& component);
[[nodiscard]] const SpotLightData* tryGetSpotLightData(const LightComponent& component);
[[nodiscard]] const PointLightData* tryGetPointLightData(const LightComponent& component);
[[nodiscard]] const EnvironmentLightData* tryGetEnvironmentLightData(const LightComponent& component);

[[nodiscard]] ecs::Entity findEnvironmentLightEntity(const ecs::World& world, const std::vector<ecs::Entity>& lightEntities);
[[nodiscard]] const std::string& getEnvironmentLightPath(const LightComponent& component);

} // namespace caustica::scene
