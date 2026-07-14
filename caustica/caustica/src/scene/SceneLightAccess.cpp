#include <scene/SceneLightAccess.h>

#include <scene/SceneEcs.h>
#include <scene/SceneRenderData.h>

#include <math/math.h>

using namespace caustica;
using namespace caustica::math;

#include <shaders/light_cb.h>
#include <shaders/light_types.h>

namespace caustica::scene
{

namespace
{

void FillCommonLightConstants(dm::float3 color, LightConstants& lightConstants)
{
    lightConstants.color = color;
    lightConstants.shadowCascades = int4(-1);
    lightConstants.perObjectShadows = int4(-1);
    lightConstants.shadowChannel = int4(-1);
    lightConstants.outOfBoundsShadow = 1.f;
}

} // namespace

int getLightType(const LightData& data)
{
    switch (data.index())
    {
    case 0: return LightType_Directional;
    case 1: return LightType_Spot;
    case 2: return LightType_Point;
    case 3: return LightType_Environment;
    default: return LightType_None;
    }
}

int getLightType(const LightRenderProxy& proxy)
{
    return getLightType(proxy.data);
}

int getLightType(const DirectionalLightComponent&)
{
    return LightType_Directional;
}

int getLightType(const SpotLightComponent&)
{
    return LightType_Spot;
}

int getLightType(const PointLightComponent&)
{
    return LightType_Point;
}

int getLightType(const EnvironmentLightComponent&)
{
    return LightType_Environment;
}

SceneContentFlags getLightContentFlags()
{
    return SceneContentFlags::Lights;
}

dm::double3 getLightPosition(const dm::daffine3& globalTransform)
{
    return globalTransform.m_translation;
}

dm::double3 getLightDirection(const dm::daffine3& globalTransform)
{
    return -normalize(dm::double3(globalTransform.m_linear.row2));
}

void setLightWorldPosition(SceneEntityWorld& world, ecs::Entity entity, const dm::double3& position)
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

    world.setTranslation(entity, inverse(parentToWorld).transformPoint(position));
}

void setLightWorldDirection(SceneEntityWorld& world, ecs::Entity entity, const dm::double3& direction)
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

bool isInfiniteLight(const LightData& data)
{
    switch (getLightType(data))
    {
    case LightType_Directional: return true;
    case LightType_Environment: return true;
    default: return false;
    }
}

bool isInfiniteLight(const LightRenderProxy& proxy)
{
    return isInfiniteLight(proxy.data);
}

LightData toLightData(const DirectionalLightComponent& component)
{
    return DirectionalLightData{ component.irradiance, component.angularSize };
}

LightData toLightData(const SpotLightComponent& component)
{
    return SpotLightData{
        component.intensity,
        component.radius,
        component.range,
        component.innerAngle,
        component.outerAngle,
    };
}

LightData toLightData(const PointLightComponent& component)
{
    return PointLightData{ component.intensity, component.radius, component.range };
}

LightData toLightData(const EnvironmentLightComponent& component)
{
    return EnvironmentLightData{
        component.radianceScale,
        component.textureIndex,
        component.rotation,
        component.path,
    };
}

