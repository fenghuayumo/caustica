#include <scene/SceneComponentBuilders.h>

#include <core/json.h>

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
        || type == "EnvironmentLight";
}

bool isJsonCameraLeafType(const std::string& type)
{
    return type == "PerspectiveCamera"
        || type == "PerspectiveCameraEx"
        || type == "OrthographicCamera";
}

std::optional<LightComponent> makeLightComponentFromJson(const std::string& type, const Json::Value& src)
{
    LightComponent component;

    if (type == "DirectionalLight")
    {
        DirectionalLightData data;
        src["color"] >> component.color;
        src["irradiance"] >> data.irradiance;
        src["angularSize"] >> data.angularSize;
        component.data = data;
        return component;
    }

    if (type == "PointLight")
    {
        PointLightData data;
        src["color"] >> component.color;
        src["intensity"] >> data.intensity;
        src["radius"] >> data.radius;
        src["range"] >> data.range;
        loadProxyMeshNodes(src, component.proxies);
        component.data = data;
        return component;
    }

    if (type == "SpotLight")
    {
        SpotLightData data;
        src["color"] >> component.color;
        src["intensity"] >> data.intensity;
        src["innerAngle"] >> data.innerAngle;
        src["outerAngle"] >> data.outerAngle;
        src["radius"] >> data.radius;
        src["range"] >> data.range;
        loadProxyMeshNodes(src, component.proxies);
        component.data = data;
        return component;
    }

    if (type == "EnvironmentLight")
    {
        EnvironmentLightData data;
        src["radianceScale"] >> data.radianceScale;
        src["textureIndex"] >> data.textureIndex;
        src["rotation"] >> data.rotation;
        src["path"] >> data.path;
        component.data = std::move(data);
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
        src["exposureCompensation"] >> data.exposureCompensation;
        src["exposureValue"] >> data.exposureValue;
        src["exposureValueMin"] >> data.exposureValueMin;
        src["exposureValueMax"] >> data.exposureValueMax;
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

} // namespace caustica::scene
