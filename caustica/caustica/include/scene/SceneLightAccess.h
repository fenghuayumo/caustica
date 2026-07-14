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
[[nodiscard]] int getLightType(const LightRenderProxy& proxy);
[[nodiscard]] int getLightType(const DirectionalLightComponent&);
[[nodiscard]] int getLightType(const SpotLightComponent&);
[[nodiscard]] int getLightType(const PointLightComponent&);
[[nodiscard]] int getLightType(const EnvironmentLightComponent&);
[[nodiscard]] SceneContentFlags getLightContentFlags();

[[nodiscard]] dm::double3 getLightPosition(const dm::daffine3& globalTransform);
[[nodiscard]] dm::double3 getLightDirection(const dm::daffine3& globalTransform);
[[nodiscard]] bool isInfiniteLight(const LightData& data);
[[nodiscard]] bool isInfiniteLight(const LightRenderProxy& proxy);

[[nodiscard]] LightData toLightData(const DirectionalLightComponent& component);
[[nodiscard]] LightData toLightData(const SpotLightComponent& component);
[[nodiscard]] LightData toLightData(const PointLightComponent& component);
[[nodiscard]] LightData toLightData(const EnvironmentLightComponent& component);

void fillLightConstants(
    dm::float3 color, const LightData& data, const dm::daffine3& globalTransform, LightConstants& lightConstants);
void fillLightConstants(const LightRenderProxy& proxy, LightConstants& lightConstants);
[[nodiscard]] bool tryFillLightConstants(
    const ecs::World& world, ecs::Entity entity, const dm::daffine3& globalTransform, LightConstants& lightConstants);

[[nodiscard]] bool hasAnyLightComponent(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] bool setLightProperty(
    ecs::World& world, ecs::Entity entity, const std::string& propName, const dm::float4& value);

[[nodiscard]] DirectionalLightComponent* tryGetDirectionalLight(ecs::World& world, ecs::Entity entity);
[[nodiscard]] SpotLightComponent* tryGetSpotLight(ecs::World& world, ecs::Entity entity);
[[nodiscard]] PointLightComponent* tryGetPointLight(ecs::World& world, ecs::Entity entity);
[[nodiscard]] EnvironmentLightComponent* tryGetEnvironmentLight(ecs::World& world, ecs::Entity entity);

[[nodiscard]] const DirectionalLightComponent* tryGetDirectionalLight(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] const SpotLightComponent* tryGetSpotLight(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] const PointLightComponent* tryGetPointLight(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] const EnvironmentLightComponent* tryGetEnvironmentLight(const ecs::World& world, ecs::Entity entity);

[[nodiscard]] DirectionalLightData* tryGetDirectionalLightData(LightData& data);
[[nodiscard]] SpotLightData* tryGetSpotLightData(LightData& data);
[[nodiscard]] PointLightData* tryGetPointLightData(LightData& data);
[[nodiscard]] EnvironmentLightData* tryGetEnvironmentLightData(LightData& data);

[[nodiscard]] const DirectionalLightData* tryGetDirectionalLightData(const LightData& data);
[[nodiscard]] const SpotLightData* tryGetSpotLightData(const LightData& data);
[[nodiscard]] const PointLightData* tryGetPointLightData(const LightData& data);
[[nodiscard]] const EnvironmentLightData* tryGetEnvironmentLightData(const LightData& data);

[[nodiscard]] ecs::Entity findEnvironmentLightEntity(const ecs::World& world, const std::vector<ecs::Entity>& lightEntities);
[[nodiscard]] const std::string& getEnvironmentLightPath(const EnvironmentLightComponent& component);

} // namespace caustica::scene
