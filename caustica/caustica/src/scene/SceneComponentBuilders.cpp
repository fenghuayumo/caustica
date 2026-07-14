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

std::optional<AnyLightComponent> makeLightComponentFromJson(const std::string& type, const Json::Value& src)
{
    if (type == "DirectionalLight")
    {
        DirectionalLightComponent component;
        src["color"] >> component.color;
        src["irradiance"] >> component.irradiance;
        src["angularSize"] >> component.angularSize;
        return component;
    }

    if (type == "PointLight")
    {
        PointLightComponent component;
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
        src["color"] >> component.color;
        src["intensity"] >> component.intensity;
        src["innerAngle"] >> component.innerAngle;
        src["outerAngle"] >> component.outerAngle;
        src["radius"] >> component.radius;
        src["range"] >> component.range;
        loadProxyMeshNodes(src, component.proxies);
        return component;
    }

    if (type == "EnvironmentLight")
    {
        EnvironmentLightComponent component;
        src["radianceScale"] >> component.radianceScale;
        src["textureIndex"] >> component.textureIndex;
        src["rotation"] >> component.rotation;
        src["path"] >> component.path;
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
