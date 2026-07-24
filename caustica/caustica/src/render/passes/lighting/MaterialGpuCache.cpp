#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/lighting/materialFeatureMask.h>

#include <assets/loader/ShaderFactory.h>
#include <assets/loader/ShaderBackend.h>
#include <render/core/FramebufferFactory.h>
#include <assets/loader/TextureLoader.h>

#include <core/scope.h>

#include <rhi/utils.h>

#include <imgui/imgui_renderer.h>

#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <render/core/ScopedPerfMarker.h>
#include <imgui/ui_macros.h>
#include <render/core/TextureUtils.h>
#include <scene/Scene.h>
#include <scene/SceneRenderData.h>
#include <render/SceneGpuResources.h>
#include <scene/SceneEcs.h>
#include <scene/scene_utils.h>

#include <core/json.h>
#include <json/json.h>

#include <algorithm>
#include <fstream>

#include <unordered_set>

#include <cctype>      // std::tolower
#include <cstring>

#include <render/passes/debug/picosha2.h>

#include <shaders/bindless.h>

using namespace caustica::math;
using namespace caustica;

static std::string kNoModel = "<no_model>";

static Json::Value JsonMemberEither(const Json::Value& object, const char* primary, const char* alternate)
{
    if (object.isMember(primary))
        return object[primary];
    if (alternate && object.isMember(alternate))
        return object[alternate];
    return Json::Value();
}

static std::array<unsigned char, picosha2::k_digest_size> HashMyString(const std::string& string)
{
    std::array<unsigned char, picosha2::k_digest_size> arr;
    picosha2::hash256(string.begin(), string.end(), arr.begin(), arr.end());
    return arr;
}

static size_t ShortHash(const std::array<unsigned char, picosha2::k_digest_size> longHash)
{
    size_t h = 0;
    for (auto b : longHash)
        h ^= static_cast<size_t>(b) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

static std::string LowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return char(std::tolower(ch));
    });
    return value;
}

static bool IsOpenPBRMaterialModel(const std::string& materialModel)
{
    std::string normalized = LowerCopy(materialModel);
    // Accept former spelling variants when loading older files.
    return normalized == "openpbr" || normalized == "openpbr-lite" || normalized == "openpbr_lite";
}

static bool HasOpenPBRFields(const Json::Value& input)
{
    return input.isObject()
        && (input.isMember("base_weight")
            || input.isMember("base_color")
            || input.isMember("base_metalness")
            || input.isMember("specular_weight")
            || input.isMember("specular_roughness")
            || input.isMember("specular_roughness_anisotropy")
            || input.isMember("transmission_weight")
            || input.isMember("geometry_opacity")
            || input.isMember("fuzz_weight")
            || input.isMember("coat_weight")
            || input.isMember("subsurface_weight")
            || input.isMember("thin_film_weight")
            || input.isMember("transmission_color")
            || input.isMember("transmission_dispersion_scale"));
}

template <typename T>
static bool ReadJsonMember(const Json::Value& input, const char* name, T& value)
{
    if (!input.isObject() || !input.isMember(name))
        return false;

    input[name] >> value;
    return true;
}

static std::filesystem::path ResolveMaterialTexturePath(
    const std::filesystem::path& localPath,
    const std::filesystem::path& sceneDirectory,
    const std::filesystem::path& mediaPath,
    std::filesystem::path& localPathForStorage)
{
    localPathForStorage = localPath;
    std::filesystem::path fullPath = resolveSceneMediaPath(localPath, sceneDirectory, mediaPath);

    constexpr bool cSearchForDDS = true;
    const std::string extension = LowerCopy(fullPath.extension().string());
    if (cSearchForDDS && extension == ".png")
    {
        std::filesystem::path ddsLocalPath = localPathForStorage;
        ddsLocalPath.replace_extension(".dds");
        std::filesystem::path ddsFullPath = resolveSceneMediaPath(ddsLocalPath, sceneDirectory, mediaPath);

        if (std::filesystem::exists(ddsFullPath))
        {
            localPathForStorage = ddsLocalPath;
            fullPath = ddsFullPath;
        }
    }

    return fullPath;
}

void StandardMaterialTexture::initFromLoadedTexture(const caustica::Handle<caustica::ImageAsset>& loaded, bool _sRGB, bool _normalMap, const std::filesystem::path & mediaPath)
{
    if (loaded == nullptr)
    { localPath = ""; sRGB = false; this->loaded = nullptr; normalMap = false; enabled = false; return; }

    std::error_code relativeError;
    localPath = std::filesystem::relative(loaded->path, mediaPath, relativeError);
    if (relativeError || localPath.empty())
        localPath = std::filesystem::path(loaded->path).lexically_normal();
    sRGB = _sRGB;
    this->loaded = loaded;
    normalMap = _normalMap;
    enabled = true;
}

std::shared_ptr<StandardMaterial> StandardMaterial::safeCast(const std::shared_ptr<Material>& bridgeMaterial)
{
    if (bridgeMaterial == nullptr)
        return nullptr;

    const std::shared_ptr<MaterialEx> materialEx = std::dynamic_pointer_cast<MaterialEx>(bridgeMaterial);
    assert(materialEx != nullptr);
    return materialEx ? materialEx->standardData : nullptr;
}

void StandardMaterial::write(Json::Value& output)
{
    auto saveTexture = [ ](Json::Value& output, const StandardMaterialTexture & texture, const std::string& name)
    {
        if (texture.loaded == nullptr)
            return;
        Json::Value texJ;
        texJ["sRGB"] = texture.sRGB;
        texJ["normalMap"] = texture.normalMap;
        texJ["path"] = texture.localPath.string();
        output[name] = texJ;
    };

    //output["name"] = Name;
    output["version"] = 1;

    saveTexture(output, baseTexture, "baseTexture");
    saveTexture(output, occlusionRoughnessMetallicTexture, "occlusionRoughnessMetallicTexture");
    saveTexture(output, normalTexture, "normalTexture");
    saveTexture(output, emissiveTexture, "emissiveTexture");
    saveTexture(output, transmissionTexture, "transmissionTexture");

#define STORE_FIELD(NAME) output[#NAME] << NAME;

    STORE_FIELD(baseOrDiffuseColor);
    STORE_FIELD(specularColor);
    STORE_FIELD(emissiveColor);

    STORE_FIELD(materialModel);
    STORE_FIELD(baseWeight);
    STORE_FIELD(specularWeight);
    STORE_FIELD(anisotropy);
    STORE_FIELD(fuzzWeight);
    STORE_FIELD(fuzzColor);
    STORE_FIELD(fuzzRoughness);

    STORE_FIELD(coatWeight);
    STORE_FIELD(coatColor);
    STORE_FIELD(coatRoughness);
    STORE_FIELD(coatAnisotropy);
    STORE_FIELD(coatIor);
    STORE_FIELD(coatDarkening);

    STORE_FIELD(subsurfaceWeight);
    STORE_FIELD(subsurfaceColor);
    STORE_FIELD(subsurfaceRadius);
    STORE_FIELD(subsurfaceScale);
    STORE_FIELD(subsurfaceAnisotropy);

    STORE_FIELD(thinFilmWeight);
    STORE_FIELD(thinFilmThickness);
    STORE_FIELD(thinFilmIor);

    STORE_FIELD(transmissionColor);
    STORE_FIELD(transmissionDepth);
    STORE_FIELD(transmissionScatter);
    STORE_FIELD(transmissionScatterAnisotropy);
    STORE_FIELD(transmissionDispersionScale);
    STORE_FIELD(transmissionDispersionAbbeNumber);

    STORE_FIELD(emissiveIntensity);
    STORE_FIELD(metalness);
    STORE_FIELD(roughness);
    STORE_FIELD(opacity);
    STORE_FIELD(transmissionFactor);
    STORE_FIELD(diffuseTransmissionFactor);
    STORE_FIELD(normalTextureScale);
    STORE_FIELD(IoR);

    STORE_FIELD(useSpecularGlossModel);
    STORE_FIELD(enableBaseTexture);
    STORE_FIELD(enableOcclusionRoughnessMetallicTexture);
    STORE_FIELD(enableNormalTexture);
    STORE_FIELD(enableEmissiveTexture);
    STORE_FIELD(enableTransmissionTexture);
    STORE_FIELD(enableAlphaTesting);
    STORE_FIELD(alphaCutoff);
    STORE_FIELD(enableTransmission);
    STORE_FIELD(metalnessInRedChannel);
    STORE_FIELD(thinSurface);
    STORE_FIELD(excludeFromNEE);
    STORE_FIELD(psdExclude);
    STORE_FIELD(psdDominantDeltaLobe);
    STORE_FIELD(psdBlockMotionVectorsAtSurfaceType);
    STORE_FIELD(nestedPriority);

    STORE_FIELD(volumeAttenuationDistance);
    STORE_FIELD(volumeAttenuationColor);

    STORE_FIELD(shadowNoLFadeout);
    STORE_FIELD(unlitReceiveShadows);
    STORE_FIELD(unlitShadowStrength);

    STORE_FIELD(enableAsAnalyticLightProxy);

    STORE_FIELD(ignoreMeshTangentSpace);

    STORE_FIELD(useEngineEmissiveIntensity);

    STORE_FIELD(skipRender);

    if (IsOpenPBRMaterialModel(materialModel))
    {
        Json::Value& openPBR = output["OpenPBR"];
        openPBR["base_weight"] << baseWeight;
        openPBR["base_color"] << baseOrDiffuseColor;
        openPBR["base_metalness"] << metalness;
        openPBR["specular_weight"] << specularWeight;
        openPBR["specular_color"] << specularColor;
        openPBR["specular_roughness"] << roughness;
        openPBR["specular_roughness_anisotropy"] << anisotropy;
        openPBR["specular_ior"] << IoR;
        openPBR["transmission_weight"] << transmissionFactor;
        openPBR["transmission_diffuse_weight"] << diffuseTransmissionFactor;
        openPBR["geometry_opacity"] << opacity;
        openPBR["geometry_thin_walled"] << thinSurface;
        openPBR["emission_color"] << emissiveColor;
        openPBR["emission_luminance"] << emissiveIntensity;
        openPBR["fuzz_weight"] << fuzzWeight;
        openPBR["fuzz_color"] << fuzzColor;
        openPBR["fuzz_roughness"] << fuzzRoughness;

        openPBR["coat_weight"] << coatWeight;
        openPBR["coat_color"] << coatColor;
        openPBR["coat_roughness"] << coatRoughness;
        openPBR["coat_roughness_anisotropy"] << coatAnisotropy;
        openPBR["coat_ior"] << coatIor;
        openPBR["coat_darkening"] << coatDarkening;

        openPBR["subsurface_weight"] << subsurfaceWeight;
        openPBR["subsurface_color"] << subsurfaceColor;
        openPBR["subsurface_radius"] << subsurfaceRadius;
        openPBR["subsurface_scale"] << subsurfaceScale;
        openPBR["subsurface_anisotropy"] << subsurfaceAnisotropy;

        openPBR["thin_film_weight"] << thinFilmWeight;
        openPBR["thin_film_thickness"] << thinFilmThickness;
        openPBR["thin_film_ior"] << thinFilmIor;

        openPBR["transmission_color"] << transmissionColor;
        openPBR["transmission_depth"] << transmissionDepth;
        openPBR["transmission_scatter"] << transmissionScatter;
        openPBR["transmission_scatter_anisotropy"] << transmissionScatterAnisotropy;
        openPBR["transmission_dispersion_scale"] << transmissionDispersionScale;
        openPBR["transmission_dispersion_abbe_number"] << transmissionDispersionAbbeNumber;
    }
}

