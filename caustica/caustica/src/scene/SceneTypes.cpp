#include <scene/SceneObjects.h>
#include <scene/SceneEcs.h>
#include <scene/IShadowMap.h>
#include <core/json.h>
#include <json/json-forwards.h>

using namespace caustica::math;
#include <shaders/light_cb.h>
#include <shaders/bindless.h>

using namespace caustica;
using namespace caustica::scene;

// =============================================================================
// Light (base)
// =============================================================================

void Light::fillLightConstants(LightConstants& lightConstants, const dm::daffine3& /*globalTransform*/) const
{
    lightConstants.color = color;
    lightConstants.shadowCascades = int4(-1);
    lightConstants.perObjectShadows = int4(-1);
    lightConstants.shadowChannel = int4(shadowChannel, -1, -1, -1);
    if (shadowMap)
        lightConstants.outOfBoundsShadow = shadowMap->isLitOutOfBounds() ? 1.f : 0.f;
    else
        lightConstants.outOfBoundsShadow = 1.f;
}

bool Light::setProperty(const std::string& propName, const dm::float4& value)
{
    if (propName == "color")
    {
        color = value.xyz();
        return true;
    }

    return false;
}

void Light::setPosition(scene::SceneEntityWorld& world, ecs::Entity entity, const dm::double3& position) const
{
    ecs::Entity parentEntity = ecs::NullEntity;
    if (const auto* parent = world.world().get<scene::ParentComponent>(entity))
        parentEntity = parent->parent;

    dm::daffine3 parentToWorld = dm::daffine3::identity();
    if (ecs::isValid(parentEntity))
    {
        if (const auto* globalTransform = world.world().get<scene::GlobalTransformComponent>(parentEntity))
            parentToWorld = globalTransform->transform;
    }

    dm::double3 translation = inverse(parentToWorld).transformPoint(position);
    world.setTranslation(entity, translation);
}

void Light::setDirection(scene::SceneEntityWorld& world, ecs::Entity entity, const dm::double3& direction) const
{
    ecs::Entity parentEntity = ecs::NullEntity;
    if (const auto* parent = world.world().get<scene::ParentComponent>(entity))
        parentEntity = parent->parent;

    dm::daffine3 parentToWorld = dm::daffine3::identity();
    if (ecs::isValid(parentEntity))
    {
        if (const auto* globalTransform = world.world().get<scene::GlobalTransformComponent>(parentEntity))
            parentToWorld = globalTransform->transform;
    }

    daffine3 worldToLocal = lookatZ(direction);
    daffine3 localToParent = inverse(worldToLocal * parentToWorld);

    dquat rotation;
    double3 scaling;
    decomposeAffine<double>(localToParent, nullptr, &rotation, &scaling);

    world.setLocalTransform(entity, nullptr, &rotation, &scaling);
}

// =============================================================================
// DirectionalLight
// =============================================================================

std::shared_ptr<DirectionalLight> DirectionalLight::clone() const
{
    auto copy = std::make_shared<DirectionalLight>();
    copy->name = name;
    copy->color = color;
    copy->irradiance = irradiance;
    copy->angularSize = angularSize;
    copy->LightLink = LightLink;
    copy->Proxies = Proxies;
    return copy;
}

void DirectionalLight::fillLightConstants(LightConstants& lightConstants, const dm::daffine3& globalTransform) const
{
    Light::fillLightConstants(lightConstants, globalTransform);

    lightConstants.lightType = LightType_Directional;
    lightConstants.direction = float3(normalize(getDirection(globalTransform)));
    float clampedAngularSize = clamp(angularSize, 0.f, 90.f);
    lightConstants.angularSizeOrInvRange = dm::radians(clampedAngularSize);
    lightConstants.intensity = irradiance;
}

void DirectionalLight::load(const Json::Value& node)
{
    node["color"] >> color;
    node["irradiance"] >> irradiance;
    node["angularSize"] >> angularSize;
}

void DirectionalLight::store(Json::Value& node) const
{
    node["type"] << "DirectionalLight";
    node["color"] << color;
    node["irradiance"] << irradiance;
    node["angularSize"] << angularSize;
}

