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
        loadIfPresent(src, "alphaCullThreshold", splat.alphaCullThreshold);
        loadIfPresent(src, "shadowStrength", splat.shadowStrength);
        loadIfPresent(src, "secondaryRays", splat.secondaryRays);
        loadIfPresent(src, "illuminateMeshes", splat.illuminateMeshes);
        loadIfPresent(src, "radianceAlphaClamp", splat.radianceAlphaClamp);
        gaussianSplat = std::move(splat);
    }

    if (node.isMember("renderSettings") && node["renderSettings"].isObject())
    {
        RenderSettingsLook render;
        const Json::Value& src = node["renderSettings"];
        loadIfPresent(src, "realtimeSamplesPerPixel", render.realtimeSamplesPerPixel);
        loadIfPresent(src, "accumulationTarget", render.accumulationTarget);
        loadIfPresent(src, "realtimeAA", render.realtimeAA);
        loadIfPresent(src, "dlssMode", render.dlssMode);
        loadIfPresent(src, "standaloneDenoiser", render.standaloneDenoiser);
        loadIfPresent(src, "referenceOidnDenoiser", render.referenceOidnDenoiser);
        loadIfPresent(src, "referenceOidnUseGpu", render.referenceOidnUseGpu);
        loadIfPresent(src, "useNEE", render.useNEE);
        loadIfPresent(src, "neeType", render.neeType);
        loadIfPresent(src, "neeCandidateSamples", render.neeCandidateSamples);
        loadIfPresent(src, "neeFullSamples", render.neeFullSamples);
        loadIfPresent(src, "useRestirDI", render.useRestirDI);
        loadIfPresent(src, "useRestirGI", render.useRestirGI);
        loadIfPresent(src, "useRestirPT", render.useRestirPT);
        loadIfPresent(src, "restirPreset", render.restirPreset);
        loadIfPresent(src, "restirPTPreset", render.restirPTPreset);
        loadIfPresent(src, "realtimeFireflyEnabled", render.realtimeFireflyEnabled);
        loadIfPresent(src, "realtimeFireflyThreshold", render.realtimeFireflyThreshold);
        loadIfPresent(src, "referenceFireflyEnabled", render.referenceFireflyEnabled);
        loadIfPresent(src, "referenceFireflyThreshold", render.referenceFireflyThreshold);
        loadIfPresent(src, "nestedDielectricsQuality", render.nestedDielectricsQuality);
        loadIfPresent(src, "neeMISType", render.neeMISType);
        loadIfPresent(src, "environmentMapDiffuseSampleMIPLevel", render.environmentMapDiffuseSampleMIPLevel);
        loadIfPresent(src, "stablePlanesActiveCount", render.stablePlanesActiveCount);
        loadIfPresent(src, "allowPrimarySurfaceReplacement", render.allowPrimarySurfaceReplacement);
        loadIfPresent(src, "enableLDSamplerForBSDF", render.enableLDSamplerForBSDF);
        loadIfPresent(src, "gaussianSplatPrimaryMethod", render.gaussianSplatPrimaryMethod);
        loadIfPresent(src, "gaussianSplatShadowsMode", render.gaussianSplatShadowsMode);
        loadIfPresent(src, "gaussianSplatSortingMode", render.gaussianSplatSortingMode);
        loadIfPresent(src, "gaussianSplatSHFormat", render.gaussianSplatSHFormat);
        loadIfPresent(src, "gaussianSplatQuantizeNormals", render.gaussianSplatQuantizeNormals);
        loadIfPresent(src, "gaussianSplatMipAntialiasing", render.gaussianSplatMipAntialiasing);
        loadIfPresent(src, "gaussianSplatFrustumCulling", render.gaussianSplatFrustumCulling);
        loadIfPresent(src, "gaussianSplatScreenSizeCulling", render.gaussianSplatScreenSizeCulling);
        loadIfPresent(src, "gaussianSplatMinPixelCoverage", render.gaussianSplatMinPixelCoverage);
        renderSettings = std::move(render);
    }
    else
        renderSettings.reset();

    if (node.isMember("postProcess") && node["postProcess"].isObject())
    {
        PostProcessLook post;
        const Json::Value& src = node["postProcess"];
        loadIfPresent(src, "bloomEnabled", post.bloomEnabled);
        loadIfPresent(src, "bloomRadius", post.bloomRadius);
        loadIfPresent(src, "bloomIntensity", post.bloomIntensity);
        loadIfPresent(src, "toneMappingEnabled", post.toneMappingEnabled);
        loadIfPresent(src, "toneMapOperator", post.toneMapOperator);
        loadIfPresent(src, "autoExposure", post.autoExposure);
        loadIfPresent(src, "exposureMode", post.exposureMode);
        loadIfPresent(src, "exposureCompensation", post.exposureCompensation);
        loadIfPresent(src, "exposureValue", post.exposureValue);
        loadIfPresent(src, "exposureValueMin", post.exposureValueMin);
        loadIfPresent(src, "exposureValueMax", post.exposureValueMax);
        loadIfPresent(src, "filmSpeed", post.filmSpeed);
        loadIfPresent(src, "fNumber", post.fNumber);
        loadIfPresent(src, "shutter", post.shutter);
        loadIfPresent(src, "whiteBalance", post.whiteBalance);
        loadIfPresent(src, "whitePoint", post.whitePoint);
        loadIfPresent(src, "whiteMaxLuminance", post.whiteMaxLuminance);
        loadIfPresent(src, "whiteScale", post.whiteScale);
        loadIfPresent(src, "whiteClamped", post.whiteClamped);
        loadIfPresent(src, "cameraLutEnabled", post.cameraLutEnabled);
        loadIfPresent(src, "cameraLutAfterToneMap", post.cameraLutAfterToneMap);
        loadIfPresent(src, "cameraLutPreset", post.cameraLutPreset);
        loadIfPresent(src, "cameraLutPath", post.cameraLutPath);
        loadIfPresent(src, "edgeDetection", post.edgeDetection);
        loadIfPresent(src, "edgeDetectionThreshold", post.edgeDetectionThreshold);
        postProcess = std::move(post);
    }
    else
        postProcess.reset();

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
        if (src.alphaCullThreshold)
            splat["alphaCullThreshold"] << *src.alphaCullThreshold;
        if (src.shadowStrength)
            splat["shadowStrength"] << *src.shadowStrength;
        if (src.secondaryRays)
            splat["secondaryRays"] << *src.secondaryRays;
        if (src.illuminateMeshes)
            splat["illuminateMeshes"] << *src.illuminateMeshes;
        if (src.radianceAlphaClamp)
            splat["radianceAlphaClamp"] << *src.radianceAlphaClamp;
        settingsNode["gaussianSplat"] = std::move(splat);
    }
    else
        settingsNode.removeMember("gaussianSplat");

    if (renderSettings)
    {
        Json::Value render(Json::objectValue);
        const RenderSettingsLook& src = *renderSettings;
        if (src.realtimeSamplesPerPixel)
            render["realtimeSamplesPerPixel"] << *src.realtimeSamplesPerPixel;
        if (src.accumulationTarget)
            render["accumulationTarget"] << *src.accumulationTarget;
        if (src.realtimeAA)
            render["realtimeAA"] << *src.realtimeAA;
        if (src.dlssMode)
            render["dlssMode"] << *src.dlssMode;
        if (src.standaloneDenoiser)
            render["standaloneDenoiser"] << *src.standaloneDenoiser;
        if (src.referenceOidnDenoiser)
            render["referenceOidnDenoiser"] << *src.referenceOidnDenoiser;
        if (src.referenceOidnUseGpu)
            render["referenceOidnUseGpu"] << *src.referenceOidnUseGpu;
        if (src.useNEE)
            render["useNEE"] << *src.useNEE;
        if (src.neeType)
            render["neeType"] << *src.neeType;
        if (src.neeCandidateSamples)
            render["neeCandidateSamples"] << *src.neeCandidateSamples;
        if (src.neeFullSamples)
            render["neeFullSamples"] << *src.neeFullSamples;
        if (src.useRestirDI)
            render["useRestirDI"] << *src.useRestirDI;
        if (src.useRestirGI)
            render["useRestirGI"] << *src.useRestirGI;
        if (src.useRestirPT)
            render["useRestirPT"] << *src.useRestirPT;
        if (src.restirPreset)
            render["restirPreset"] << *src.restirPreset;
        if (src.restirPTPreset)
            render["restirPTPreset"] << *src.restirPTPreset;
        if (src.realtimeFireflyEnabled)
            render["realtimeFireflyEnabled"] << *src.realtimeFireflyEnabled;
        if (src.realtimeFireflyThreshold)
            render["realtimeFireflyThreshold"] << *src.realtimeFireflyThreshold;
        if (src.referenceFireflyEnabled)
            render["referenceFireflyEnabled"] << *src.referenceFireflyEnabled;
        if (src.referenceFireflyThreshold)
            render["referenceFireflyThreshold"] << *src.referenceFireflyThreshold;
        if (src.nestedDielectricsQuality)
            render["nestedDielectricsQuality"] << *src.nestedDielectricsQuality;
        if (src.neeMISType)
            render["neeMISType"] << *src.neeMISType;
        if (src.environmentMapDiffuseSampleMIPLevel)
            render["environmentMapDiffuseSampleMIPLevel"] << *src.environmentMapDiffuseSampleMIPLevel;
        if (src.stablePlanesActiveCount)
            render["stablePlanesActiveCount"] << *src.stablePlanesActiveCount;
        if (src.allowPrimarySurfaceReplacement)
            render["allowPrimarySurfaceReplacement"] << *src.allowPrimarySurfaceReplacement;
        if (src.enableLDSamplerForBSDF)
            render["enableLDSamplerForBSDF"] << *src.enableLDSamplerForBSDF;
        if (src.gaussianSplatPrimaryMethod)
            render["gaussianSplatPrimaryMethod"] << *src.gaussianSplatPrimaryMethod;
        if (src.gaussianSplatShadowsMode)
            render["gaussianSplatShadowsMode"] << *src.gaussianSplatShadowsMode;
        if (src.gaussianSplatSortingMode)
            render["gaussianSplatSortingMode"] << *src.gaussianSplatSortingMode;
        if (src.gaussianSplatSHFormat)
            render["gaussianSplatSHFormat"] << *src.gaussianSplatSHFormat;
        if (src.gaussianSplatQuantizeNormals)
            render["gaussianSplatQuantizeNormals"] << *src.gaussianSplatQuantizeNormals;
        if (src.gaussianSplatMipAntialiasing)
            render["gaussianSplatMipAntialiasing"] << *src.gaussianSplatMipAntialiasing;
        if (src.gaussianSplatFrustumCulling)
            render["gaussianSplatFrustumCulling"] << *src.gaussianSplatFrustumCulling;
        if (src.gaussianSplatScreenSizeCulling)
            render["gaussianSplatScreenSizeCulling"] << *src.gaussianSplatScreenSizeCulling;
        if (src.gaussianSplatMinPixelCoverage)
            render["gaussianSplatMinPixelCoverage"] << *src.gaussianSplatMinPixelCoverage;
        settingsNode["renderSettings"] = std::move(render);
    }
    else
        settingsNode.removeMember("renderSettings");

    if (postProcess)
    {
        Json::Value post(Json::objectValue);
        const PostProcessLook& src = *postProcess;
        if (src.bloomEnabled)
            post["bloomEnabled"] << *src.bloomEnabled;
        if (src.bloomRadius)
            post["bloomRadius"] << *src.bloomRadius;
        if (src.bloomIntensity)
            post["bloomIntensity"] << *src.bloomIntensity;
        if (src.toneMappingEnabled)
            post["toneMappingEnabled"] << *src.toneMappingEnabled;
        if (src.toneMapOperator)
            post["toneMapOperator"] << *src.toneMapOperator;
        if (src.autoExposure)
            post["autoExposure"] << *src.autoExposure;
        if (src.exposureMode)
            post["exposureMode"] << *src.exposureMode;
        if (src.exposureCompensation)
            post["exposureCompensation"] << *src.exposureCompensation;
        if (src.exposureValue)
            post["exposureValue"] << *src.exposureValue;
        if (src.exposureValueMin)
            post["exposureValueMin"] << *src.exposureValueMin;
        if (src.exposureValueMax)
            post["exposureValueMax"] << *src.exposureValueMax;
        if (src.filmSpeed)
            post["filmSpeed"] << *src.filmSpeed;
        if (src.fNumber)
            post["fNumber"] << *src.fNumber;
        if (src.shutter)
            post["shutter"] << *src.shutter;
        if (src.whiteBalance)
            post["whiteBalance"] << *src.whiteBalance;
        if (src.whitePoint)
            post["whitePoint"] << *src.whitePoint;
        if (src.whiteMaxLuminance)
            post["whiteMaxLuminance"] << *src.whiteMaxLuminance;
        if (src.whiteScale)
            post["whiteScale"] << *src.whiteScale;
        if (src.whiteClamped)
            post["whiteClamped"] << *src.whiteClamped;
        if (src.cameraLutEnabled)
            post["cameraLutEnabled"] << *src.cameraLutEnabled;
        if (src.cameraLutAfterToneMap)
            post["cameraLutAfterToneMap"] << *src.cameraLutAfterToneMap;
        if (src.cameraLutPreset)
            post["cameraLutPreset"] << *src.cameraLutPreset;
        if (src.cameraLutPath)
            post["cameraLutPath"] << *src.cameraLutPath;
        if (src.edgeDetection)
            post["edgeDetection"] << *src.edgeDetection;
        if (src.edgeDetectionThreshold)
            post["edgeDetectionThreshold"] << *src.edgeDetectionThreshold;
        settingsNode["postProcess"] = std::move(post);
    }
    else
        settingsNode.removeMember("postProcess");

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