bool StandardMaterial::read(
    Json::Value& input,
    const std::filesystem::path& mediaPath,
    const std::shared_ptr<caustica::TextureLoader>& textureCache,
    const std::filesystem::path& sceneDirectory)
{
    // int version = -1;
    // input["version"] >> version;
    // if (version != 1)
    // {
    //     caustica::warning("Unsupported/missing material version"); return nullptr;
    // }

    const bool hasMaterialModelField = input.isMember("materialModel") || input.isMember("MaterialModel");
    const bool hasOpenPBRBlock = input.isMember("OpenPBR");
    const bool hasTopLevelOpenPBRFields = HasOpenPBRFields(input);

    auto loadTexture = [&](const char* camelName, const char* pascalName, StandardMaterialTexture& output)
    {
        output = StandardMaterialTexture();
        Json::Value texJ = JsonMemberEither(input, camelName, pascalName);

        if (texJ.empty())
            return;

        std::string localPath;
        texJ["path"] >> localPath; output.localPath = localPath;
        if (output.localPath == "")
        {
            caustica::warning("Path for texture is empty"); return;
        }
        texJ["sRGB"] >> output.sRGB;
        if (texJ.isMember("normalMap"))
            texJ["normalMap"] >> output.normalMap;
        else if (texJ.isMember("NormalMap"))
            texJ["NormalMap"] >> output.normalMap;

        std::filesystem::path storagePath;
        std::filesystem::path fullPath = ResolveMaterialTexturePath(
            output.localPath,
            sceneDirectory,
            mediaPath,
            storagePath);
        output.localPath = storagePath;

        output.loaded = textureCache->loadTextureFromFileDeferred(fullPath, output.sRGB);
        output.enabled = output.loaded != nullptr;
    };

    loadTexture("baseTexture", "BaseTexture", this->baseTexture);
    loadTexture("occlusionRoughnessMetallicTexture", "OcclusionRoughnessMetallicTexture", this->occlusionRoughnessMetallicTexture);
    loadTexture("normalTexture", "NormalTexture", this->normalTexture);
    loadTexture("emissiveTexture", "EmissiveTexture", this->emissiveTexture);
    loadTexture("transmissionTexture", "TransmissionTexture", this->transmissionTexture);

#define LOAD_FIELD_EITHER(CAMEL, PASCAL) \
    do { \
        if (input.isMember(#CAMEL)) \
            input[#CAMEL] >> this->CAMEL; \
        else if (input.isMember(PASCAL)) \
            input[PASCAL] >> this->CAMEL; \
    } while (0)

    LOAD_FIELD_EITHER(baseOrDiffuseColor, "BaseOrDiffuseColor");
    LOAD_FIELD_EITHER(specularColor, "SpecularColor");
    LOAD_FIELD_EITHER(emissiveColor, "EmissiveColor");

    LOAD_FIELD_EITHER(materialModel, "MaterialModel");
    LOAD_FIELD_EITHER(baseWeight, "BaseWeight");
    LOAD_FIELD_EITHER(specularWeight, "SpecularWeight");
    LOAD_FIELD_EITHER(anisotropy, "Anisotropy");
    LOAD_FIELD_EITHER(fuzzWeight, "FuzzWeight");
    LOAD_FIELD_EITHER(fuzzColor, "FuzzColor");
    LOAD_FIELD_EITHER(fuzzRoughness, "FuzzRoughness");

    LOAD_FIELD_EITHER(coatWeight, "CoatWeight");
    LOAD_FIELD_EITHER(coatColor, "CoatColor");
    LOAD_FIELD_EITHER(coatRoughness, "CoatRoughness");
    LOAD_FIELD_EITHER(coatAnisotropy, "CoatAnisotropy");
    LOAD_FIELD_EITHER(coatIor, "CoatIor");
    LOAD_FIELD_EITHER(coatDarkening, "CoatDarkening");

    LOAD_FIELD_EITHER(subsurfaceWeight, "SubsurfaceWeight");
    LOAD_FIELD_EITHER(subsurfaceColor, "SubsurfaceColor");
    LOAD_FIELD_EITHER(subsurfaceRadius, "SubsurfaceRadius");
    LOAD_FIELD_EITHER(subsurfaceScale, "SubsurfaceScale");
    LOAD_FIELD_EITHER(subsurfaceAnisotropy, "SubsurfaceAnisotropy");

    LOAD_FIELD_EITHER(thinFilmWeight, "ThinFilmWeight");
    LOAD_FIELD_EITHER(thinFilmThickness, "ThinFilmThickness");
    LOAD_FIELD_EITHER(thinFilmIor, "ThinFilmIor");

    LOAD_FIELD_EITHER(transmissionColor, "TransmissionColor");
    LOAD_FIELD_EITHER(transmissionDepth, "TransmissionDepth");
    LOAD_FIELD_EITHER(transmissionScatter, "TransmissionScatter");
    LOAD_FIELD_EITHER(transmissionScatterAnisotropy, "TransmissionScatterAnisotropy");
    LOAD_FIELD_EITHER(transmissionDispersionScale, "TransmissionDispersionScale");
    LOAD_FIELD_EITHER(transmissionDispersionAbbeNumber, "TransmissionDispersionAbbeNumber");

    LOAD_FIELD_EITHER(emissiveIntensity, "EmissiveIntensity");
    LOAD_FIELD_EITHER(metalness, "Metalness");
    LOAD_FIELD_EITHER(roughness, "Roughness");
    LOAD_FIELD_EITHER(opacity, "Opacity");
    LOAD_FIELD_EITHER(transmissionFactor, "TransmissionFactor");
    LOAD_FIELD_EITHER(diffuseTransmissionFactor, "DiffuseTransmissionFactor");
    LOAD_FIELD_EITHER(normalTextureScale, "NormalTextureScale");
    LOAD_FIELD_EITHER(IoR, "IoR");

    LOAD_FIELD_EITHER(useSpecularGlossModel, "UseSpecularGlossModel");
    LOAD_FIELD_EITHER(enableBaseTexture, "EnableBaseTexture");
    LOAD_FIELD_EITHER(enableOcclusionRoughnessMetallicTexture, "EnableOcclusionRoughnessMetallicTexture");
    LOAD_FIELD_EITHER(enableNormalTexture, "EnableNormalTexture");
    LOAD_FIELD_EITHER(enableEmissiveTexture, "EnableEmissiveTexture");
    LOAD_FIELD_EITHER(enableTransmissionTexture, "EnableTransmissionTexture");
    LOAD_FIELD_EITHER(enableAlphaTesting, "EnableAlphaTesting");
    LOAD_FIELD_EITHER(alphaCutoff, "AlphaCutoff");
    LOAD_FIELD_EITHER(enableTransmission, "EnableTransmission");
    LOAD_FIELD_EITHER(metalnessInRedChannel, "MetalnessInRedChannel");
    LOAD_FIELD_EITHER(thinSurface, "ThinSurface");
    LOAD_FIELD_EITHER(excludeFromNEE, "ExcludeFromNEE");
    LOAD_FIELD_EITHER(psdExclude, "PSDExclude");
    LOAD_FIELD_EITHER(psdBlockMotionVectorsAtSurfaceType, "PSDBlockMotionVectorsAtSurfaceType");

    LOAD_FIELD_EITHER(psdDominantDeltaLobe, "PSDDominantDeltaLobe");
    LOAD_FIELD_EITHER(nestedPriority, "NestedPriority");

    LOAD_FIELD_EITHER(volumeAttenuationDistance, "VolumeAttenuationDistance");
    LOAD_FIELD_EITHER(volumeAttenuationColor, "VolumeAttenuationColor");

    LOAD_FIELD_EITHER(shadowNoLFadeout, "ShadowNoLFadeout");
    LOAD_FIELD_EITHER(unlitReceiveShadows, "UnlitReceiveShadows");
    LOAD_FIELD_EITHER(unlitShadowStrength, "UnlitShadowStrength");

    LOAD_FIELD_EITHER(enableAsAnalyticLightProxy, "EnableAsAnalyticLightProxy");

    LOAD_FIELD_EITHER(ignoreMeshTangentSpace, "IgnoreMeshTangentSpace");

    if (input.isMember("useEngineEmissiveIntensity"))
        input["useEngineEmissiveIntensity"] >> useEngineEmissiveIntensity;
    else if (input.isMember("UseEngineEmissiveIntensity"))
        input["UseEngineEmissiveIntensity"] >> useEngineEmissiveIntensity;
    else if (input.isMember("UseDonutEmissiveIntensity"))
        input["UseDonutEmissiveIntensity"] >> useEngineEmissiveIntensity;

    LOAD_FIELD_EITHER(skipRender, "SkipRender");

    auto readOpenPBR = [this](const Json::Value& openPBR)
    {
        if (!openPBR.isObject())
            return;

        materialModel = "OpenPBR";

        if (!openPBR.isMember("specular_color"))
            specularColor = dm::float3(1.f);

        ReadJsonMember(openPBR, "base_weight", baseWeight);
        ReadJsonMember(openPBR, "base_color", baseOrDiffuseColor);
        ReadJsonMember(openPBR, "base_metalness", metalness);

        ReadJsonMember(openPBR, "specular_weight", specularWeight);
        ReadJsonMember(openPBR, "specular_color", specularColor);
        ReadJsonMember(openPBR, "specular_roughness", roughness);
        ReadJsonMember(openPBR, "specular_roughness_anisotropy", anisotropy);
        ReadJsonMember(openPBR, "specular_ior", IoR);

        bool hasSpecularTransmission = ReadJsonMember(openPBR, "transmission_weight", transmissionFactor);
        bool hasDiffuseTransmission = ReadJsonMember(openPBR, "transmission_diffuse_weight", diffuseTransmissionFactor);
        enableTransmission |= hasSpecularTransmission || hasDiffuseTransmission || transmissionFactor > 0.0f || diffuseTransmissionFactor > 0.0f;

        ReadJsonMember(openPBR, "geometry_opacity", opacity);
        ReadJsonMember(openPBR, "geometry_thin_walled", thinSurface);

        ReadJsonMember(openPBR, "emission_color", emissiveColor);
        ReadJsonMember(openPBR, "emission_luminance", emissiveIntensity);

        ReadJsonMember(openPBR, "fuzz_weight", fuzzWeight);
        ReadJsonMember(openPBR, "fuzz_color", fuzzColor);
        ReadJsonMember(openPBR, "fuzz_roughness", fuzzRoughness);

        ReadJsonMember(openPBR, "coat_weight", coatWeight);
        ReadJsonMember(openPBR, "coat_color", coatColor);
        ReadJsonMember(openPBR, "coat_roughness", coatRoughness);
        ReadJsonMember(openPBR, "coat_roughness_anisotropy", coatAnisotropy);
        ReadJsonMember(openPBR, "coat_ior", coatIor);
        ReadJsonMember(openPBR, "coat_darkening", coatDarkening);

        ReadJsonMember(openPBR, "subsurface_weight", subsurfaceWeight);
        ReadJsonMember(openPBR, "subsurface_color", subsurfaceColor);
        ReadJsonMember(openPBR, "subsurface_radius", subsurfaceRadius);
        ReadJsonMember(openPBR, "subsurface_scale", subsurfaceScale);
        ReadJsonMember(openPBR, "subsurface_anisotropy", subsurfaceAnisotropy);

        ReadJsonMember(openPBR, "thin_film_weight", thinFilmWeight);
        ReadJsonMember(openPBR, "thin_film_thickness", thinFilmThickness);
        ReadJsonMember(openPBR, "thin_film_ior", thinFilmIor);

        ReadJsonMember(openPBR, "transmission_color", transmissionColor);
        ReadJsonMember(openPBR, "transmission_depth", transmissionDepth);
        ReadJsonMember(openPBR, "transmission_scatter", transmissionScatter);
        ReadJsonMember(openPBR, "transmission_scatter_anisotropy", transmissionScatterAnisotropy);
        ReadJsonMember(openPBR, "transmission_dispersion_scale", transmissionDispersionScale);
        ReadJsonMember(openPBR, "transmission_dispersion_abbe_number", transmissionDispersionAbbeNumber);

        useSpecularGlossModel = false;
    };

    if (hasOpenPBRBlock)
        readOpenPBR(input["OpenPBR"]);
    else if ((hasMaterialModelField && IsOpenPBRMaterialModel(materialModel)) || hasTopLevelOpenPBRFields)
        readOpenPBR(input);
    else if (!hasMaterialModelField && !useSpecularGlossModel)
    {
        materialModel = "OpenPBR";
        specularColor = dm::float3(1.f);
    }
    else if (!hasMaterialModelField && useSpecularGlossModel)
        materialModel = "RTXPT";

    // Legacy RTXPT JSON often stores SpecularColor as 0 (unused in metal-rough).
    // OpenPBR uses it as dielectric specular tint — keep 0 and F0 collapses to black.
    if (IsOpenPBRMaterialModel(materialModel) && !useSpecularGlossModel
        && specularColor.x == 0.f && specularColor.y == 0.f && specularColor.z == 0.f)
    {
        specularColor = dm::float3(1.f);
    }

    baseWeight = std::clamp(baseWeight, 0.0f, 1.0f);
    specularWeight = std::max(specularWeight, 0.0f);
    anisotropy = std::clamp(anisotropy, -1.0f, 1.0f);
    fuzzWeight = std::clamp(fuzzWeight, 0.0f, 1.0f);
    fuzzRoughness = std::clamp(fuzzRoughness, 0.0f, 1.0f);
    coatWeight = std::clamp(coatWeight, 0.0f, 1.0f);
    coatRoughness = std::clamp(coatRoughness, 0.0f, 1.0f);
    coatAnisotropy = std::clamp(coatAnisotropy, -1.0f, 1.0f);
    coatIor = std::max(coatIor, 1.0f);
    coatDarkening = std::clamp(coatDarkening, 0.0f, 1.0f);
    subsurfaceWeight = std::clamp(subsurfaceWeight, 0.0f, 1.0f);
    subsurfaceRadius = std::max(subsurfaceRadius, 0.0f);
    subsurfaceScale = std::max(subsurfaceScale, 0.0f);
    subsurfaceAnisotropy = std::clamp(subsurfaceAnisotropy, -1.0f, 1.0f);
    thinFilmWeight = std::clamp(thinFilmWeight, 0.0f, 1.0f);
    thinFilmThickness = std::max(thinFilmThickness, 0.0f);
    thinFilmIor = std::max(thinFilmIor, 1.0f);
    transmissionDepth = std::max(transmissionDepth, 0.0f);
    transmissionScatterAnisotropy = std::clamp(transmissionScatterAnisotropy, -1.0f, 1.0f);
    transmissionDispersionScale = std::clamp(transmissionDispersionScale, 0.0f, 1.0f);
    transmissionDispersionAbbeNumber = std::max(transmissionDispersionAbbeNumber, 0.0f);

    return true;
}

std::shared_ptr<StandardMaterial> StandardMaterial::fromJson(
    Json::Value& input,
    const std::filesystem::path& mediaPath,
    const std::shared_ptr<caustica::TextureLoader>& textureCache,
    const std::string& modelName,
    const std::string& name,
    const std::filesystem::path& sceneDirectory)
{
    std::shared_ptr<StandardMaterial> material = std::make_shared<StandardMaterial>();

    material->read(input, mediaPath, textureCache, sceneDirectory);
    material->name = name;
    material->modelName = modelName;

    return material;
}

StandardMaterialTexture& StandardMaterial::getTexture(StandardMaterialTextureSlot slot)
{
    switch (slot)
    {
    case StandardMaterialTextureSlot::Base:
        return baseTexture;
    case StandardMaterialTextureSlot::OcclusionRoughnessMetallic:
        return occlusionRoughnessMetallicTexture;
    case StandardMaterialTextureSlot::Normal:
        return normalTexture;
    case StandardMaterialTextureSlot::Emissive:
        return emissiveTexture;
    case StandardMaterialTextureSlot::Transmission:
        return transmissionTexture;
    default:
        assert(false);
        return baseTexture;
    }
}

const StandardMaterialTexture& StandardMaterial::getTexture(StandardMaterialTextureSlot slot) const
{
    return const_cast<StandardMaterial*>(this)->getTexture(slot);
}

bool StandardMaterial::isTextureEnabled(StandardMaterialTextureSlot slot) const
{
    switch (slot)
    {
    case StandardMaterialTextureSlot::Base:
        return enableBaseTexture;
    case StandardMaterialTextureSlot::OcclusionRoughnessMetallic:
        return enableOcclusionRoughnessMetallicTexture;
    case StandardMaterialTextureSlot::Normal:
        return enableNormalTexture;
    case StandardMaterialTextureSlot::Emissive:
        return enableEmissiveTexture;
    case StandardMaterialTextureSlot::Transmission:
        return enableTransmissionTexture;
    default:
        assert(false);
        return false;
    }
}

void StandardMaterial::setTextureEnabled(StandardMaterialTextureSlot slot, bool enabled)
{
    switch (slot)
    {
    case StandardMaterialTextureSlot::Base:
        enableBaseTexture = enabled;
        break;
    case StandardMaterialTextureSlot::OcclusionRoughnessMetallic:
        enableOcclusionRoughnessMetallicTexture = enabled;
        break;
    case StandardMaterialTextureSlot::Normal:
        enableNormalTexture = enabled;
        break;
    case StandardMaterialTextureSlot::Emissive:
        enableEmissiveTexture = enabled;
        break;
    case StandardMaterialTextureSlot::Transmission:
        enableTransmissionTexture = enabled;
        break;
    default:
        assert(false);
        break;
    }
}

bool StandardMaterial::editorGui(MaterialGpuCache & cache)
{
    bool update = false;

    float itemWidth = ImGui::CalcItemWidth();

    auto getShortTexturePath = [ ](const StandardMaterialTexture & texture) -> std::string
    {
        if( texture.loaded == nullptr ) return "<nullptr>";
        return texture.localPath.string();
    };

    if (ImGui::CollapsingHeader("Special Properties"))
    {
        ImGui::Indent();
        {
            UI_SCOPED_DISABLE(!enableTransmission);
            update |= ImGui::Checkbox("Thin surface", &thinSurface);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Material has no volumetric properties - used for double sided thin surfaces like leafs.\nNon-transparent materials are always considered thin surface.");

        update |= ImGui::Checkbox("Ignore by NEE shadow ray (bias!)", &excludeFromNEE);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ignored for shadow rays during Next Event Estimation\nNote: this isn't physically correct - it adds bias!");

        update |= ImGui::SliderFloat("Shadow NoL Fadeout", &shadowNoLFadeout, 0.0f, 0.2f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Low tessellation geometry often has triangle (flat) normals that differ significantly from shading normals. \n"
            "This causes shading vs shadow discrepancy that exposes triangle edges. One way to mitigate this (other than \n"
            "having more detailed mesh) is to add additional shadowing falloff to hide the seam. This setting is not \n"
            "physically correct and adds bias. Setting of 0 means no fadeout (default).");

        update |= ImGui::Checkbox("Unlit, receive shadows", &unlitReceiveShadows);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Display the material's base color without BRDF, direct, or indirect lighting,\n"
            "but keep shadows from sampled lights.");
        {
            UI_SCOPED_DISABLE(!unlitReceiveShadows);
            update |= ImGui::SliderFloat("Unlit shadow strength", &unlitShadowStrength, 0.0f, 1.0f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Artistic control for how strongly sampled-light visibility darkens the unlit color.\n"
                "0 keeps the base color fully visible; 1 applies the full shadow mask.");
        }

        update |= ImGui::Checkbox("Enable as analytic light proxy", &enableAsAnalyticLightProxy);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Any scene object with this material will look at it's parent node in the scenegraph; if the parent contains\n"
            "an analytic light, the rays falling of this surface will also output radiance from the analytic light.\n"
            "The more closely the object's mesh resembles the analytic light, the more physically correct results will be.\n");

        update |= ImGui::Checkbox("Emissive intensity from engine material", &useEngineEmissiveIntensity);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Engine materials can have emissive intensity animation attached; this allows RTXPT to use it\n");

        update |= ImGui::Checkbox("Ignore mesh tangent space", &ignoreMeshTangentSpace);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("This will ignore tangent space loaded from the mesh and generate new one - can help issues with normals.");

        update |= ImGui::Checkbox("Skip render", &skipRender);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ignore all meshes with this material - sometimes easier than removing the object itself.\nNote: this will not remove it as an emissive light on the light sampling side!");

        std::string fullName = uniqueFullName();
        ImGui::TextWrapped("Full, unique ID: %s", fullName.c_str());
        if (ImGui::Button("Copy to clipboard"))
            ImGui::SetClipboardText(fullName.c_str());

        ImGui::Unindent();
    }

    const ImVec4 filenameColor = ImVec4(0.474f, 0.722f, 0.176f, 1.0f);

    auto drawTextureToggle = [&](const char* label, StandardMaterialTexture& texture, bool& enabled)
    {
        if (texture.loaded != nullptr)
        {
            update |= ImGui::Checkbox(label, &enabled);
            ImGui::SameLine();
            ImGui::TextColored(filenameColor, "%s", getShortTexturePath(texture).c_str());
        }
    };

    bool normalizedToOpenPBR = false;
    if (!IsOpenPBRMaterialModel(materialModel))
    {
        materialModel = "OpenPBR";
        normalizedToOpenPBR = true;
    }
    if (useSpecularGlossModel)
    {
        useSpecularGlossModel = false;
        normalizedToOpenPBR = true;
    }
    if (normalizedToOpenPBR)
    {
        const float* specColor = specularColor.data();
        if (specColor[0] == 0.f && specColor[1] == 0.f && specColor[2] == 0.f)
            specularColor = dm::float3(1.f);
        update = true;
    }

    drawTextureToggle("Use base_color texture", baseTexture, enableBaseTexture);

    update |= ImGui::ColorEdit3(enableBaseTexture ? "base_color factor" : "base_color", baseOrDiffuseColor.data(), ImGuiColorEditFlags_Float);
    update |= ImGui::SliderFloat("base_weight", &baseWeight, 0.f, 1.f);

    drawTextureToggle("Use base_metalness/specular_roughness texture", occlusionRoughnessMetallicTexture, enableOcclusionRoughnessMetallicTexture);

    update |= ImGui::SliderFloat(enableOcclusionRoughnessMetallicTexture ? "base_metalness factor" : "base_metalness", &metalness, 0.f, 1.f);
    update |= ImGui::SliderFloat("specular_weight", &specularWeight, 0.f, 2.f);
    update |= ImGui::ColorEdit3("specular_color", specularColor.data(), ImGuiColorEditFlags_Float);
    update |= ImGui::SliderFloat(enableOcclusionRoughnessMetallicTexture ? "specular_roughness factor" : "specular_roughness", &roughness, 0.f, 1.f);
    update |= ImGui::SliderFloat("specular_roughness_anisotropy", &anisotropy, -1.f, 1.f);
    update |= ImGui::InputFloat("specular_ior", &IoR);
    if (IoR < 1.0f) { IoR = 1.0f; update = true; }

    update |= ImGui::SliderFloat("fuzz_weight", &fuzzWeight, 0.f, 1.f);
    update |= ImGui::ColorEdit3("fuzz_color", fuzzColor.data(), ImGuiColorEditFlags_Float);
    update |= ImGui::SliderFloat("fuzz_roughness", &fuzzRoughness, 0.f, 1.f);

    if (ImGui::CollapsingHeader("Coat", ImGuiTreeNodeFlags_DefaultOpen))
    {
        update |= ImGui::SliderFloat("coat_weight", &coatWeight, 0.f, 1.f);
        update |= ImGui::ColorEdit3("coat_color", coatColor.data(), ImGuiColorEditFlags_Float);
        update |= ImGui::SliderFloat("coat_roughness", &coatRoughness, 0.f, 1.f);
        update |= ImGui::SliderFloat("coat_roughness_anisotropy", &coatAnisotropy, -1.f, 1.f);
        update |= ImGui::InputFloat("coat_ior", &coatIor);
        if (coatIor < 1.0f) { coatIor = 1.0f; update = true; }
        update |= ImGui::SliderFloat("coat_darkening", &coatDarkening, 0.f, 1.f);
    }

    if (ImGui::CollapsingHeader("Subsurface"))
    {
        update |= ImGui::SliderFloat("subsurface_weight", &subsurfaceWeight, 0.f, 1.f);
        update |= ImGui::ColorEdit3("subsurface_color", subsurfaceColor.data(), ImGuiColorEditFlags_Float);
        update |= ImGui::InputFloat("subsurface_radius", &subsurfaceRadius);
        if (subsurfaceRadius < 0.0f) { subsurfaceRadius = 0.0f; update = true; }
        update |= ImGui::InputFloat("subsurface_scale", &subsurfaceScale);
        if (subsurfaceScale < 0.0f) { subsurfaceScale = 0.0f; update = true; }
        update |= ImGui::SliderFloat("subsurface_anisotropy", &subsurfaceAnisotropy, -1.f, 1.f);
    }

    if (ImGui::CollapsingHeader("Thin Film"))
    {
        update |= ImGui::SliderFloat("thin_film_weight", &thinFilmWeight, 0.f, 1.f);
        update |= ImGui::SliderFloat("thin_film_thickness", &thinFilmThickness, 0.f, 3.f);
        update |= ImGui::InputFloat("thin_film_ior", &thinFilmIor);
        if (thinFilmIor < 1.0f) { thinFilmIor = 1.0f; update = true; }
    }

    update |= ImGui::SliderFloat("geometry_opacity", &opacity, 0.f, 1.f);
    update |= ImGui::Checkbox("geometry_thin_walled", &thinSurface);

    drawTextureToggle("Use transmission_weight texture", transmissionTexture, enableTransmissionTexture);

    float previousTransmissionFactor = transmissionFactor;
    float previousDiffuseTransmissionFactor = diffuseTransmissionFactor;
    update |= ImGui::SliderFloat("transmission_weight", &transmissionFactor, 0.f, 1.f);
    update |= ImGui::SliderFloat("transmission_diffuse_weight", &diffuseTransmissionFactor, 0.f, 1.f);
    update |= ImGui::ColorEdit3("transmission_color", transmissionColor.data(), ImGuiColorEditFlags_Float);
    update |= ImGui::InputFloat("transmission_depth", &transmissionDepth);
    if (transmissionDepth < 0.0f) { transmissionDepth = 0.0f; update = true; }
    update |= ImGui::ColorEdit3("transmission_scatter", transmissionScatter.data(), ImGuiColorEditFlags_Float);
    update |= ImGui::SliderFloat("transmission_scatter_anisotropy", &transmissionScatterAnisotropy, -1.f, 1.f);
    update |= ImGui::SliderFloat("transmission_dispersion_scale", &transmissionDispersionScale, 0.f, 1.f);
    update |= ImGui::InputFloat("transmission_dispersion_abbe_number", &transmissionDispersionAbbeNumber);
    if (transmissionDispersionAbbeNumber < 0.0f) { transmissionDispersionAbbeNumber = 0.0f; update = true; }

    bool openPBRTransmissionEnabled = (transmissionFactor > 0.f) || (diffuseTransmissionFactor > 0.f);
    if (openPBRTransmissionEnabled != enableTransmission)
    {
        enableTransmission = openPBRTransmissionEnabled;
        update = true;
    }
    if (previousTransmissionFactor != transmissionFactor || previousDiffuseTransmissionFactor != diffuseTransmissionFactor)
        enableTransmission = openPBRTransmissionEnabled;

    if (enableTransmission && !thinSurface)
    {
        update |= ImGui::InputFloat("volume_attenuation_distance", &volumeAttenuationDistance);
        if (volumeAttenuationDistance < 0.0f) { volumeAttenuationDistance = 0.0f; update = true; }
        update |= ImGui::ColorEdit3("volume_attenuation_color", volumeAttenuationColor.data(), ImGuiColorEditFlags_Float);
        update |= ImGui::InputInt("nested_priority", &nestedPriority);
        if (nestedPriority < 0 || nestedPriority > 14) { nestedPriority = dm::clamp(nestedPriority, 0, 14); update = true; }
    }

    update |= ImGui::Checkbox("geometry_enable_alpha_test", &enableAlphaTesting);

    if (enableAlphaTesting && baseTexture.loaded)
    {
        update |= ImGui::SliderFloat("geometry_alpha_cutoff", &alphaCutoff, 0.f, 1.f);
    }

    drawTextureToggle("Use geometry_normal texture", normalTexture, enableNormalTexture);

    if (enableNormalTexture)
    {
        ImGui::SetNextItemWidth(itemWidth - 31.f);
        update |= ImGui::SliderFloat("###normtexscale", &normalTextureScale, -2.f, 2.f);
        ImGui::SameLine(0.f, 5.f);
        ImGui::SetNextItemWidth(26.f);
        if (ImGui::Button("1.0"))
        {
            normalTextureScale = 1.f;
            update = true;
        }
        ImGui::SameLine();
        ImGui::Text("geometry_normal_scale");
    }

    drawTextureToggle("Use emission_color texture", emissiveTexture, enableEmissiveTexture);

    update |= ImGui::ColorEdit3("emission_color", emissiveColor.data(), ImGuiColorEditFlags_Float);
    update |= ImGui::SliderFloat("emission_luminance", &emissiveIntensity, 0.f, 100000.f, "%.3f", ImGuiSliderFlags_Logarithmic);

    if (ImGui::CollapsingHeader("Path Decomposition"))
    {
        ImGui::Indent();

        update |= ImGui::Combo("Block mv-s at surface", (int*)&psdBlockMotionVectorsAtSurfaceType, "Off\0AutoLow\0AutoHigh\0Full\0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Curved surfaces cause motion vectors on reflected or transmitted\nsegments to be incorrect and are better disabled.\n"
            "When this is enabled, motion for all de-composited paths will come\nfrom this surface. AutoLow and AutoHigh will attempt to set the flag based on\n surface curvature (with Low and High sensitivities).");

        bool psdEnable = !psdExclude; // makes more sense from UI perspective - avoids double negative
        update |= ImGui::Checkbox("Enable delta lobe decomposition", &psdEnable);
        psdExclude = !psdEnable;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Some materials/meshes look best without decomposition.");
        
        {
            UI_SCOPED_DISABLE(psdExclude);
            int dominantDeltaLobeP1 = dm::clamp(psdDominantDeltaLobe, -1, 2) + 1;
            update |= ImGui::Combo("Dominant bounce", &dominantDeltaLobeP1, "None (surface)\0Transparency\0Reflection\0Coat\0\0");
            psdDominantDeltaLobe = dm::clamp(dominantDeltaLobeP1 - 1, -1, 2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Determines which surface will:\n * provide motion vectors for denoising\n * get ReSTIR DI lighting\n * get 'boost samples' for NEE lighting");
        }
        ImGui::Unindent();
    }

    if (ImGui::CollapsingHeader("Save/load"))
    {
        RAII_SCOPE( ImGui::Indent();, ImGui::Unindent(); );

        ImGui::Checkbox("Share with all scenes", &sharedWithAllScenes);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("if checked, material saved to /Assets/Materials/ path and \nshared between all scenes; otherwise it will be saved to \n/Assets/Materials/SceneName specific to current scene");

        auto matPath = cache.getMaterialStoragePath(*this);

        ImGui::TextWrapped("File name: %s", matPath.string().c_str());

        if (ImGui::Button("load"))
        {
            cache.loadSingle(*this);
            update = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Save"))
            cache.saveSingle(*this);
    }

    // mark for update
    gpuDataDirty |= update;
    if (update)
        cache.notifyMaterialEdited();

    return update;
}

static void GetBindlessTextureIndex(const Handle<ImageAsset>& texture, uint& outEncodedInfo, unsigned int& flags, unsigned int textureBit)
{
    // if bit not set, don't set the texture; if texture unavailable - remove the texture bit!
    if ((flags & textureBit) == 0 || texture == nullptr || texture->gpu.texture == nullptr || texture->gpu.bindlessDescriptor.Get() == ~0u)
    {
        outEncodedInfo = 0xFFFFFFFF;
        flags &= ~textureBit; // remove flag
        return;
    }

    uint bindlessDescIndex = texture->gpu.bindlessDescriptor.Get();
    assert(bindlessDescIndex <= 0xFFFF);
    bindlessDescIndex &= 0xFFFF;

    const auto desc = texture->gpu.texture->getDesc();
    float baseLODf = caustica::math::log2f((float)desc.width * desc.height);
    uint baseLOD = (uint)(baseLODf + 0.5f);
    uint mipLevels = desc.mipLevels;
    assert(baseLOD >= 0 && baseLOD <= 255);
    assert(mipLevels >= 0 && mipLevels <= 255);

    outEncodedInfo = (baseLOD << 24) | (mipLevels << 16) | bindlessDescIndex;
}

bool StandardMaterial::isEmissive() const
{
    if (unlitReceiveShadows)
        return false;
    return (emissiveIntensity > 0) && (caustica::math::any(emissiveColor>0.0f)) || useEngineEmissiveIntensity;  // useEngineEmissiveIntensity can animate on/off so just assume we're emissive and pay the cost
}

void StandardMaterial::fillData(StandardMaterialData & data)
{
    // flags

    data.Flags = 0;

    if (useSpecularGlossModel)
        data.Flags |= StandardMaterialFlags_UseSpecularGlossModel;

    if (baseTexture.loaded && enableBaseTexture)
        data.Flags |= StandardMaterialFlags_UseBaseOrDiffuseTexture;

    if (occlusionRoughnessMetallicTexture.loaded && enableOcclusionRoughnessMetallicTexture)
        data.Flags |= StandardMaterialFlags_UseMetalRoughOrSpecularTexture;

    if (emissiveTexture.loaded && enableEmissiveTexture)
        data.Flags |= StandardMaterialFlags_UseEmissiveTexture;

    if (normalTexture.loaded && enableNormalTexture)
        data.Flags |= StandardMaterialFlags_UseNormalTexture;

    if (transmissionTexture.loaded && enableTransmissionTexture && enableTransmission)
        data.Flags |= StandardMaterialFlags_UseTransmissionTexture;

    if (metalnessInRedChannel)
        data.Flags |= StandardMaterialFlags_MetalnessInRedChannel;

    if (thinSurface || !enableTransmission) // materials with no transmission are automatically considered thin surface - simplifies a lot of things
        data.Flags |= StandardMaterialFlags_ThinSurface;

    if (psdExclude)
        data.Flags |= StandardMaterialFlags_PSDExclude;

    if (psdBlockMotionVectorsAtSurfaceType % 2)
        data.Flags |= StandardMaterialFlags_PSDBlockMVsAtSurfaceTypeB0;

    if (psdBlockMotionVectorsAtSurfaceType / 2)
        data.Flags |= StandardMaterialFlags_PSDBlockMVsAtSurfaceTypeB1;

    if (enableAsAnalyticLightProxy)
        data.Flags |= StandardMaterialFlags_EnableAsAnalyticLightProxy;

    if (ignoreMeshTangentSpace)
        data.Flags |= StandardMaterialFlags_IgnoreMeshTangentSpace;

    if (IsOpenPBRMaterialModel(materialModel))
        data.Flags |= StandardMaterialFlags_UseOpenPBRMaterialModel;

    if (unlitReceiveShadows)
        data.Flags |= StandardMaterialFlags_UnlitReceiveShadows;

    // free parameters

    data.BaseOrDiffuseColor = baseOrDiffuseColor;
    data.SpecularColor = specularColor;
    data.EmissiveColor = emissiveColor * emissiveIntensity;
    data.Roughness = roughness;
    data.Metalness = metalness;
    data.BaseWeight = std::clamp(baseWeight, 0.0f, 1.0f);
    data.SpecularWeight = std::max(specularWeight, 0.0f);
    data.Anisotropy = std::clamp(anisotropy, -1.0f, 1.0f);
    data.FuzzWeight = std::clamp(fuzzWeight, 0.0f, 1.0f);
    data.FuzzColor = fuzzColor;
    data.FuzzRoughness = std::clamp(fuzzRoughness, 0.0f, 1.0f);
    data.NormalTextureScale = normalTextureScale;
    data.TransmissionFactor = (enableTransmission)?(transmissionFactor):(0);
    data.DiffuseTransmissionFactor = (enableTransmission)?(diffuseTransmissionFactor):(0);
    data.Opacity = opacity;
    data.AlphaCutoff = alphaCutoff;
    data.IoR = IoR;
    data.Volume.AttenuationColor    = volumeAttenuationColor;
    data.Volume.AttenuationDistance = volumeAttenuationDistance;

    data.CoatWeight = std::clamp(coatWeight, 0.0f, 1.0f);
    data.CoatColor = coatColor;
    data.CoatRoughness = std::clamp(coatRoughness, 0.0f, 1.0f);
    data.CoatAnisotropy = std::clamp(coatAnisotropy, -1.0f, 1.0f);
    data.CoatIor = std::max(coatIor, 1.0f);
    data.CoatDarkening = std::clamp(coatDarkening, 0.0f, 1.0f);

    data.SubsurfaceWeight = std::clamp(subsurfaceWeight, 0.0f, 1.0f);
    data.SubsurfaceColor = subsurfaceColor;
    data.SubsurfaceRadius = std::max(subsurfaceRadius, 0.0f);
    data.SubsurfaceScale = std::max(subsurfaceScale, 0.0f);
    data.SubsurfaceAnisotropy = std::clamp(subsurfaceAnisotropy, -1.0f, 1.0f);

    data.ThinFilmWeight = std::clamp(thinFilmWeight, 0.0f, 1.0f);
    data.ThinFilmThickness = std::max(thinFilmThickness, 0.0f);
    data.ThinFilmIor = std::max(thinFilmIor, 1.0f);

    data.TransmissionColor = transmissionColor;
    data.TransmissionDepth = std::max(transmissionDepth, 0.0f);
    data.TransmissionScatter = transmissionScatter;
    data.TransmissionScatterAnisotropy = std::clamp(transmissionScatterAnisotropy, -1.0f, 1.0f);
    data.TransmissionDispersionScale = std::clamp(transmissionDispersionScale, 0.0f, 1.0f);
    data.TransmissionDispersionAbbeNumber = std::max(transmissionDispersionAbbeNumber, 0.0f);
    data.UnlitShadowStrength = std::clamp(unlitShadowStrength, 0.0f, 1.0f);
    data._padOpenPBR1 = data._padOpenPBR2 = 0.f;

    // bindless textures

    GetBindlessTextureIndex(baseTexture.loaded, data.BaseOrDiffuseTextureIndex, data.Flags, StandardMaterialFlags_UseBaseOrDiffuseTexture);
    GetBindlessTextureIndex(occlusionRoughnessMetallicTexture.loaded, data.MetalRoughOrSpecularTextureIndex, data.Flags, StandardMaterialFlags_UseMetalRoughOrSpecularTexture);
    GetBindlessTextureIndex(emissiveTexture.loaded, data.EmissiveTextureIndex, data.Flags, StandardMaterialFlags_UseEmissiveTexture);
    GetBindlessTextureIndex(normalTexture.loaded, data.NormalTextureIndex, data.Flags, StandardMaterialFlags_UseNormalTexture);
    GetBindlessTextureIndex(transmissionTexture.loaded, data.TransmissionTextureIndex, data.Flags, StandardMaterialFlags_UseTransmissionTexture);

    data.Flags |= (uint)(min(nestedPriority, kMaterialMaxNestedPriority)) << StandardMaterialFlags_NestedPriorityShift;
    data.Flags |= (uint)(clamp(psdDominantDeltaLobe + 1, 0, 7)) << StandardMaterialFlags_PSDDominantDeltaLobeP1Shift;

    data.ShadowNoLFadeout = std::clamp(shadowNoLFadeout, 0.0f, 0.25f);
}

std::filesystem::path MaterialGpuCache::getMaterialStoragePath(StandardMaterialBase& material)
{
    std::filesystem::path matPath = m_materialsPath;
    if (!material.sharedWithAllScenes)
        matPath = m_materialsSceneSpecializedPath;
    std::string fileName = material.name + c_MaterialsExtension;
    if (material.modelName != kNoModel)
        fileName = material.modelName + "." + fileName;
    matPath /= fileName;
    return matPath;
}

MaterialGpuCache::MaterialGpuCache(const std::string & relativeShaderSourcePath, caustica::rhi::IDevice* device, std::shared_ptr<caustica::TextureLoader> textureCache, std::shared_ptr<caustica::ShaderFactory> shaderFactory)
    : m_relativeShaderSourcePath(relativeShaderSourcePath)
    , m_device(device)
    , m_textureCache(textureCache)
    , m_bindingCache(device)
    , m_shaderFactory(shaderFactory)
{
}

static bool DefaultTextureSRGB(const StandardMaterial& material, StandardMaterialTextureSlot slot)
{
    switch (slot)
    {
    case StandardMaterialTextureSlot::Base:
    case StandardMaterialTextureSlot::Emissive:
        return true;
    case StandardMaterialTextureSlot::OcclusionRoughnessMetallic:
        return material.useSpecularGlossModel;
    case StandardMaterialTextureSlot::Normal:
    case StandardMaterialTextureSlot::Transmission:
        return false;
    default:
        assert(false);
        return false;
    }
}

static bool DefaultTextureNormalMap(StandardMaterialTextureSlot slot)
{
    return slot == StandardMaterialTextureSlot::Normal;
}

void MaterialGpuCache::recordTexture(const StandardMaterialTexture& texture)
{
    if (texture.loaded == nullptr)
        return;

    if (texture.localPath.empty())
    {
        caustica::warning("Skipping loaded texture with no storage path.");
        return;
    }

    auto existing = m_textures.find(texture.localPath.generic_string());
    if (existing != m_textures.end())
    {
        if (existing->second.normalMap != texture.normalMap)
        {
            caustica::warning("Texture with path '%s' is used as a normalMap and not a normalMap - this is not supported, expect errors.", texture.localPath.string().c_str());
            assert(false);
        }
        if (existing->second.sRGB != texture.sRGB)
        {
            caustica::warning("Texture with path '%s' is marked as both sRGB and not sRGB in different places - this is not supported, expect errors.", texture.localPath.string().c_str());
            assert(false);
        }
    }
    else
    {
        m_textures.insert(std::make_pair(texture.localPath.generic_string(), texture));
    }
}

bool MaterialGpuCache::setMaterialTexture(
    StandardMaterial& material,
    StandardMaterialTextureSlot slot,
    const std::filesystem::path& localPath,
    std::optional<bool> sRGB,
    std::optional<bool> normalMap)
{
    if (localPath.empty())
    {
        clearMaterialTexture(material, slot);
        return true;
    }

    if (m_textureCache == nullptr)
        return false;

    std::filesystem::path storagePath;
    const std::filesystem::path fullPath = ResolveMaterialTexturePath(
        localPath,
        m_sceneDirectory,
        m_mediaPath,
        storagePath);

    if (!std::filesystem::exists(fullPath))
    {
        caustica::warning("Material texture '%s' resolved to missing file '%s'.", localPath.string().c_str(), fullPath.string().c_str());
        return false;
    }

    StandardMaterialTexture& texture = material.getTexture(slot);
    texture.localPath = storagePath;
    texture.sRGB = sRGB.value_or(DefaultTextureSRGB(material, slot));
    texture.normalMap = normalMap.value_or(DefaultTextureNormalMap(slot));
    texture.loaded = m_textureCache->loadTextureFromFileDeferred(fullPath, texture.sRGB);
    texture.enabled = texture.loaded != nullptr;

    if (texture.loaded == nullptr ||
        (!m_textureCache->isTextureLoaded(texture.loaded) && !m_textureCache->isTextureFinalized(texture.loaded)))
    {
        texture = StandardMaterialTexture();
        texture.enabled = false;
        return false;
    }

    material.setTextureEnabled(slot, true);
    material.gpuDataDirty = true;
    notifyMaterialEdited();

    m_deferredTextureLoadInProgress = true;
    rebuildActiveTextureIndex();
    return true;
}

void MaterialGpuCache::clearMaterialTexture(StandardMaterial& material, StandardMaterialTextureSlot slot)
{
    StandardMaterialTexture& texture = material.getTexture(slot);
    texture = StandardMaterialTexture();
    texture.enabled = false;

    material.setTextureEnabled(slot, false);
    material.gpuDataDirty = true;
    notifyMaterialEdited();
    rebuildActiveTextureIndex();
}

void MaterialGpuCache::initializeUniqueDeterministicName(const std::shared_ptr<StandardMaterialBase> & material)
{
    std::string hashBase = material->modelName + "_" + material->name;
    uint evenShorterHash = ShortHash(HashMyString(hashBase));
    
    std::string uniqueName = stripNonAsciiAlnum(material->name).substr(0, 16) + "_" + hexString(evenShorterHash);

    auto findV = m_uniqueNames.find(uniqueName);
    if (findV == m_uniqueNames.end())
    {
        m_uniqueNames.insert({uniqueName, material});
    }
    else
    {
        assert( false ); // hash collision? name collision?
        static int errorNumber = 0;
        uniqueName = "hasherror_" + std::to_string(errorNumber); errorNumber++;
        m_uniqueNames.insert({uniqueName, material});
    }
    material->uniqueName = uniqueName;
}

void MaterialGpuCache::clear() 
{
    for (auto& material : m_materials)
    {
        if (material)
            material->runtimeMaterialGpuCache = nullptr;
    }

    m_materialDataWasReset = true;
    m_materials.clear();
    m_materialsById.clear();
    m_materialsGPU.clear();
    m_textures.clear();
    m_mediaPath = std::filesystem::path();
    m_sceneDirectory = std::filesystem::path();
    m_sceneMaterialsPath = std::filesystem::path();
    m_sceneMaterialsSceneSpecializedPath = std::filesystem::path();
    m_materialsPath = std::filesystem::path();
    m_materialsSceneSpecializedPath = std::filesystem::path();
    m_uniqueNames.clear();
    notifyMaterialEdited();
}

MaterialGpuCache::~MaterialGpuCache()
{
    clear();
}

static std::string ModelNameFromModelFileName(const std::string& modelFileName)
{
    constexpr const char* builtinPrefix = "builtin:";
    std::string normalized = modelFileName;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return char(std::tolower(ch));
    });
    if (normalized.rfind(builtinPrefix, 0) == 0)
    {
        normalized.erase(0, std::strlen(builtinPrefix));
        return std::string("builtin_") + normalized;
    }

    std::filesystem::path modelFileNamePath = modelFileName;
    std::filesystem::path modelName = modelFileNamePath.filename();
    modelName.replace_extension();
    return modelName.string();
}

static bool IsBuiltinModelFileName(const std::string& modelFileName)
{
    constexpr const char* builtinPrefix = "builtin:";
    std::string normalized = modelFileName;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return char(std::tolower(ch));
    });
    return normalized.rfind(builtinPrefix, 0) == 0;
}

