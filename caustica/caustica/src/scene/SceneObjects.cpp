#include <scene/SceneResources.h>  // provides complete SceneTypeFactory; transitively includes SceneObjects.h
#include <core/json.h>

using namespace caustica;

// =============================================================================
// GaussianSplat
// =============================================================================

void GaussianSplat::load(const Json::Value& node)
{
    node["path"] >> path;
    if (path.empty()) node["file"]     >> path;
    if (path.empty()) node["fileName"] >> path;
    node["convertRdfToRub"] >> convertRdfToRub;
    node["enabled"] >> enabled;
}

// =============================================================================
// SceneSettings
// =============================================================================

namespace
{

template<typename T>
void loadIfPresent(const Json::Value& node, const char* key, std::optional<T>& dest)
{
    if (!node.isMember(key) || node[key].isNull())
        return;
    T value{};
    node[key] >> value;
    dest = std::move(value);
}

void writeStringArray(Json::Value& node, const std::vector<std::string>& values)
{
    node = Json::Value(Json::arrayValue);
    for (const std::string& value : values)
        node.append(value);
}

} // namespace

void SceneSettings::load(const Json::Value& node)
{
    node["realtimeMode"]          >> realtimeMode;
    node["enableAnimations"]      >> enableAnimations;
    node["enableKeyframes"]       >> enableKeyframes;
    if (node["startingCamera"].isString())
        node["startingCamera"] >> startingCameraId;
    else
        node["startingCamera"] >> startingCamera;
    node["realtimeFireflyFilter"] >> realtimeFireflyFilter;
    node["maxBounces"]            >> maxBounces;
    node["maxDiffuseBounces"]     >> maxDiffuseBounces;
    node["textureMIPBias"]        >> textureMIPBias;

    environment.reset();
    gaussianSplat.reset();
    hiddenEntities.clear();

    if (node.isMember("environment") && node["environment"].isObject())
    {
        EnvironmentLookSettings env;
        const Json::Value& src = node["environment"];
        loadIfPresent(src, "tintColor", env.tintColor);
        loadIfPresent(src, "intensity", env.intensity);
        loadIfPresent(src, "rotation", env.rotationXYZ);
        loadIfPresent(src, "visibleToCamera", env.visibleToCamera);
        loadIfPresent(src, "enabled", env.enabled);
        loadIfPresent(src, "override", env.overrideSource);
        environment = std::move(env);
    }

    if (node.isMember("gaussianSplat") && node["gaussianSplat"].isObject())
    {
        GaussianSplatLookSettings splat;
        const Json::Value& src = node["gaussianSplat"];
        loadIfPresent(src, "footprintScale", splat.footprintScale);
        loadIfPresent(src, "alphaScale", splat.alphaScale);
        loadIfPresent(src, "brightness", splat.brightness);
        loadIfPresent(src, "tintColor", splat.tintColor);
        loadIfPresent(src, "applyToneMapping", splat.applyToneMapping);
        loadIfPresent(src, "asEmitter", splat.asEmitter);
        loadIfPresent(src, "emissionIntensity", splat.emissionIntensity);
        loadIfPresent(src, "alphaCullThreshold", splat.alphaCullThreshold);
        loadIfPresent(src, "shadowStrength", splat.shadowStrength);
        gaussianSplat = std::move(splat);
    }

    if (node.isMember("hiddenEntities") && node["hiddenEntities"].isArray())
        hiddenEntities = caustica::json::readStringArray(node["hiddenEntities"]);
}

void SceneSettings::writeLook(Json::Value& settingsNode) const
{
    if (!settingsNode.isObject())
        settingsNode = Json::Value(Json::objectValue);

    if (environment)
    {
        Json::Value env(Json::objectValue);
        const EnvironmentLookSettings& src = *environment;
        if (src.tintColor)
            env["tintColor"] << *src.tintColor;
        if (src.intensity)
            env["intensity"] << *src.intensity;
        if (src.rotationXYZ)
            env["rotation"] << *src.rotationXYZ;
        if (src.visibleToCamera)
            env["visibleToCamera"] << *src.visibleToCamera;
        if (src.enabled)
            env["enabled"] << *src.enabled;
        if (src.overrideSource)
            env["override"] << *src.overrideSource;
        settingsNode["environment"] = std::move(env);
    }
    else
        settingsNode.removeMember("environment");

    if (gaussianSplat)
    {
        Json::Value splat(Json::objectValue);
        const GaussianSplatLookSettings& src = *gaussianSplat;
        if (src.footprintScale)
            splat["footprintScale"] << *src.footprintScale;
        if (src.alphaScale)
            splat["alphaScale"] << *src.alphaScale;
        if (src.brightness)
            splat["brightness"] << *src.brightness;
        if (src.tintColor)
            splat["tintColor"] << *src.tintColor;
        if (src.applyToneMapping)
            splat["applyToneMapping"] << *src.applyToneMapping;
        if (src.asEmitter)
            splat["asEmitter"] << *src.asEmitter;
        if (src.emissionIntensity)
            splat["emissionIntensity"] << *src.emissionIntensity;
        if (src.alphaCullThreshold)
            splat["alphaCullThreshold"] << *src.alphaCullThreshold;
        if (src.shadowStrength)
            splat["shadowStrength"] << *src.shadowStrength;
        settingsNode["gaussianSplat"] = std::move(splat);
    }
    else
        settingsNode.removeMember("gaussianSplat");

    if (hiddenEntities.empty())
        settingsNode.removeMember("hiddenEntities");
    else
        writeStringArray(settingsNode["hiddenEntities"], hiddenEntities);
}

// =============================================================================
// GameSettings
// =============================================================================

void GameSettings::load(const Json::Value& node)
{
    Json::StreamWriterBuilder writer;
    jsonData = Json::writeString(writer, node);
}

// =============================================================================
// SceneTypeFactory
// =============================================================================

std::shared_ptr<void> SceneTypeFactory::createLeaf(const std::string& type)
{
    if (type == "GaussianSplat" || type == "GaussianSplats" || type == "3DGaussianSplat")
        return std::make_shared<GaussianSplat>();
    // Accept the legacy name so scenes authored before the engine rename still load.
    if (type == "SceneSettings" || type == "SampleSettings")
        return std::make_shared<SceneSettings>();
    if (type == "GameSettings")
        return std::make_shared<GameSettings>();
    return nullptr;
}

std::shared_ptr<Material> SceneTypeFactory::createMaterial()
{
    return std::make_shared<Material>();
}

std::shared_ptr<MeshInfo> SceneTypeFactory::createMesh()
{
    return std::make_shared<MeshInfo>();
}

std::shared_ptr<MeshGeometry> SceneTypeFactory::createMeshGeometry()
{
    return std::make_shared<MeshGeometry>();
}