bool DirectionalLight::setProperty(const std::string& propName, const dm::float4& value)
{
    if (propName == "irradiance")  { irradiance = value.x; return true; }
    if (propName == "angularSize") { angularSize = value.x; return true; }
    return Light::setProperty(propName, value);
}

// =============================================================================
// SpotLight
// =============================================================================

inline float square(const float x) { return x * x; }

std::shared_ptr<SpotLight> SpotLight::clone() const
{
    auto copy = std::make_shared<SpotLight>();
    copy->name = name;
    copy->color = color;
    copy->intensity = intensity;
    copy->radius = radius;
    copy->range = range;
    copy->innerAngle = innerAngle;
    copy->outerAngle = outerAngle;
    copy->LightLink = LightLink;
    copy->Proxies = Proxies;
    return copy;
}

void SpotLight::fillLightConstants(LightConstants& lightConstants, const dm::daffine3& globalTransform) const
{
    Light::fillLightConstants(lightConstants, globalTransform);

    lightConstants.lightType = LightType_Spot;
    lightConstants.direction = float3(getDirection(globalTransform));
    lightConstants.position = float3(getPosition(globalTransform));
    lightConstants.radius = radius;
    lightConstants.angularSizeOrInvRange = (range <= 0.f) ? 0.f : 1.f / range;
    lightConstants.intensity = intensity;
    lightConstants.color = color;
    lightConstants.innerAngle = dm::radians(innerAngle);
    lightConstants.outerAngle = dm::radians(outerAngle);
}

void SpotLight::load(const Json::Value& node)
{
    node["color"] >> color;
    node["intensity"] >> intensity;
    node["innerAngle"] >> innerAngle;
    node["outerAngle"] >> outerAngle;
    node["radius"] >> radius;
    node["range"] >> range;

    // load light-sampler proxy mesh references
    if (node.isMember("proxyMeshNodes") && node["proxyMeshNodes"].isArray())
    {
        Proxies.reserve(node["proxyMeshNodes"].size());
        for (const auto& v : node["proxyMeshNodes"])
            Proxies.push_back(v.asString());
    }
}

void SpotLight::store(Json::Value& node) const
{
    node["type"] << "SpotLight";
    node["color"] << color;
    node["intensity"] << intensity;
    node["innerAngle"] << innerAngle;
    node["outerAngle"] << outerAngle;
    node["radius"] << radius;
    node["range"] << range;
}

bool SpotLight::setProperty(const std::string& propName, const dm::float4& value)
{
    if (propName == "intensity")   { intensity = value.x; return true; }
    if (propName == "radius")      { radius = value.x; return true; }
    if (propName == "range")       { range = value.x; return true; }
    if (propName == "innerAngle")  { innerAngle = value.x; return true; }
    if (propName == "outerAngle")  { outerAngle = value.x; return true; }
    return Light::setProperty(propName, value);
}

// =============================================================================
// PointLight
// =============================================================================

std::shared_ptr<PointLight> PointLight::clone() const
{
    auto copy = std::make_shared<PointLight>();
    copy->name = name;
    copy->color = color;
    copy->intensity = intensity;
    copy->radius = radius;
    copy->range = range;
    copy->LightLink = LightLink;
    copy->Proxies = Proxies;
    return copy;
}

void PointLight::fillLightConstants(LightConstants& lightConstants, const dm::daffine3& globalTransform) const
{
    Light::fillLightConstants(lightConstants, globalTransform);

    lightConstants.lightType = LightType_Point;
    lightConstants.position = float3(getPosition(globalTransform));
    lightConstants.radius = radius;
    lightConstants.angularSizeOrInvRange = (range <= 0.f) ? 0.f : 1.f / range;
    lightConstants.intensity = intensity;
    lightConstants.color = color;
}

void PointLight::load(const Json::Value& node)
{
    node["color"] >> color;
    node["intensity"] >> intensity;
    node["radius"] >> radius;
    node["range"] >> range;

    // load light-sampler proxy mesh references
    if (node.isMember("proxyMeshNodes") && node["proxyMeshNodes"].isArray())
    {
        Proxies.reserve(node["proxyMeshNodes"].size());
        for (const auto& v : node["proxyMeshNodes"])
            Proxies.push_back(v.asString());
    }
}