std::shared_ptr<StandardMaterial> MaterialGpuCache::importFromEngineMaterial(
    const caustica::scene::MaterialRenderResourceSnapshot& material)
{
    std::shared_ptr<StandardMaterial> standardMaterial = std::make_shared<StandardMaterial>();

    standardMaterial->name = material.debugName;
    standardMaterial->modelName = ModelNameFromModelFileName(material.modelFileName);

    standardMaterial->baseTexture.initFromLoadedTexture(material.baseOrDiffuseTexture, true, false, m_mediaPath);

    if( material.useSpecularGlossModel ) // spec-gloss model is a special case hack where we use metalRoughOrSpecularTexture to store specular color, which is handled as sRGB
        standardMaterial->occlusionRoughnessMetallicTexture.initFromLoadedTexture(material.metalRoughOrSpecularTexture, true, false, m_mediaPath);
    else
        standardMaterial->occlusionRoughnessMetallicTexture.initFromLoadedTexture(material.metalRoughOrSpecularTexture, false, false, m_mediaPath);

    standardMaterial->normalTexture.initFromLoadedTexture(material.normalTexture, false, true, m_mediaPath);
    standardMaterial->emissiveTexture.initFromLoadedTexture(material.emissiveTexture, true, false, m_mediaPath);
    standardMaterial->transmissionTexture.initFromLoadedTexture(material.transmissionTexture, false, false, m_mediaPath);

    // Toggles for the textures. Only effective if the corresponding texture is non-null.
    standardMaterial->enableBaseTexture = material.enableBaseOrDiffuseTexture;
    standardMaterial->enableOcclusionRoughnessMetallicTexture = material.enableMetalRoughOrSpecularTexture;
    standardMaterial->enableNormalTexture = material.enableNormalTexture;
    standardMaterial->enableEmissiveTexture = material.enableEmissiveTexture;
    standardMaterial->enableTransmissionTexture = material.enableTransmissionTexture;

    standardMaterial->baseOrDiffuseColor = material.baseOrDiffuseColor;
    // Engine Material defaults specularColor to 0 (metal-rough unused). OpenPBR uses it as
    // dielectric specular tint — leave 0 and EvaluateStandardMaterial zeros specular F0.
    const bool zeroSpecular =
        material.specularColor.x == 0.f && material.specularColor.y == 0.f && material.specularColor.z == 0.f;
    standardMaterial->specularColor = (!material.useSpecularGlossModel && zeroSpecular)
        ? dm::float3(1.f)
        : material.specularColor;
    standardMaterial->emissiveColor = material.emissiveColor;

    standardMaterial->emissiveIntensity = material.emissiveIntensity;
    standardMaterial->metalness = material.metalness;
    standardMaterial->roughness = material.roughness;
    standardMaterial->opacity = material.opacity;
    standardMaterial->alphaCutoff = material.alphaCutoff;
    standardMaterial->transmissionFactor = material.transmissionFactor;
    //standardMaterial->diffuseTransmissionFactor = material.diffuseTransmissionFactor;
    standardMaterial->normalTextureScale = material.normalTextureScale;
    standardMaterial->useSpecularGlossModel = material.useSpecularGlossModel;
    standardMaterial->metalnessInRedChannel = material.metalnessInRedChannel;
    // OpenPBR is the built-in model; imported engine materials should shade as OpenPBR.
    if (!material.useSpecularGlossModel)
        standardMaterial->materialModel = "OpenPBR";

    standardMaterial->enableAlphaTesting = (material.domain == MaterialDomain::AlphaTested || material.domain == MaterialDomain::TransmissiveAlphaTested);
    standardMaterial->enableTransmission = (material.domain == MaterialDomain::Transmissive || material.domain == MaterialDomain::TransmissiveAlphaBlended || material.domain == MaterialDomain::TransmissiveAlphaTested);

    return standardMaterial;
}

