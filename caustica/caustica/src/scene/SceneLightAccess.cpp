#include <scene/SceneLightAccess.h>

#include <scene/SceneEcs.h>

#include <math/math.h>

using namespace caustica;
using namespace caustica::math;

#include <shaders/light_cb.h>
#include <shaders/light_types.h>

namespace caustica::scene
{

namespace
{

void FillCommonLightConstants(const LightComponent& component, LightConstants& lightConstants)
{
    lightConstants.color = component.color;
    lightConstants.shadowCascades = int4(-1);
    lightConstants.perObjectShadows = int4(-1);
    lightConstants.shadowChannel = int4(component.shadowChannel, -1, -1, -1);
    if (component.shadowMap)
        lightConstants.outOfBoundsShadow = component.shadowMap->IsLitOutOfBounds() ? 1.f : 0.f;
    else
        lightConstants.outOfBoundsShadow = 1.f;
}

} // namespace

int GetLightType(const LightComponent& component)
{
    switch (component.data.index())
    {
    case 0: return LightType_Directional;
    case 1: return LightType_Spot;
    case 2: return LightType_Point;
    case 3: return LightType_Environment;
    default: return LightType_None;
    }
}

SceneContentFlags GetLightContentFlags()
{
    return SceneContentFlags::Lights;
}

dm::double3 GetLightPosition(const dm::daffine3& globalTransform)
{
    return globalTransform.m_translation;
}

dm::double3 GetLightDirection(const dm::daffine3& globalTransform)
{
    return -normalize(dm::double3(globalTransform.m_linear.row2));
}

bool IsInfiniteLight(const LightComponent& component)
{
    switch (GetLightType(component))
    {
    case LightType_Directional: return true;
    case LightType_Environment: return true;
    default: return false;
    }
}

void FillLightConstants(
    const LightComponent& component, const dm::daffine3& globalTransform, LightConstants& lightConstants)
{
    FillCommonLightConstants(component, lightConstants);

    switch (GetLightType(component))
    {
    case LightType_Directional:
    {
        const auto& directional = std::get<DirectionalLightData>(component.data);
        lightConstants.lightType = LightType_Directional;
        lightConstants.direction = float3(normalize(GetLightDirection(globalTransform)));
        const float clampedAngularSize = clamp(directional.angularSize, 0.f, 90.f);
        lightConstants.angularSizeOrInvRange = dm::radians(clampedAngularSize);
        lightConstants.intensity = directional.irradiance;
        break;
    }
    case LightType_Spot:
    {
        const auto& spot = std::get<SpotLightData>(component.data);
        lightConstants.lightType = LightType_Spot;
        lightConstants.direction = float3(GetLightDirection(globalTransform));
        lightConstants.position = float3(GetLightPosition(globalTransform));
        lightConstants.radius = spot.radius;
        lightConstants.angularSizeOrInvRange = (spot.range <= 0.f) ? 0.f : 1.f / spot.range;
        lightConstants.intensity = spot.intensity;
        lightConstants.color = component.color;
        lightConstants.innerAngle = dm::radians(spot.innerAngle);
        lightConstants.outerAngle = dm::radians(spot.outerAngle);
        break;
    }
    case LightType_Point:
    {
        const auto& point = std::get<PointLightData>(component.data);
        lightConstants.lightType = LightType_Point;
        lightConstants.position = float3(GetLightPosition(globalTransform));
        lightConstants.radius = point.radius;
        lightConstants.angularSizeOrInvRange = (point.range <= 0.f) ? 0.f : 1.f / point.range;
        lightConstants.intensity = point.intensity;
        lightConstants.color = component.color;
        break;
    }
    case LightType_Environment:
        lightConstants.intensity = 0.0f;
        lightConstants.color = { 0, 0, 0 };
        break;
    default:
        break;
    }
}

bool SetLightProperty(LightComponent& component, const std::string& propName, const dm::float4& value)
{
    if (propName == "color")
    {
        component.color = value.xyz();
        return true;
    }

    switch (component.data.index())
    {
    case 0:
    {
        auto& directional = std::get<DirectionalLightData>(component.data);
        if (propName == "irradiance") { directional.irradiance = value.x; return true; }
        if (propName == "angularSize") { directional.angularSize = value.x; return true; }
        break;
    }
    case 1:
    {
        auto& spot = std::get<SpotLightData>(component.data);
        if (propName == "intensity") { spot.intensity = value.x; return true; }
        if (propName == "radius") { spot.radius = value.x; return true; }
        if (propName == "range") { spot.range = value.x; return true; }
        if (propName == "innerAngle") { spot.innerAngle = value.x; return true; }
        if (propName == "outerAngle") { spot.outerAngle = value.x; return true; }
        break;
    }
    case 2:
    {
        auto& point = std::get<PointLightData>(component.data);
        if (propName == "intensity") { point.intensity = value.x; return true; }
        if (propName == "radius") { point.radius = value.x; return true; }
        if (propName == "range") { point.range = value.x; return true; }
        break;
    }
    default:
        break;
    }

    return false;
}

void SetLightPosition(SceneEntityWorld& world, ecs::Entity entity, const dm::double3& position)
{
    ecs::Entity parentEntity = ecs::NullEntity;
    if (const auto* parent = world.world().get<ParentComponent>(entity))
        parentEntity = parent->parent;

    dm::daffine3 parentToWorld = dm::daffine3::identity();
    if (ecs::isValid(parentEntity))
    {
        if (const auto* globalTransform = world.world().get<GlobalTransformComponent>(parentEntity))
            parentToWorld = globalTransform->transform;
    }

    const dm::double3 translation = inverse(parentToWorld).transformPoint(position);
    world.setTranslation(entity, translation);
}

void SetLightDirection(SceneEntityWorld& world, ecs::Entity entity, const dm::double3& direction)
{
    ecs::Entity parentEntity = ecs::NullEntity;
    if (const auto* parent = world.world().get<ParentComponent>(entity))
        parentEntity = parent->parent;

    dm::daffine3 parentToWorld = dm::daffine3::identity();
    if (ecs::isValid(parentEntity))
    {
        if (const auto* globalTransform = world.world().get<GlobalTransformComponent>(parentEntity))
            parentToWorld = globalTransform->transform;
    }

    const dm::daffine3 worldToLocal = lookatZ(direction);
    const dm::daffine3 localToParent = inverse(worldToLocal * parentToWorld);

    dm::dquat rotation;
    dm::double3 scaling;
    decomposeAffine<double>(localToParent, nullptr, &rotation, &scaling);

    world.setLocalTransform(entity, nullptr, &rotation, &scaling);
}

void InitializeLightComponent(LightComponent& component, const Light& light)
{
    component.shadowMap = light.shadowMap;
    component.shadowChannel = light.shadowChannel;
    component.color = light.color;
    component.lightLink = light.LightLink;
    component.proxies = light.Proxies;

    if (const auto* directional = dynamic_cast<const DirectionalLight*>(&light))
    {
        component.data = DirectionalLightData{
            directional->irradiance,
            directional->angularSize,
            directional->perObjectShadows,
        };
    }
    else if (const auto* spot = dynamic_cast<const SpotLight*>(&light))
    {
        component.data = SpotLightData{
            spot->intensity,
            spot->radius,
            spot->range,
            spot->innerAngle,
            spot->outerAngle,
        };
    }
    else if (const auto* point = dynamic_cast<const PointLight*>(&light))
    {
        component.data = PointLightData{
            point->intensity,
            point->radius,
            point->range,
        };
    }
    else if (const auto* environment = dynamic_cast<const EnvironmentLight*>(&light))
    {
        component.data = EnvironmentLightData{
            environment->radianceScale,
            environment->textureIndex,
            environment->rotation,
            environment->path,
        };
    }
}

void InitializeLightComponent(LightComponent& component, const std::shared_ptr<Light>& light)
{
    if (light)
        InitializeLightComponent(component, *light);
}

const LightComponent* TryGetLight(const ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<LightComponent>(entity);
}

LightComponent* TryGetLight(ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<LightComponent>(entity);
}

DirectionalLightData* TryGetDirectionalLightData(LightComponent& component)
{
    return std::get_if<DirectionalLightData>(&component.data);
}

SpotLightData* TryGetSpotLightData(LightComponent& component)
{
    return std::get_if<SpotLightData>(&component.data);
}

PointLightData* TryGetPointLightData(LightComponent& component)
{
    return std::get_if<PointLightData>(&component.data);
}

EnvironmentLightData* TryGetEnvironmentLightData(LightComponent& component)
{
    return std::get_if<EnvironmentLightData>(&component.data);
}

const DirectionalLightData* TryGetDirectionalLightData(const LightComponent& component)
{
    return std::get_if<DirectionalLightData>(&component.data);
}

const SpotLightData* TryGetSpotLightData(const LightComponent& component)
{
    return std::get_if<SpotLightData>(&component.data);
}

const PointLightData* TryGetPointLightData(const LightComponent& component)
{
    return std::get_if<PointLightData>(&component.data);
}

const EnvironmentLightData* TryGetEnvironmentLightData(const LightComponent& component)
{
    return std::get_if<EnvironmentLightData>(&component.data);
}

ecs::Entity FindEnvironmentLightEntity(const ecs::World& world, const std::vector<ecs::Entity>& lightEntities)
{
    for (ecs::Entity entity : lightEntities)
    {
        const auto* component = TryGetLight(world, entity);
        if (component && GetLightType(*component) == LightType_Environment)
            return entity;
    }
    return ecs::NullEntity;
}

const std::string& GetEnvironmentLightPath(const LightComponent& component)
{
    static const std::string s_empty;
    if (const auto* environment = TryGetEnvironmentLightData(component))
        return environment->path;
    return s_empty;
}

} // namespace caustica::scene