#pragma once

#include <ecs/Entity.h>
#include <ecs/World.h>
#include <math/math.h>
#include <scene/SceneEcs.h>
#include <scene/SceneObjects.h>

#include <memory>
#include <string>

struct LightConstants;

namespace caustica::scene
{

[[nodiscard]] int GetLightType(const LightComponent& component);
[[nodiscard]] SceneContentFlags GetLightContentFlags();

[[nodiscard]] dm::double3 GetLightPosition(const dm::daffine3& globalTransform);
[[nodiscard]] dm::double3 GetLightDirection(const dm::daffine3& globalTransform);
[[nodiscard]] bool IsInfiniteLight(const LightComponent& component);

void FillLightConstants(
    const LightComponent& component, const dm::daffine3& globalTransform, LightConstants& lightConstants);
[[nodiscard]] bool SetLightProperty(LightComponent& component, const std::string& propName, const dm::float4& value);

void SetLightPosition(SceneEntityWorld& world, ecs::Entity entity, const dm::double3& position);
void SetLightDirection(SceneEntityWorld& world, ecs::Entity entity, const dm::double3& direction);

void InitializeLightComponent(LightComponent& component, const Light& light);
void InitializeLightComponent(LightComponent& component, const std::shared_ptr<Light>& light);

[[nodiscard]] const LightComponent* TryGetLight(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] LightComponent* TryGetLight(ecs::World& world, ecs::Entity entity);

[[nodiscard]] DirectionalLightData* TryGetDirectionalLightData(LightComponent& component);
[[nodiscard]] SpotLightData* TryGetSpotLightData(LightComponent& component);
[[nodiscard]] PointLightData* TryGetPointLightData(LightComponent& component);
[[nodiscard]] EnvironmentLightData* TryGetEnvironmentLightData(LightComponent& component);

[[nodiscard]] const DirectionalLightData* TryGetDirectionalLightData(const LightComponent& component);
[[nodiscard]] const SpotLightData* TryGetSpotLightData(const LightComponent& component);
[[nodiscard]] const PointLightData* TryGetPointLightData(const LightComponent& component);
[[nodiscard]] const EnvironmentLightData* TryGetEnvironmentLightData(const LightComponent& component);

[[nodiscard]] ecs::Entity FindEnvironmentLightEntity(const ecs::World& world, const std::vector<ecs::Entity>& lightEntities);
[[nodiscard]] const std::string& GetEnvironmentLightPath(const LightComponent& component);

} // namespace caustica::scene