std::shared_ptr<StandardMaterial> MaterialGpuCache::load(const std::string & modelFileName, const std::string & name)
{
    std::string modelName = ModelNameFromModelFileName(modelFileName);

    struct MaterialFileCandidate
    {
        std::filesystem::path path;
        bool sharedWithAllScenes = true;
        bool nameOnly = false;
    };

    std::vector<MaterialFileCandidate> candidates;
    auto appendCandidates = [&](const std::filesystem::path& folder, bool sharedWithAllScenes)
    {
        if (folder.empty())
            return;

        MaterialFileCandidate modelAndName;
        modelAndName.path = folder / (modelName + "." + name + c_MaterialsExtension);
        modelAndName.sharedWithAllScenes = sharedWithAllScenes;
        candidates.push_back(modelAndName);

        MaterialFileCandidate nameOnlyCandidate;
        nameOnlyCandidate.path = folder / (name + c_MaterialsExtension);
        nameOnlyCandidate.sharedWithAllScenes = sharedWithAllScenes;
        nameOnlyCandidate.nameOnly = true;
        candidates.push_back(nameOnlyCandidate);
    };

    // Scene-local Materials/ (e.g. <scene-dir>/Materials/ when scene lives next to project assets).
    appendCandidates(m_sceneMaterialsSceneSpecializedPath, false);
    appendCandidates(m_sceneMaterialsPath, true);

    // Runtime Assets root (e.g. pip site-packages/rtxpt/Assets or ../Assets next to bin).
    appendCandidates(m_materialsSceneSpecializedPath, false);
    appendCandidates(m_materialsPath, true);

    Json::Value rootJ;
    std::string actualLoadedFileName;
    bool shared = false;
    for (const MaterialFileCandidate& candidate : candidates)
    {
        if (std::filesystem::exists(candidate.path) && caustica::json::loadFromFile(candidate.path, rootJ))
        {
            actualLoadedFileName = candidate.path.string();
            shared = candidate.sharedWithAllScenes;
            if (candidate.nameOnly)
                modelName = kNoModel;
            break;
        }
    }
    if (actualLoadedFileName=="")
    {
        caustica::warning("No material definition file found '%s' - consider doing Scene->Materials->Advanced->Save All", (modelName + "." + name + c_MaterialsExtension).c_str());
        return nullptr;
    }

    std::shared_ptr<StandardMaterial> standardMaterial = StandardMaterial::fromJson(rootJ, m_mediaPath, m_textureCache, modelName, name, m_sceneDirectory);
    if (standardMaterial == nullptr)
    {
        caustica::warning("Error while parsing material file '%s'", actualLoadedFileName.c_str()); 
        return nullptr;
    }
    standardMaterial->sharedWithAllScenes = shared; // this property is not loaded from the file, but determined based on where the file was loaded from
    return standardMaterial;
}

