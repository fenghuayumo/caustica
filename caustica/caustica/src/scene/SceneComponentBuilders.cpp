#include <scene/SceneComponentBuilders.h>
#include <scene/SceneSerializer.h>
#include <core/json.h>
#include <core/StringUtils.h>

#include <algorithm>

namespace caustica::scene
{
namespace
{

void loadProxyMeshNodes(const Json::Value& node, std::vector<std::string>& proxies)
{
    if (!node.isMember("proxyMeshNodes") || !node["proxyMeshNodes"].isArray())
        return;

    proxies.reserve(proxies.size() + node["proxyMeshNodes"].size());
    for (const auto& v : node["proxyMeshNodes"])
        proxies.push_back(v.asString());
}

} // namespace

bool isJsonLightLeafType(const std::string& type)
{
    return type == "DirectionalLight"
        || type == "PointLight"
        || type == "SpotLight"
        || type == "RectLight"
        || type == "EnvironmentLight";
}

bool isJsonCameraLeafType(const std::string& type)
{
    return type == "PerspectiveCamera"
        || type == "PerspectiveCameraEx"
        || type == "OrthographicCamera";
}

std::optional<AnyLightComponent> makeLightComponentFromJson(const std::string& type, const Json::Value& src)
{
    if (type == "DirectionalLight")
    {
        DirectionalLightComponent component;
        if (src.isMember("enabled")) src["enabled"] >> component.enabled;
        src["color"] >> component.color;
        src["irradiance"] >> component.irradiance;
        src["angularSize"] >> component.angularSize;
        return component;
    }

    if (type == "PointLight")
    {
        PointLightComponent component;
        if (src.isMember("enabled")) src["enabled"] >> component.enabled;
        src["color"] >> component.color;
        src["intensity"] >> component.intensity;
        src["radius"] >> component.radius;
        src["range"] >> component.range;
        loadProxyMeshNodes(src, component.proxies);
        return component;
    }

    if (type == "SpotLight")
    {
        SpotLightComponent component;
        if (src.isMember("enabled")) src["enabled"] >> component.enabled;
        src["color"] >> component.color;
        src["intensity"] >> component.intensity;
        src["innerAngle"] >> component.innerAngle;
        src["outerAngle"] >> component.outerAngle;
        src["radius"] >> component.radius;
        src["range"] >> component.range;
        loadProxyMeshNodes(src, component.proxies);
        return component;
    }

    if (type == "RectLight")
    {
        RectLightComponent component;
        if (src.isMember("enabled")) src["enabled"] >> component.enabled;
        src["color"] >> component.color;
        src["intensity"] >> component.intensity;
        src["width"] >> component.width;
        src["height"] >> component.height;
        return component;
    }

    if (type == "EnvironmentLight")
    {
        EnvironmentLightComponent component;
        if (src.isMember("enabled")) src["enabled"] >> component.enabled;
        src["radianceScale"] >> component.radianceScale;
        src["textureIndex"] >> component.textureIndex;
        src["rotation"] >> component.rotation;
        if (src["source"].isString())
            src["source"] >> component.path;
        else
            src["path"] >> component.path;
        component.path = canonicalizeEnvSource(component.path);
        return component;
    }

    return std::nullopt;
}

std::optional<CameraComponent> makeCameraComponentFromJson(const std::string& type, const Json::Value& src)
{
    CameraComponent component;

    if (type == "PerspectiveCamera" || type == "PerspectiveCameraEx")
    {
        PerspectiveCameraData data;
        src["verticalFov"] >> data.verticalFov;
        src["aspectRatio"] >> data.aspectRatio;
        src["zNear"] >> data.zNear;
        src["zFar"] >> data.zFar;
        src["enableAutoExposure"] >> data.enableAutoExposure;
        src["toneMapOperator"] >> data.toneMapOperator;
        src["exposureCompensation"] >> data.exposureCompensation;
        src["exposureValue"] >> data.exposureValue;
        src["exposureValueMin"] >> data.exposureValueMin;
        src["exposureValueMax"] >> data.exposureValueMax;
        if (src.isMember("fx") && src.isMember("fy") && src.isMember("cx") && src.isMember("cy")
            && src.isMember("width") && src.isMember("height"))
        {
            CameraIntrinsics k;
            src["fx"] >> k.fx;
            src["fy"] >> k.fy;
            src["cx"] >> k.cx;
            src["cy"] >> k.cy;
            src["width"] >> k.width;
            src["height"] >> k.height;
            if (k.fx > 0.f && k.fy > 0.f && k.width > 0.f && k.height > 0.f)
                data.intrinsics = k;
        }
        component.data = std::move(data);
        return component;
    }

    if (type == "OrthographicCamera")
    {
        OrthographicCameraData data;
        src["xMag"] >> data.xMag;
        src["yMag"] >> data.yMag;
        src["zNear"] >> data.zNear;
        src["zFar"] >> data.zFar;
        component.data = data;
        return component;
    }

    return std::nullopt;
}

bool isJsonPhysicsLeafType(const std::string& type)
{
    return type == "RigidBody" || type == "Collider";
}

std::optional<physics::RigidBodyComponent> makeRigidBodyComponentFromJson(const Json::Value& src)
{
    physics::RigidBodyComponent body;
    std::string motion = "dynamic";
    if (src["motion"].isString())
        src["motion"] >> motion;
    else if (src["bodyType"].isString())
        src["bodyType"] >> motion;
    if (caustica::string_utils::caseInsensitiveEquals(motion, std::string("static")))
        body.type = physics::RigidBodyType::Static;
    else if (caustica::string_utils::caseInsensitiveEquals(motion, std::string("kinematic")))
        body.type = physics::RigidBodyType::Kinematic;
    else
        body.type = physics::RigidBodyType::Dynamic;
    src["mass"] >> body.mass;
    src["gravity"] >> body.gravity;
    src["enabled"] >> body.enabled;
    src["linearVelocity"] >> body.linearVelocity;
    src["angularVelocity"] >> body.angularVelocity;
    return body;
}

std::optional<physics::ColliderComponent> makeColliderComponentFromJson(const Json::Value& src)
{
    physics::ColliderComponent collider;
    std::string shape = "box";
    if (src["shape"].isString())
        src["shape"] >> shape;
    if (caustica::string_utils::caseInsensitiveEquals(shape, std::string("sphere")))
        collider.shape = physics::ColliderShape::Sphere;
    else if (caustica::string_utils::caseInsensitiveEquals(shape, std::string("capsule")))
        collider.shape = physics::ColliderShape::Capsule;
    else
        collider.shape = physics::ColliderShape::Box;
    if (src["dimensions"].isArray() || src["dimensions"].isNumeric())
        src["dimensions"] >> collider.dimensions;
    else if (src["size"].isArray() || src["size"].isNumeric())
        src["size"] >> collider.dimensions;
    else if (collider.shape == physics::ColliderShape::Sphere && src["radius"].isNumeric())
    {
        float radius = collider.dimensions.x;
        src["radius"] >> radius;
        collider.dimensions = math::float3(radius);
    }
    src["offset"] >> collider.offset;
    src["staticFriction"] >> collider.staticFriction;
    src["dynamicFriction"] >> collider.dynamicFriction;
    src["restitution"] >> collider.restitution;
    src["isTrigger"] >> collider.isTrigger;
    return collider;
}

void writeRigidBodyComponent(Json::Value& dst, const physics::RigidBodyComponent& body)
{
    switch (body.type)
    {
    case physics::RigidBodyType::Static:
        dst["motion"] = "static";
        break;
    case physics::RigidBodyType::Kinematic:
        dst["motion"] = "kinematic";
        break;
    default:
        dst["motion"] = "dynamic";
        break;
    }
    dst["mass"] << body.mass;
    dst["gravity"] << body.gravity;
    dst["enabled"] << body.enabled;
    dst["linearVelocity"] << body.linearVelocity;
    dst["angularVelocity"] << body.angularVelocity;
}

void writeColliderComponent(Json::Value& dst, const physics::ColliderComponent& collider)
{
    switch (collider.shape)
    {
    case physics::ColliderShape::Sphere:
        dst["shape"] = "sphere";
        break;
    case physics::ColliderShape::Capsule:
        dst["shape"] = "capsule";
        break;
    default:
        dst["shape"] = "box";
        break;
    }
    dst["dimensions"] << collider.dimensions;
    dst["offset"] << collider.offset;
    dst["staticFriction"] << collider.staticFriction;
    dst["dynamicFriction"] << collider.dynamicFriction;
    dst["restitution"] << collider.restitution;
    dst["isTrigger"] << collider.isTrigger;
}

} // namespace caustica::scene