void fillLightConstants(
    dm::float3 color, const LightData& data, const dm::daffine3& globalTransform, LightConstants& lightConstants)
{
    FillCommonLightConstants(color, lightConstants);

    switch (getLightType(data))
    {
    case LightType_Directional:
    {
        const auto& directional = std::get<DirectionalLightData>(data);
        lightConstants.lightType = LightType_Directional;
        lightConstants.direction = float3(normalize(getLightDirection(globalTransform)));
        const float clampedAngularSize = clamp(directional.angularSize, 0.f, 90.f);
        lightConstants.angularSizeOrInvRange = dm::radians(clampedAngularSize);
        lightConstants.intensity = directional.irradiance;
        break;
    }
    case LightType_Spot:
    {
        const auto& spot = std::get<SpotLightData>(data);
        lightConstants.lightType = LightType_Spot;
        lightConstants.direction = float3(getLightDirection(globalTransform));
        lightConstants.position = float3(getLightPosition(globalTransform));
        lightConstants.radius = spot.radius;
        lightConstants.angularSizeOrInvRange = (spot.range <= 0.f) ? 0.f : 1.f / spot.range;
        lightConstants.intensity = spot.intensity;
        lightConstants.color = color;
        lightConstants.innerAngle = dm::radians(spot.innerAngle);
        lightConstants.outerAngle = dm::radians(spot.outerAngle);
        break;
    }
    case LightType_Point:
    {
        const auto& point = std::get<PointLightData>(data);
        lightConstants.lightType = LightType_Point;
        lightConstants.position = float3(getLightPosition(globalTransform));
        lightConstants.radius = point.radius;
        lightConstants.angularSizeOrInvRange = (point.range <= 0.f) ? 0.f : 1.f / point.range;
        lightConstants.intensity = point.intensity;
        lightConstants.color = color;
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

void fillLightConstants(const LightRenderProxy& proxy, LightConstants& lightConstants)
{
    fillLightConstants(proxy.color, proxy.data, proxy.transform, lightConstants);
}

bool tryFillLightConstants(
    const ecs::World& world, ecs::Entity entity, const dm::daffine3& globalTransform, LightConstants& lightConstants)
{
    if (const auto* directional = tryGetDirectionalLight(world, entity))
    {
        fillLightConstants(directional->color, toLightData(*directional), globalTransform, lightConstants);
        return true;
    }
    if (const auto* spot = tryGetSpotLight(world, entity))
    {
        fillLightConstants(spot->color, toLightData(*spot), globalTransform, lightConstants);
        return true;
    }
    if (const auto* point = tryGetPointLight(world, entity))
    {
        fillLightConstants(point->color, toLightData(*point), globalTransform, lightConstants);
        return true;
    }
    if (const auto* environment = tryGetEnvironmentLight(world, entity))
    {
        fillLightConstants(environment->color, toLightData(*environment), globalTransform, lightConstants);
        return true;
    }
    return false;
}

bool hasAnyLightComponent(const ecs::World& world, ecs::Entity entity)
{
    return world.has<DirectionalLightComponent>(entity)
        || world.has<SpotLightComponent>(entity)
        || world.has<PointLightComponent>(entity)
        || world.has<EnvironmentLightComponent>(entity);
}

bool setLightProperty(
    ecs::World& world, ecs::Entity entity, const std::string& propName, const dm::float4& value)
{
    if (auto* directional = tryGetDirectionalLight(world, entity))
    {
        if (propName == "color") { directional->color = value.xyz(); return true; }
        if (propName == "irradiance") { directional->irradiance = value.x; return true; }
        if (propName == "angularSize") { directional->angularSize = value.x; return true; }
        return false;
    }

    if (auto* spot = tryGetSpotLight(world, entity))
    {
        if (propName == "color") { spot->color = value.xyz(); return true; }
        if (propName == "intensity") { spot->intensity = value.x; return true; }
        if (propName == "radius") { spot->radius = value.x; return true; }
        if (propName == "range") { spot->range = value.x; return true; }
        if (propName == "innerAngle") { spot->innerAngle = value.x; return true; }
        if (propName == "outerAngle") { spot->outerAngle = value.x; return true; }
        return false;
    }

    if (auto* point = tryGetPointLight(world, entity))
    {
        if (propName == "color") { point->color = value.xyz(); return true; }
        if (propName == "intensity") { point->intensity = value.x; return true; }
        if (propName == "radius") { point->radius = value.x; return true; }
        if (propName == "range") { point->range = value.x; return true; }
        return false;
    }

    if (auto* environment = tryGetEnvironmentLight(world, entity))
    {
        if (propName == "color") { environment->color = value.xyz(); return true; }
        return false;
    }

    return false;
}

DirectionalLightComponent* tryGetDirectionalLight(ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<DirectionalLightComponent>(entity);
}

SpotLightComponent* tryGetSpotLight(ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<SpotLightComponent>(entity);
}

PointLightComponent* tryGetPointLight(ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<PointLightComponent>(entity);
}

EnvironmentLightComponent* tryGetEnvironmentLight(ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<EnvironmentLightComponent>(entity);
}

const DirectionalLightComponent* tryGetDirectionalLight(const ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<DirectionalLightComponent>(entity);
}

const SpotLightComponent* tryGetSpotLight(const ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<SpotLightComponent>(entity);
}

const PointLightComponent* tryGetPointLight(const ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<PointLightComponent>(entity);
}

const EnvironmentLightComponent* tryGetEnvironmentLight(const ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<EnvironmentLightComponent>(entity);
}

DirectionalLightData* tryGetDirectionalLightData(LightData& data)
{
    return std::get_if<DirectionalLightData>(&data);
}

SpotLightData* tryGetSpotLightData(LightData& data)
{
    return std::get_if<SpotLightData>(&data);
}

PointLightData* tryGetPointLightData(LightData& data)
{
    return std::get_if<PointLightData>(&data);
}

EnvironmentLightData* tryGetEnvironmentLightData(LightData& data)
{
    return std::get_if<EnvironmentLightData>(&data);
}

const DirectionalLightData* tryGetDirectionalLightData(const LightData& data)
{
    return std::get_if<DirectionalLightData>(&data);
}

const SpotLightData* tryGetSpotLightData(const LightData& data)
{
    return std::get_if<SpotLightData>(&data);
}

const PointLightData* tryGetPointLightData(const LightData& data)
{
    return std::get_if<PointLightData>(&data);
}

const EnvironmentLightData* tryGetEnvironmentLightData(const LightData& data)
{
    return std::get_if<EnvironmentLightData>(&data);
}

ecs::Entity findEnvironmentLightEntity(const ecs::World& world, const std::vector<ecs::Entity>& lightEntities)
{
    for (ecs::Entity entity : lightEntities)
    {
        if (world.has<EnvironmentLightComponent>(entity))
            return entity;
    }
    return ecs::NullEntity;
}

const std::string& getEnvironmentLightPath(const EnvironmentLightComponent& component)
{
    return component.path;
}

} // namespace caustica::scene