void MaterialGpuCache::sceneReloaded()
{
    clear();
}

void MaterialGpuCache::notifyMaterialEdited()
{
    ++m_materialStateRevision;
}

void MaterialGpuCache::rebuildActiveTextureIndex()
{
    m_textures.clear();
    for (const auto& material : m_materials)
    {
        recordTexture(material->baseTexture);
        recordTexture(material->occlusionRoughnessMetallicTexture);
        recordTexture(material->normalTexture);
        recordTexture(material->emissiveTexture);
        recordTexture(material->transmissionTexture);
    }
}

bool MaterialGpuCache::reconcileLiveMaterials(
    std::span<const caustica::scene::MaterialRenderResourceSnapshot> materials)
{
    std::unordered_set<caustica::scene::MaterialRenderResourceId,
        caustica::scene::MaterialRenderResourceId::Hash> liveIds;
    liveIds.reserve(materials.size());
    for (const auto& material : materials)
    {
        if (material.id)
            liveIds.insert(material.id);
    }

    const size_t oldCount = m_materialsById.size();
    std::erase_if(m_materialsById, [&liveIds](const auto& entry) {
        return !liveIds.contains(entry.first);
    });
    if (m_materialsById.size() == oldCount)
        return false;

    std::unordered_set<const StandardMaterial*> activeMaterials;
    activeMaterials.reserve(m_materialsById.size());
    for (const auto& entry : m_materialsById)
        activeMaterials.insert(entry.second.get());

    std::erase_if(m_materials, [&activeMaterials](const auto& material) {
        if (activeMaterials.contains(material.get()))
            return false;
        material->runtimeMaterialGpuCache = nullptr;
        return true;
    });

    m_materialsGPU.assign(m_materials.size(), StandardMaterialData{});
    for (uint32_t index = 0; index < m_materials.size(); ++index)
    {
        m_materials[index]->gpuDataIndex = index;
        m_materials[index]->gpuDataDirty = true;
    }
    m_uniqueNames.clear();
    for (const auto& material : m_materials)
        initializeUniqueDeterministicName(material);
    m_materialDataWasReset = true;
    rebuildActiveTextureIndex();
    notifyMaterialEdited();
    caustica::info("MaterialGpuCache: reclaimed %zu removed materials (remaining=%zu)",
        oldCount - m_materialsById.size(), m_materials.size());
    return true;
}