void PointLight::store(Json::Value& node) const
{
    node["type"] << "PointLight";
    node["color"] << color;
    node["intensity"] << intensity;
    node["radius"] << radius;
    node["range"] << range;
}

bool PointLight::setProperty(const std::string& propName, const dm::float4& value)
{
    if (propName == "intensity") { intensity = value.x; return true; }
    if (propName == "radius")    { radius = value.x; return true; }
    if (propName == "range")     { range = value.x; return true; }
    return Light::setProperty(propName, value);
}

// =============================================================================
// EnvironmentLight
// =============================================================================

void EnvironmentLight::fillLightConstants(LightConstants& lightConstants, const dm::daffine3& globalTransform) const
{
    Light::fillLightConstants(lightConstants, globalTransform);
    lightConstants.intensity = 0.0f;
    lightConstants.color = { 0, 0, 0 };
}

// =============================================================================
// Vertex attribute descriptors and material helpers (unchanged)
// =============================================================================

nvrhi::VertexAttributeDesc caustica::getVertexAttributeDesc(VertexAttribute attribute, const char* name, uint32_t bufferIndex)
{
    nvrhi::VertexAttributeDesc result = {};
    result.name = name;
    result.bufferIndex = bufferIndex;
    result.arraySize = 1;

    switch (attribute)
    {
    case VertexAttribute::Position:
    case VertexAttribute::PrevPosition:
        result.format = nvrhi::Format::RGB32_FLOAT;
        result.elementStride = sizeof(float3);
        break;
    case VertexAttribute::TexCoord1:
    case VertexAttribute::TexCoord2:
        result.format = nvrhi::Format::RG32_FLOAT;
        result.elementStride = sizeof(float2);
        break;
    case VertexAttribute::Normal:
    case VertexAttribute::Tangent:
        result.format = nvrhi::Format::RGBA8_SNORM;
        result.elementStride = sizeof(uint32_t);
        break;
    case VertexAttribute::Transform:
        result.format = nvrhi::Format::RGBA32_FLOAT;
        result.arraySize = 3;
        result.offset = offsetof(InstanceData, transform);
        result.elementStride = sizeof(InstanceData);
        result.isInstanced = true;
        break;
    case VertexAttribute::PrevTransform:
        result.format = nvrhi::Format::RGBA32_FLOAT;
        result.arraySize = 3;
        result.offset = offsetof(InstanceData, prevTransform);
        result.elementStride = sizeof(InstanceData);
        result.isInstanced = true;
        break;

    default:
        assert(!"unknown attribute");
    }

    return result;
}

const char* caustica::materialDomainToString(MaterialDomain domain)
{
    switch (domain)
    {
    case MaterialDomain::Opaque: return "Opaque";
    case MaterialDomain::AlphaTested: return "AlphaTested";
    case MaterialDomain::AlphaBlended: return "AlphaBlended";
    case MaterialDomain::Transmissive: return "Transmissive";
    case MaterialDomain::TransmissiveAlphaTested: return "TransmissiveAlphaTested";
    case MaterialDomain::TransmissiveAlphaBlended: return "TransmissiveAlphaBlended";
    case MaterialDomain::Count: return "Count";
    default: return "<Invalid>";
    }
}

bool LightProbe::isActive() const
{
    if (!enabled)
        return false;
    if (bounds.isempty())
        return false;
    if ((diffuseScale == 0.f || !diffuseMap) && (specularScale == 0.f || !specularMap))
        return false;

    return true;
}

void LightProbe::fillLightProbeConstants(LightProbeConstants& lightProbeConstants) const
{
    lightProbeConstants.diffuseArrayIndex = diffuseArrayIndex;
    lightProbeConstants.specularArrayIndex = specularArrayIndex;
    lightProbeConstants.diffuseScale = diffuseScale;
    lightProbeConstants.specularScale = specularScale;
    lightProbeConstants.mipLevels = specularMap ? static_cast<float>(specularMap->getDesc().mipLevels) : 0.f;

    for (uint32_t nPlane = 0; nPlane < frustum::PLANES_COUNT; nPlane++)
    {
        lightProbeConstants.frustumPlanes[nPlane] = float4(bounds.planes[nPlane].normal, bounds.planes[nPlane].distance);
    }
}