int MaterialGpuCache::ensureMaterialsFromScene(
    std::span<const caustica::scene::MaterialRenderResourceSnapshot> materials)
{
    reconcileLiveMaterials(materials);
    int added = 0;
    for (const auto& material : materials)
    {
        if (!material.id || m_materialsById.contains(material.id))
            continue;

        std::shared_ptr<StandardMaterial> standardMaterial;
        if (IsBuiltinModelFileName(material.modelFileName))
            standardMaterial = importFromEngineMaterial(material);
        else
        {
            standardMaterial = load(material.modelFileName, material.debugName);
            if (standardMaterial == nullptr)
                standardMaterial = importFromEngineMaterial(material);
        }

        standardMaterial->runtimeMaterialGpuCache = this;

        m_materials.push_back(standardMaterial);
        m_materialsById[material.id] = standardMaterial;
        m_materialsGPU.push_back(StandardMaterialData{});
        standardMaterial->gpuDataIndex = uint(m_materialsGPU.size() - 1);
        standardMaterial->gpuDataDirty = true;
        assert(m_materialsGPU.size() <= CAUSTICA_MATERIAL_MAX_COUNT);

        recordTexture(standardMaterial->baseTexture);
        recordTexture(standardMaterial->occlusionRoughnessMetallicTexture);
        recordTexture(standardMaterial->normalTexture);
        recordTexture(standardMaterial->emissiveTexture);
        recordTexture(standardMaterial->transmissionTexture);
        initializeUniqueDeterministicName(standardMaterial);
        ++added;
    }

    if (added > 0)
    {
        notifyMaterialEdited();
        m_materialDataWasReset = true;
        // Runtime import: do not rebuild the specialized permutation table.
        // Rebaking every material forces PathTracingShaderCompiler to reload/create
        // dozens of RT libraries + a huge hit-group PSO (looks like a hang after drag-drop).
        // New materials use the ubershader until the next full scene material bake.
        if (m_ubershader)
        {
            for (auto& standardMaterial : m_materials)
            {
                if (!standardMaterial->bakedShaderPermutation)
                    standardMaterial->bakedShaderPermutation = m_ubershader;
            }
        }
        else
        {
            bakeShaderPermutations();
        }
        caustica::info("MaterialGpuCache: ensured %d new runtime materials (total=%zu)", added, m_materials.size());
    }
    return added;
}

std::shared_ptr<StandardMaterial> MaterialGpuCache::findByResourceId(
    caustica::scene::MaterialRenderResourceId id) const
{
    const auto it = m_materialsById.find(id);
    return it == m_materialsById.end() ? nullptr : it->second;
}

std::shared_ptr<StandardMaterial> MaterialGpuCache::findByGpuDataIndex(uint gpuDataIndex) const
{
    if (gpuDataIndex == 0xFFFFFFFFu)
        return nullptr;
    for (const auto& material : m_materials)
    {
        if (material && material->gpuDataIndex == gpuDataIndex)
            return material;
    }
    return nullptr;
}

MaterialGpuCache::RayTracingState MaterialGpuCache::resolveRayTracingState(
    caustica::scene::MaterialRenderResourceId id) const
{
    const std::shared_ptr<StandardMaterial> material = findByResourceId(id);
    if (!material)
        return {};
    return {
        .skipRender = material->skipRender,
        .excludeFromNEE = material->excludeFromNEE,
        .alphaTest = material->enableAlphaTesting,
        .transmission = material->enableTransmission,
    };
}

void MaterialGpuCache::completeDeferredTexturesLoad(caustica::rhi::ICommandList* commandList)
{
    if (m_deferredTextureLoadInProgress)
    {
        info("MaterialGpuCache: deferred texture flush begin");
        if (commandList != nullptr)
        {
            commandList->close();
            m_device->executeCommandList(commandList);
        }

        // In case new textures were loaded, we need to make sure they were uploaded properly
        const bool texturesFinalized = m_textureCache->processRenderingThreadCommands(*m_renderDevice, 0.f);
        m_textureCache->loadingFinished();
        m_deferredTextureLoadInProgress = false;
        info("MaterialGpuCache: deferred texture flush end, ok=%s", texturesFinalized ? "true" : "false");

        // Bindless indices are only valid after finalize. First fillData() may have
        // cleared Use*Texture flags — dirty all materials so the next update re-encodes.
        for (auto& material : m_materials)
        {
            if (material)
                material->gpuDataDirty = true;
        }
        m_materialDataWasReset = true;

        if (commandList != nullptr)
        {
            commandList->open();
        }
    }
}

static std::string MacrosToString( const std::vector<caustica::ShaderMacro> & macros )
{
    std::string result;
    result.reserve(macros.size() * 16); // optional optimization guess
    bool first = true;
    for (const auto& p : macros) 
    {
        if (!first)
            result += ',';
        first = false;
        result += '{' + p.name + ',' + p.definition + '}';
    }
    return result;
}

static void InitializeStableShaderIdentity(MaterialShaderPermutation& msp)
{
    uint32_t tierIndex = 0;
    for (const auto& macro : msp.macros)
    {
        if (macro.name == "CAUSTICA_MATERIAL_FEATURE_TIER")
        {
            tierIndex = static_cast<uint32_t>(std::stoul(macro.definition));
            break;
        }
    }

    msp.featureTier = tierIndex;

    if (tierIndex == 0)
    {
        msp.stableShaderName = "Ubershader";
        msp.stableShaderId = -1;
        return;
    }

    msp.stableShaderName = caustica::render::materialFeatureTierStableName(tierIndex);
    msp.stableShaderId = int(tierIndex);
}

caustica::ShaderKey MaterialShaderPermutation::makeShaderKey(
    caustica::rhi::GraphicsAPI api,
    ShaderCompilerUtils::ShaderProfile profile) const
{
    return caustica::makeShaderLibraryKey(shaderFilePath, caustica::shader::fromRhiGraphicsApi(api), macros, profile);
}

// MaterialShaderPermutation::MaterialShaderPermutation(const std::string & shaderFilePath, const std::string & closestHitName, const std::string & anyHitName, const std::vector<std::pair<std::string, std::string>> & macros )


MaterialShaderPermutationKey::MaterialShaderPermutationKey( const MaterialShaderPermutation & msp )
    : fullKey(msp.shaderFilePath + ", " + caustica::formatCanonicalMacros(msp.macros))
{
    hash = ShortHash(HashMyString(fullKey));
}

void MaterialGpuCache::bakeShaderPermutations()
{
    MaterialShaderPermutation ubershader{
        .shaderFilePath = m_relativeShaderSourcePath,
        .macros = caustica::render::buildMaterialShaderMacros(0),
    };
    InitializeStableShaderIdentity(ubershader);
    m_ubershader = std::make_shared<MaterialShaderPermutation>(ubershader);
    m_ubershader->uniqueMaterialName = "Ubershader";

    // Map materials to offline-compilable feature tiers (multiple materials can share a tier).
    m_shaderPermutations.clear();
    m_shaderPermutationTable.clear();
    for (auto& standardMaterial : m_materials)
    {
        MaterialShaderPermutation variant = standardMaterial->computeShaderPermutation(m_relativeShaderSourcePath);
        InitializeStableShaderIdentity(variant);
        MaterialShaderPermutationKey key(variant);
        std::shared_ptr<MaterialShaderPermutation> ptVariant;
        auto findV = m_shaderPermutations.find(key);
        if (findV == m_shaderPermutations.end())
        {
            ptVariant = std::make_shared<MaterialShaderPermutation>(variant);
            m_shaderPermutations.insert(std::make_pair(key, ptVariant));
            ptVariant->indexInTable = (int)m_shaderPermutationTable.size();
            m_shaderPermutationTable.push_back(ptVariant);
        }
        else
        {
            ptVariant = findV->second;
            assert( ptVariant->macros == variant.macros );
        }

        std::string proposedName = standardMaterial->uniqueFullName();
        if (ptVariant->uniqueMaterialName == "" || (ptVariant->uniqueMaterialName.compare(proposedName)>0) )  // keep the "smallest" name out of all - still not deterministic between different scenes but goodenough!
            ptVariant->uniqueMaterialName = proposedName;

        standardMaterial->bakedShaderPermutation = ptVariant;
    }
}

void MaterialGpuCache::createRenderPassesAndLoadMaterials(caustica::rhi::IBindingLayout* bindlessLayout, caustica::render::RenderDevice& renderDevice, std::span<const caustica::scene::MaterialRenderResourceSnapshot> materials, const std::filesystem::path& sceneFilePath, const std::filesystem::path & mediaPath )
{
    info("MaterialGpuCache: createRenderPassesAndLoadMaterials begin");
    assert(!mediaPath.empty());
    //m_bindlessLayout = bindlessLayout;
    m_renderDevice = &renderDevice;

    static_assert(sizeof(StandardMaterialData) == 272,
        "StandardMaterialData size changed — update CAUSTICA_STANDARD_MATERIAL_DATA_BYTES in "
        "PtPipelineFeaturePresets.cpp, MaterialFeatureMask.cpp, and precompile_pt_shader_bins.py");

    {
        caustica::rhi::BufferDesc bufferDesc;
        bufferDesc.initialState = caustica::rhi::ResourceStates::ShaderResource;
        bufferDesc.keepInitialState = true;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.byteSize = sizeof(StandardMaterialData) * CAUSTICA_MATERIAL_MAX_COUNT;
        bufferDesc.structStride = sizeof(StandardMaterialData);
        bufferDesc.debugName = "StandardMaterialDataStorage";
        if (!m_materialData)
            m_materialData = m_device->createBuffer(bufferDesc);
        m_materialDataWasReset = true;
    }

    // Runtime imports can add materials after the first load. Keep existing PT
    // materials and only create missing ones (same path as ensureMaterialsFromScene).
    if (m_mediaPath != "")
        ensureMaterialsFromScene(materials);

    if (m_mediaPath == "") // first time load all
    {
        info("MaterialGpuCache: first material load begin");
        m_mediaPath = mediaPath;
        m_sceneDirectory = sceneFilePath.parent_path();
        if (!sceneFilePath.empty() && isInlineScenePath(sceneFilePath))
            m_sceneDirectory = std::filesystem::path();
        if (!m_sceneDirectory.empty())
        {
            m_sceneMaterialsPath = m_sceneDirectory / std::string(c_MaterialsSubFolder);
            std::filesystem::path justName = sceneFilePath.filename().stem();
            if (!justName.empty())
                m_sceneMaterialsSceneSpecializedPath = m_sceneMaterialsPath / justName;
        }
        m_materialsPath = mediaPath / std::string(c_MaterialsSubFolder);

        std::filesystem::path justName = sceneFilePath.filename().stem();

        m_materialsSceneSpecializedPath = m_materialsPath / justName;
  
        const int initializedFromEngineCount = ensureMaterialsFromScene(materials);

        // sort by name so when we're saving it's consistent
        std::sort(m_materials.begin(), m_materials.end(), [](const auto & a, const auto & b) { return a->name < b->name; } );

        (void)initializedFromEngineCount;

        m_deferredTextureLoadInProgress = true;
        info("MaterialGpuCache: first material load end, materials=%zu", m_materials.size());
    }

    completeDeferredTexturesLoad(nullptr);

    info("MaterialGpuCache: record material textures begin");
    m_uniqueNames.clear(); // must be done before initializeUniqueDeterministicName
    for (auto& standardMaterial : m_materials)
    {
        recordTexture(standardMaterial->baseTexture);
        recordTexture(standardMaterial->occlusionRoughnessMetallicTexture);
        recordTexture(standardMaterial->normalTexture);
        recordTexture(standardMaterial->emissiveTexture);
        recordTexture(standardMaterial->transmissionTexture);

        initializeUniqueDeterministicName(standardMaterial);
    }
    info("MaterialGpuCache: record material textures end, uniqueTextures=%zu", m_textures.size());
    info("MaterialGpuCache: bake shader permutations begin");
    bakeShaderPermutations();
    info("MaterialGpuCache: bake shader permutations end");
    info("MaterialGpuCache: createRenderPassesAndLoadMaterials end");
}

// NOTE: this also handles some of the geometry data and mixed geometry&material stuff - it might be a good idea to rethink whether it needs to live outside of material baker
void UpdateSubInstanceData(SubInstanceData& ret,
    const caustica::render::SceneGpuResources* gpuResources,
    const caustica::scene::MeshRenderResourceSnapshot& mesh,
    const caustica::scene::GeometryRenderResourceSnapshot& geometry,
    uint meshGeometryIndex,
    const StandardMaterial& material)
{
    if (mesh.geometries.empty() || meshGeometryIndex >= mesh.geometries.size())
        return;

    bool alphaTest = material.hasAlphaTest();

    // we need alpha texture for alpha testing to work - disable otherwise
    if (alphaTest && (!material.enableBaseTexture || material.baseTexture.loaded == nullptr))
        alphaTest = false;
    if (alphaTest && (material.baseTexture.loaded->gpu.texture == nullptr
        || material.baseTexture.loaded->gpu.bindlessDescriptor.Get() == ~0u))
    {
        alphaTest = false;
    }


    float alphaCutoff = 0.0;

    ret.FlagsAndAlphaInfo = 0;
    if (alphaTest)
    {
        ret.FlagsAndAlphaInfo |= SubInstanceData::Flags_AlphaTested;

        assert(material.enableBaseTexture && material.baseTexture.loaded != nullptr); // disable alpha testing if this happens to be possible
        // ret.alphaCutoff = material.alphaCutoff;
        alphaCutoff = material.alphaCutoff;

        uint alphaTextureIndex = material.baseTexture.loaded->gpu.bindlessDescriptor.Get();
        assert(alphaTextureIndex < 0xFFFF);
        // ret.AlphaTextureIndex = material.baseTexture.loaded->gpu.bindlessDescriptor.Get();

        ret.FlagsAndAlphaInfo |= alphaTextureIndex & 0xFFFF;
    }
    ret.EmissiveLightMappingOffset = 0xFFFFFFFF;

    uint quantizedAlphaCutoff = (uint)(dm::clamp(alphaCutoff, 0.0f, 1.0f) * 255.0f + 0.5f); assert(quantizedAlphaCutoff < 256);
    ret.FlagsAndAlphaInfo |= (quantizedAlphaCutoff << SubInstanceData::Flags_AlphaOffsetOffset);

    if (material.excludeFromNEE)
        ret.FlagsAndAlphaInfo |= SubInstanceData::Flags_ExcludeFromNEE;

    uint globalGeometryIndex = mesh.geometries[0].globalGeometryIndex + meshGeometryIndex;
    uint globalMaterialIndex = material.gpuDataIndex;
    if (globalMaterialIndex == 0xFFFFFFFFu || globalMaterialIndex > 0xFFFFu)
    {
        caustica::warning(
            "MaterialGpuCache: material '%s' has invalid gpuDataIndex %u — binding slot 0",
            material.name.c_str(),
            globalMaterialIndex);
        globalMaterialIndex = 0;
    }
    ret.GlobalGeometryIndex_StandardMaterialDataIndex = (globalGeometryIndex << 16) | (globalMaterialIndex & 0xFFFFu);

#if SUBINSTANCEDATA_EXTENDED
    GeometryData* gdata = nullptr;
    if (gpuResources && uint(geometry.globalGeometryIndex) < gpuResources->geometryData.size())
        gdata = const_cast<GeometryData*>(&gpuResources->geometryData[geometry.globalGeometryIndex]);
    if (gdata != nullptr)
    {
        GeometryData& geometryData = *gdata;
        assert(geometryData.indexBufferIndex < 0xFFFF);
        assert(geometryData.vertexBufferIndex < 0xFFFF);
        ret.IndexBufferIndex_VertexBufferIndex = (geometryData.indexBufferIndex << 16) | geometryData.vertexBufferIndex;
        ret.IndexOffset = geometryData.indexOffset;
        ret.TexCoord1Offset = geometryData.texCoord1Offset;
    }
    else
    {
        assert(false);
    }
#else
    (void)gpuResources;
    (void)geometry;
#endif

}

void MaterialGpuCache::update(caustica::rhi::ICommandList* commandList,
    const caustica::scene::SceneRenderData& renderData,
    const caustica::render::SceneGpuResources* gpuResources,
    std::vector<SubInstanceData>& subInstanceData)
{
    RAII_SCOPE( commandList->beginMarker("MaterialGpuCache");, commandList->endMarker(); );

    ensureMaterialsFromScene(renderData.materialSnapshots);
    completeDeferredTexturesLoad(commandList);

    for (const auto& snapshot : renderData.materialSnapshots)
    {
        const std::shared_ptr<StandardMaterial> standardMaterial = findByResourceId(snapshot.id);
        if (standardMaterial && standardMaterial->useEngineEmissiveIntensity
            && standardMaterial->emissiveIntensity != snapshot.emissiveIntensity)
        {
            standardMaterial->emissiveIntensity = snapshot.emissiveIntensity;
            standardMaterial->gpuDataDirty = true;
        }
    }

    bool needsUpload = false;
    for (auto& standardMaterial : m_materials)
    {
        if (!standardMaterial->gpuDataDirty && !m_materialDataWasReset)
            continue;

        standardMaterial->fillData(m_materialsGPU[standardMaterial->gpuDataIndex]);
        standardMaterial->gpuDataDirty = false;
        needsUpload = true;
    }

    if ( needsUpload )
    {
        commandList->writeBuffer( m_materialData, m_materialsGPU.data(), m_materialsGPU.size() * sizeof(StandardMaterialData), 0 );
        m_materialDataWasReset = false;
    }

    // NOTE: this also handles some of the geometry data and mixed geometry&material stuff - it might be a good idea to rethink whether it needs to live outside of material baker.
    // Walk the render snapshot in the same order as TLAS / hit-group setup and write a dense
    // prefix of SubInstanceData slots. Do not trust live geometryInstanceIndex alone ??after
    // runtime import it can briefly disagree with the snapshot order TLAS already committed.
    size_t compactedGeometryInstanceIndex = 0;
    for (const scene::MeshInstanceRenderProxy& proxy : renderData.meshInstances)
    {
        const auto* mesh = renderData.findMesh(proxy.meshId);
        if (!mesh)
            continue;

        const size_t firstSubInstanceIndex = compactedGeometryInstanceIndex;
        compactedGeometryInstanceIndex += mesh->geometries.size();
        for (size_t geometryIndex = 0; geometryIndex < mesh->geometries.size(); ++geometryIndex)
        {
            const auto& geometry = mesh->geometries[geometryIndex];
            const size_t subInstanceIndex = firstSubInstanceIndex + geometryIndex;
            if (subInstanceIndex >= subInstanceData.size())
            {
                assert(false && "Sub-instance data is out of sync with scene geometry instances");
                continue;
            }

            std::shared_ptr<StandardMaterial> standardMaterial = findByResourceId(geometry.materialId);
            assert(standardMaterial != nullptr && "Unknown error - should never have happened" );
            if (!standardMaterial)
            {
                caustica::warning(
                    "MaterialGpuCache: no StandardMaterial for geometry materialId on mesh '%s' (subInstance %zu) — leaving stale SubInstanceData",
                    mesh->debugName.c_str(),
                    subInstanceIndex);
                continue;
            }

            UpdateSubInstanceData(subInstanceData[subInstanceIndex], gpuResources, *mesh, geometry,
                static_cast<uint>(geometryIndex), *standardMaterial);
        }
    }
}

/*
void MaterialGpuCache::loadAll(std::unordered_map<std::string, std::shared_ptr<StandardMaterial>>& container)
{
    std::ifstream inFile(m_sceneMaterialsFilePath);

    if (!inFile.is_open())
        { caustica::warning("No material definition file found at '%s' - consider doing Scene->Materials->Advanced->Save", m_sceneMaterialsFilePath.string().c_str()); return; }

    Json::Value rootJ;
    inFile >> rootJ;

    int version = -1;
    rootJ["RTXPTMaterials"]["version"] >> version;
    if (version != 1)
        { caustica::warning("Malformed or unsupported material definition file version '%s' - consider doing Scene->Materials->Advanced->Save", m_sceneMaterialsFilePath.string().c_str()); return; }

    Json::Value materialsJ;

    materialsJ = rootJ["materials"];
    if (materialsJ.empty() || !materialsJ.isArray())
        { caustica::warning("Malformed or empty material definition file '%s' - consider doing Scene->Materials->Advanced->Save", m_sceneMaterialsFilePath.string().c_str()); return; }

    for ( Json::Value materialJ : materialsJ )
    {
        std::shared_ptr<StandardMaterial> standardMaterial = StandardMaterial::fromJson(materialJ, m_mediaPath, m_textureCache);
        if (standardMaterial == nullptr)
            { caustica::warning("Error while reading material in material definition file '%s'", m_sceneMaterialsFilePath.string().c_str()); continue; }
        
        auto existing = container.find(standardMaterial->name);
        if (existing != container.end())
            { caustica::warning("Duplicated materials with name '%s' found in material definition file '%s' - subsequent instances ignored.", standardMaterial->name.c_str(), m_sceneMaterialsFilePath.string().c_str()); assert( false ); continue; }
        else
            container.insert( make_pair(standardMaterial->name, standardMaterial) );
    }
}*/

bool MaterialGpuCache::loadSingle(StandardMaterialBase & material)
{
    std::filesystem::path inPath = getMaterialStoragePath(material);

    Json::Value rootJ;
    if ( !caustica::json::loadFromFile(inPath, rootJ) )
    {
        caustica::warning("No material definition file found '%s' - consider doing Scene->Materials->Advanced->Save All", inPath.string().c_str());
        return false;
    }
    assert( material.name != "" );
    m_deferredTextureLoadInProgress = true;
    return material.read(rootJ, m_mediaPath, m_textureCache, m_sceneDirectory);
}

bool MaterialGpuCache::saveSingle(StandardMaterialBase & material)
{
    if (!ensureDirectoryExists(m_materialsPath))
        return false;
    std::filesystem::path outPath = m_materialsPath;
    if (!material.sharedWithAllScenes)
        outPath = m_materialsSceneSpecializedPath;
    if (!ensureDirectoryExists(outPath))

    assert( material.modelName != "" && material.name != "" );
    outPath = getMaterialStoragePath(material);

    Json::Value rootJ;
    //rootJ["RTXPTMaterialVersion"] = 1;

    material.write(rootJ);

    // TODO: refactor, replace with caustica::json::saveToFile
 
    std::ofstream outFile(outPath, std::ios::trunc);
    if (!outFile.is_open())
    {
        caustica::error("Error attempting to save material to file '%s'", outPath.c_str());
        return false;
    }

    Json::StreamWriterBuilder builder;
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());

    writer->write(rootJ, &outFile);
    outFile.close();
    return true;
}

void MaterialGpuCache::saveAll()
{
#if 0
    Json::Value rootJ;

    rootJ["RTXPTMaterials"]["version"] = 1;
    Json::Value materialsJ;
    for (auto& standardMaterial : m_materials)
    {
        Json::Value materialJ;
        standardMaterial->write(materialJ, m_mediaPath);
        materialsJ.append(materialJ);
    }

    rootJ["materials"] = materialsJ;

    std::ofstream outFile(m_sceneMaterialsFilePath, std::ios::trunc);

    Json::StreamWriterBuilder builder;
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    
    writer->write(rootJ, &outFile);
    outFile.close();
#else
    for (auto& standardMaterial : m_materials)
        saveSingle(*standardMaterial);
#endif
}

bool MaterialGpuCache::debugGui(float indent)
{
    RAII_SCOPE(ImGui::PushID("MaterialGpuCacheDebugGUI"); , ImGui::PopID(); );
    
    bool resetAccumulation = false;
    #define IMAGE_QUALITY_OPTION(code) do{if (code) resetAccumulation = true;} while(false)

    ImGui::Text("Scene material count: %d", (int)m_materials.size());
    ImGui::Text("Material texture use count: %d", (int)m_textures.size());
    ImGui::Text("Material shader count: %d", (int)m_shaderPermutationTable.size());

    // ImGui::Separator();
    // if (ImGui::CollapsingHeader("Debugging", ImGuiTreeNodeFlags_DefaultOpen))
    // {
    //     RAII_SCOPE(ImGui::Indent(indent); , ImGui::Unindent(indent););
    // 
    //     ImGui::Text("<shrug>");
    // }
    // ImGui::Separator();

    if (ImGui::CollapsingHeader("Advanced", 0/*ImGuiTreeNodeFlags_DefaultOpen*/))
    {
        ImGui::Text("Materials storage directory:");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0, 1.0, 0.5, 1.0));
        ImGui::TextWrapped("%s", m_materialsPath.string().c_str());
        ImGui::PopStyleColor();

        ImGui::Text("Set all sharedWithAllScenes props to");
        {
            RAII_SCOPE(ImGui::Indent();, ImGui::Unindent(););
            if (ImGui::Button("Shared"))
                for (auto& standardMaterial : m_materials)
                    standardMaterial->sharedWithAllScenes = true;
            ImGui::SameLine();
            if (ImGui::Button("Not Shared"))
                for (auto& standardMaterial : m_materials)
                    standardMaterial->sharedWithAllScenes = false;
        }

        if( ImGui::Button("Save all") )
            saveAll();
    }

    return resetAccumulation;
}

MaterialShaderPermutation StandardMaterial::computeShaderPermutation(const std::string& defaultShaderPath)
{
    const caustica::render::materialFeatureMask featureMask = caustica::render::computeMaterialFeatureMask(*this);
    const uint32_t tierIndex = caustica::render::mapFeatureMaskToTier(featureMask);
    return MaterialShaderPermutation{
        .shaderFilePath = defaultShaderPath,
        .macros = caustica::render::buildMaterialShaderMacros(tierIndex),
    };
}

std::shared_ptr<StandardMaterial> MaterialGpuCache::findByUniqueId(const std::string & name)
{
    for( int i = 0; i < m_materials.size(); i++)
        if (equalsIgnoreCase(name, m_materials[i]->uniqueFullName()))
            return m_materials[i];

    return nullptr;
}
