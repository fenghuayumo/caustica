#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/lighting/MaterialFeatureMask.h>

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
#include <scene/SceneEcs.h>

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
    return normalized == "openpbr" || normalized == "openpbr-lite" || normalized == "openpbr_lite";
}

static bool HasOpenPBRLiteFields(const Json::Value& input)
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
            || input.isMember("fuzz_weight"));
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
    std::filesystem::path fullPath = ResolveSceneMediaPath(localPath, sceneDirectory, mediaPath);

    constexpr bool cSearchForDDS = true;
    const std::string extension = LowerCopy(fullPath.extension().string());
    if (cSearchForDDS && extension == ".png")
    {
        std::filesystem::path ddsLocalPath = localPathForStorage;
        ddsLocalPath.replace_extension(".dds");
        std::filesystem::path ddsFullPath = ResolveSceneMediaPath(ddsLocalPath, sceneDirectory, mediaPath);

        if (std::filesystem::exists(ddsFullPath))
        {
            localPathForStorage = ddsLocalPath;
            fullPath = ddsFullPath;
        }
    }

    return fullPath;
}

void PTTexture::initFromLoadedTexture(caustica::Handle<caustica::ImageAsset> & loaded, bool _sRGB, bool _normalMap, const std::filesystem::path & mediaPath)
{
    if (loaded == nullptr)
    { localPath = ""; sRGB = false; this->loaded = nullptr; normalMap = false; enabled = false; return; }

    localPath = std::filesystem::relative(loaded->path, mediaPath);
    sRGB = _sRGB;
    this->loaded = loaded;
    normalMap = _normalMap;
    enabled = true;
}

std::shared_ptr<PTMaterial> PTMaterial::safeCast(const std::shared_ptr<Material>& bridgeMaterial)
{
    if (bridgeMaterial == nullptr)
        return nullptr;

    const std::shared_ptr<MaterialEx> materialEx = std::dynamic_pointer_cast<MaterialEx>(bridgeMaterial);
    assert(materialEx != nullptr);
    return materialEx ? materialEx->ptData : nullptr;
}

void PTMaterial::write(Json::Value& output)
{
    auto saveTexture = [ ](Json::Value& output, const PTTexture & texture, const std::string& name)
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
    }
}

bool PTMaterial::read(
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
    const bool hasTopLevelOpenPBRFields = HasOpenPBRLiteFields(input);

    auto loadTexture = [&](const char* camelName, const char* pascalName, PTTexture& output)
    {
        output = PTTexture();
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

    LOAD_FIELD_EITHER(enableAsAnalyticLightProxy, "EnableAsAnalyticLightProxy");

    LOAD_FIELD_EITHER(ignoreMeshTangentSpace, "IgnoreMeshTangentSpace");

    if (input.isMember("useEngineEmissiveIntensity"))
        input["useEngineEmissiveIntensity"] >> useEngineEmissiveIntensity;
    else if (input.isMember("UseEngineEmissiveIntensity"))
        input["UseEngineEmissiveIntensity"] >> useEngineEmissiveIntensity;
    else if (input.isMember("UseDonutEmissiveIntensity"))
        input["UseDonutEmissiveIntensity"] >> useEngineEmissiveIntensity;

    LOAD_FIELD_EITHER(skipRender, "SkipRender");

    auto readOpenPBRLite = [this](const Json::Value& openPBR)
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

        useSpecularGlossModel = false;
    };

    if (hasOpenPBRBlock)
        readOpenPBRLite(input["OpenPBR"]);
    else if ((hasMaterialModelField && IsOpenPBRMaterialModel(materialModel)) || hasTopLevelOpenPBRFields)
        readOpenPBRLite(input);
    else if (!hasMaterialModelField && !useSpecularGlossModel)
    {
        materialModel = "OpenPBR";
        specularColor = dm::float3(1.f);
    }
    else if (!hasMaterialModelField && useSpecularGlossModel)
        materialModel = "RTXPT";

    baseWeight = std::clamp(baseWeight, 0.0f, 1.0f);
    specularWeight = std::max(specularWeight, 0.0f);
    anisotropy = std::clamp(anisotropy, -1.0f, 1.0f);
    fuzzWeight = std::clamp(fuzzWeight, 0.0f, 1.0f);
    fuzzRoughness = std::clamp(fuzzRoughness, 0.0f, 1.0f);

    return true;
}

std::shared_ptr<PTMaterial> PTMaterial::fromJson(
    Json::Value& input,
    const std::filesystem::path& mediaPath,
    const std::shared_ptr<caustica::TextureLoader>& textureCache,
    const std::string& modelName,
    const std::string& name,
    const std::filesystem::path& sceneDirectory)
{
    std::shared_ptr<PTMaterial> material = std::make_shared<PTMaterial>();

    material->read(input, mediaPath, textureCache, sceneDirectory);
    material->name = name;
    material->modelName = modelName;

    return material;
}

PTTexture& PTMaterial::getTexture(PTMaterialTextureSlot slot)
{
    switch (slot)
    {
    case PTMaterialTextureSlot::Base:
        return baseTexture;
    case PTMaterialTextureSlot::OcclusionRoughnessMetallic:
        return occlusionRoughnessMetallicTexture;
    case PTMaterialTextureSlot::Normal:
        return normalTexture;
    case PTMaterialTextureSlot::Emissive:
        return emissiveTexture;
    case PTMaterialTextureSlot::Transmission:
        return transmissionTexture;
    default:
        assert(false);
        return baseTexture;
    }
}

const PTTexture& PTMaterial::getTexture(PTMaterialTextureSlot slot) const
{
    return const_cast<PTMaterial*>(this)->getTexture(slot);
}

bool PTMaterial::isTextureEnabled(PTMaterialTextureSlot slot) const
{
    switch (slot)
    {
    case PTMaterialTextureSlot::Base:
        return enableBaseTexture;
    case PTMaterialTextureSlot::OcclusionRoughnessMetallic:
        return enableOcclusionRoughnessMetallicTexture;
    case PTMaterialTextureSlot::Normal:
        return enableNormalTexture;
    case PTMaterialTextureSlot::Emissive:
        return enableEmissiveTexture;
    case PTMaterialTextureSlot::Transmission:
        return enableTransmissionTexture;
    default:
        assert(false);
        return false;
    }
}

void PTMaterial::setTextureEnabled(PTMaterialTextureSlot slot, bool enabled)
{
    switch (slot)
    {
    case PTMaterialTextureSlot::Base:
        enableBaseTexture = enabled;
        break;
    case PTMaterialTextureSlot::OcclusionRoughnessMetallic:
        enableOcclusionRoughnessMetallicTexture = enabled;
        break;
    case PTMaterialTextureSlot::Normal:
        enableNormalTexture = enabled;
        break;
    case PTMaterialTextureSlot::Emissive:
        enableEmissiveTexture = enabled;
        break;
    case PTMaterialTextureSlot::Transmission:
        enableTransmissionTexture = enabled;
        break;
    default:
        assert(false);
        break;
    }
}

bool PTMaterial::editorGui(MaterialGpuCache & cache)
{
    bool update = false;

    float itemWidth = ImGui::CalcItemWidth();

    auto getShortTexturePath = [ ](const PTTexture & texture) -> std::string
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

    auto drawTextureToggle = [&](const char* label, PTTexture& texture, bool& enabled)
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

    update |= ImGui::SliderFloat("geometry_opacity", &opacity, 0.f, 1.f);
    update |= ImGui::Checkbox("geometry_thin_walled", &thinSurface);

    drawTextureToggle("Use transmission_weight texture", transmissionTexture, enableTransmissionTexture);

    float previousTransmissionFactor = transmissionFactor;
    float previousDiffuseTransmissionFactor = diffuseTransmissionFactor;
    update |= ImGui::SliderFloat("transmission_weight", &transmissionFactor, 0.f, 1.f);
    update |= ImGui::SliderFloat("transmission_diffuse_weight", &diffuseTransmissionFactor, 0.f, 1.f);

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
            int dominantDeltaLobeP1 = dm::clamp(psdDominantDeltaLobe, -1, 1) + 1;
            update |= ImGui::Combo("Dominant bounce", &dominantDeltaLobeP1, "None (surface)\0Transparency\0Reflection\0\0");
            psdDominantDeltaLobe = dm::clamp(dominantDeltaLobeP1 - 1, -1, 1);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Determines which surface will:\n * provide motion vectors for denoising\n * get ReSTIR DI lighting\n * get 'boost samples' for NEE lighting");
        }
        ImGui::Unindent();
    }

    if (ImGui::CollapsingHeader("Save/Load"))
    {
        RAII_SCOPE( ImGui::Indent();, ImGui::Unindent(); );

        ImGui::Checkbox("Share with all scenes", &sharedWithAllScenes);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("if checked, material saved to /Assets/Materials/ path and \nshared between all scenes; otherwise it will be saved to \n/Assets/Materials/SceneName specific to current scene");

        auto matPath = cache.getMaterialStoragePath(*this);

        ImGui::TextWrapped("File name: %s", matPath.string().c_str());

        if (ImGui::Button("Load"))
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

bool PTMaterial::isEmissive() const
{
    return (emissiveIntensity > 0) && (caustica::math::any(emissiveColor>0.0f)) || useEngineEmissiveIntensity;  // useEngineEmissiveIntensity can animate on/off so just assume we're emissive and pay the cost
}

void PTMaterial::fillData(PTMaterialData & data)
{
    // flags

    data.Flags = 0;

    if (useSpecularGlossModel)
        data.Flags |= PTMaterialFlags_UseSpecularGlossModel;

    if (baseTexture.loaded && enableBaseTexture)
        data.Flags |= PTMaterialFlags_UseBaseOrDiffuseTexture;

    if (occlusionRoughnessMetallicTexture.loaded && enableOcclusionRoughnessMetallicTexture)
        data.Flags |= PTMaterialFlags_UseMetalRoughOrSpecularTexture;

    if (emissiveTexture.loaded && enableEmissiveTexture)
        data.Flags |= PTMaterialFlags_UseEmissiveTexture;

    if (normalTexture.loaded && enableNormalTexture)
        data.Flags |= PTMaterialFlags_UseNormalTexture;

    if (transmissionTexture.loaded && enableTransmissionTexture && enableTransmission)
        data.Flags |= PTMaterialFlags_UseTransmissionTexture;

    if (metalnessInRedChannel)
        data.Flags |= PTMaterialFlags_MetalnessInRedChannel;

    if (thinSurface || !enableTransmission) // materials with no transmission are automatically considered thin surface - simplifies a lot of things
        data.Flags |= PTMaterialFlags_ThinSurface;

    if (psdExclude)
        data.Flags |= PTMaterialFlags_PSDExclude;

    if (psdBlockMotionVectorsAtSurfaceType % 2)
        data.Flags |= PTMaterialFlags_PSDBlockMVsAtSurfaceTypeB0;

    if (psdBlockMotionVectorsAtSurfaceType / 2)
        data.Flags |= PTMaterialFlags_PSDBlockMVsAtSurfaceTypeB1;

    if (enableAsAnalyticLightProxy)
        data.Flags |= PTMaterialFlags_EnableAsAnalyticLightProxy;

    if (ignoreMeshTangentSpace)
        data.Flags |= PTMaterialFlags_IgnoreMeshTangentSpace;

    if (IsOpenPBRMaterialModel(materialModel))
        data.Flags |= PTMaterialFlags_UseOpenPBRMaterialModel;

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

    // bindless textures

    GetBindlessTextureIndex(baseTexture.loaded, data.BaseOrDiffuseTextureIndex, data.Flags, PTMaterialFlags_UseBaseOrDiffuseTexture);
    GetBindlessTextureIndex(occlusionRoughnessMetallicTexture.loaded, data.MetalRoughOrSpecularTextureIndex, data.Flags, PTMaterialFlags_UseMetalRoughOrSpecularTexture);
    GetBindlessTextureIndex(emissiveTexture.loaded, data.EmissiveTextureIndex, data.Flags, PTMaterialFlags_UseEmissiveTexture);
    GetBindlessTextureIndex(normalTexture.loaded, data.NormalTextureIndex, data.Flags, PTMaterialFlags_UseNormalTexture);
    GetBindlessTextureIndex(transmissionTexture.loaded, data.TransmissionTextureIndex, data.Flags, PTMaterialFlags_UseTransmissionTexture);

    data.Flags |= (uint)(min(nestedPriority, kMaterialMaxNestedPriority)) << PTMaterialFlags_NestedPriorityShift;
    data.Flags |= (uint)(clamp(psdDominantDeltaLobe + 1, 0, 7)) << PTMaterialFlags_PSDDominantDeltaLobeP1Shift;

    data.ShadowNoLFadeout = std::clamp(shadowNoLFadeout, 0.0f, 0.25f);

    data._padding0 = data._padding1 = 42;
}

std::filesystem::path MaterialGpuCache::getMaterialStoragePath(PTMaterialBase& material)
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

MaterialGpuCache::MaterialGpuCache(const std::string & relativeShaderSourcePath, nvrhi::IDevice* device, std::shared_ptr<caustica::TextureLoader> textureCache, std::shared_ptr<caustica::ShaderFactory> shaderFactory)
    : m_relativeShaderSourcePath(relativeShaderSourcePath)
    , m_device(device)
    , m_textureCache(textureCache)
    , m_bindingCache(device)
    , m_shaderFactory(shaderFactory)
{
}

static bool DefaultTextureSRGB(const PTMaterial& material, PTMaterialTextureSlot slot)
{
    switch (slot)
    {
    case PTMaterialTextureSlot::Base:
    case PTMaterialTextureSlot::Emissive:
        return true;
    case PTMaterialTextureSlot::OcclusionRoughnessMetallic:
        return material.useSpecularGlossModel;
    case PTMaterialTextureSlot::Normal:
    case PTMaterialTextureSlot::Transmission:
        return false;
    default:
        assert(false);
        return false;
    }
}

static bool DefaultTextureNormalMap(PTMaterialTextureSlot slot)
{
    return slot == PTMaterialTextureSlot::Normal;
}

static void UpdateEngineMaterialTexture(
    PTMaterial& material,
    PTMaterialTextureSlot slot,
    const Handle<ImageAsset>& loaded,
    bool enabled)
{
    if (material.engineMaterialCounterpart == nullptr)
        return;

    switch (slot)
    {
    case PTMaterialTextureSlot::Base:
        material.engineMaterialCounterpart->baseOrDiffuseTexture = loaded;
        material.engineMaterialCounterpart->enableBaseOrDiffuseTexture = enabled;
        break;
    case PTMaterialTextureSlot::OcclusionRoughnessMetallic:
        material.engineMaterialCounterpart->metalRoughOrSpecularTexture = loaded;
        material.engineMaterialCounterpart->enableMetalRoughOrSpecularTexture = enabled;
        break;
    case PTMaterialTextureSlot::Normal:
        material.engineMaterialCounterpart->normalTexture = loaded;
        material.engineMaterialCounterpart->enableNormalTexture = enabled;
        break;
    case PTMaterialTextureSlot::Emissive:
        material.engineMaterialCounterpart->emissiveTexture = loaded;
        material.engineMaterialCounterpart->enableEmissiveTexture = enabled;
        break;
    case PTMaterialTextureSlot::Transmission:
        material.engineMaterialCounterpart->transmissionTexture = loaded;
        material.engineMaterialCounterpart->enableTransmissionTexture = enabled;
        break;
    default:
        assert(false);
        break;
    }
}

void MaterialGpuCache::recordTexture(const PTTexture& texture)
{
    if (texture.loaded == nullptr)
        return;

    assert(texture.localPath != "");

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
    PTMaterial& material,
    PTMaterialTextureSlot slot,
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

    PTTexture& texture = material.getTexture(slot);
    texture.localPath = storagePath;
    texture.sRGB = sRGB.value_or(DefaultTextureSRGB(material, slot));
    texture.normalMap = normalMap.value_or(DefaultTextureNormalMap(slot));
    texture.loaded = m_textureCache->loadTextureFromFileDeferred(fullPath, texture.sRGB);
    texture.enabled = texture.loaded != nullptr;

    if (texture.loaded == nullptr ||
        (!m_textureCache->isTextureLoaded(texture.loaded) && !m_textureCache->isTextureFinalized(texture.loaded)))
    {
        texture = PTTexture();
        texture.enabled = false;
        return false;
    }

    material.setTextureEnabled(slot, true);
    UpdateEngineMaterialTexture(material, slot, texture.loaded, true);
    material.gpuDataDirty = true;

    m_deferredTextureLoadInProgress = true;
    recordTexture(texture);
    return true;
}

void MaterialGpuCache::clearMaterialTexture(PTMaterial& material, PTMaterialTextureSlot slot)
{
    PTTexture& texture = material.getTexture(slot);
    texture = PTTexture();
    texture.enabled = false;

    material.setTextureEnabled(slot, false);
    UpdateEngineMaterialTexture(material, slot, nullptr, false);
    material.gpuDataDirty = true;
}

void MaterialGpuCache::initializeUniqueDeterministicName(const std::shared_ptr<PTMaterialBase> & material)
{
    std::string hashBase = material->modelName + "_" + material->name;
    uint evenShorterHash = ShortHash(HashMyString(hashBase));
    
    std::string uniqueName = StripNonAsciiAlnum(material->name).substr(0, 16) + "_" + HexString(evenShorterHash);

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
    m_materialsGPU.clear();
    m_textures.clear();
    m_mediaPath = std::filesystem::path();
    m_sceneDirectory = std::filesystem::path();
    m_sceneMaterialsPath = std::filesystem::path();
    m_sceneMaterialsSceneSpecializedPath = std::filesystem::path();
    m_materialsPath = std::filesystem::path();
    m_materialsSceneSpecializedPath = std::filesystem::path();
    m_uniqueNames.clear();
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

std::shared_ptr<PTMaterial> MaterialGpuCache::importFromEngineMaterial(caustica::Material& material)
{
    std::shared_ptr<PTMaterial> materialPT = std::make_shared<PTMaterial>();

    materialPT->name = material.name;
    materialPT->modelName = ModelNameFromModelFileName(material.modelFileName);

    materialPT->baseTexture.initFromLoadedTexture(material.baseOrDiffuseTexture, true, false, m_mediaPath);

    if( material.useSpecularGlossModel ) // spec-gloss model is a special case hack where we use metalRoughOrSpecularTexture to store specular color, which is handled as sRGB
        materialPT->occlusionRoughnessMetallicTexture.initFromLoadedTexture(material.metalRoughOrSpecularTexture, true, false, m_mediaPath);
    else
        materialPT->occlusionRoughnessMetallicTexture.initFromLoadedTexture(material.metalRoughOrSpecularTexture, false, false, m_mediaPath);

    materialPT->normalTexture.initFromLoadedTexture(material.normalTexture, false, true, m_mediaPath);
    materialPT->emissiveTexture.initFromLoadedTexture(material.emissiveTexture, true, false, m_mediaPath);
    materialPT->transmissionTexture.initFromLoadedTexture(material.transmissionTexture, false, false, m_mediaPath);

    // Toggles for the textures. Only effective if the corresponding texture is non-null.
    materialPT->enableBaseTexture = material.enableBaseOrDiffuseTexture;
    materialPT->enableOcclusionRoughnessMetallicTexture = material.enableMetalRoughOrSpecularTexture;
    materialPT->enableNormalTexture = material.enableNormalTexture;
    materialPT->enableEmissiveTexture = material.enableEmissiveTexture;
    materialPT->enableTransmissionTexture = material.enableTransmissionTexture;

    materialPT->baseOrDiffuseColor = material.baseOrDiffuseColor;
    materialPT->specularColor = material.specularColor;
    materialPT->emissiveColor = material.emissiveColor;

    materialPT->emissiveIntensity = material.emissiveIntensity;
    materialPT->metalness = material.metalness;
    materialPT->roughness = material.roughness;
    materialPT->opacity = material.opacity;
    materialPT->alphaCutoff = material.alphaCutoff;
    materialPT->transmissionFactor = material.transmissionFactor;
    //materialPT->diffuseTransmissionFactor = material.diffuseTransmissionFactor;
    materialPT->normalTextureScale = material.normalTextureScale;
    //materialPT->IoR = material.ior;
    materialPT->useSpecularGlossModel = material.useSpecularGlossModel;
    materialPT->metalnessInRedChannel = material.metalnessInRedChannel;

    materialPT->enableAlphaTesting = (material.domain == MaterialDomain::AlphaTested || material.domain == MaterialDomain::TransmissiveAlphaTested);
    materialPT->enableTransmission = (material.domain == MaterialDomain::Transmissive || material.domain == MaterialDomain::TransmissiveAlphaBlended || material.domain == MaterialDomain::TransmissiveAlphaTested);

    return materialPT;
}

std::shared_ptr<PTMaterial> MaterialGpuCache::load(const std::string & modelFileName, const std::string & name)
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
        if (std::filesystem::exists(candidate.path) && caustica::json::LoadFromFile(candidate.path, rootJ))
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
        caustica::warning("No RTXPT material definition file found '%s' - consider doing Scene->Materials->Advanced->Save All", (modelName + "." + name + c_MaterialsExtension).c_str()); 
        return nullptr;
    }

    std::shared_ptr<PTMaterial> materialPT = materialPT->fromJson(rootJ, m_mediaPath, m_textureCache, modelName, name, m_sceneDirectory);
    if (materialPT == nullptr)
    {
        caustica::warning("Error while parsing material file '%s'", actualLoadedFileName.c_str()); 
        return nullptr;
    }
    materialPT->sharedWithAllScenes = shared; // this property is not loaded from the file, but determined based on where the file was loaded from
    return materialPT;
}

void MaterialGpuCache::sceneReloaded()
{
    clear();
}

void MaterialGpuCache::completeDeferredTexturesLoad(nvrhi::ICommandList* commandList)
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

    msp.stableShaderName = caustica::render::MaterialFeatureTierStableName(tierIndex);
    msp.stableShaderId = int(tierIndex);
}

caustica::ShaderKey MaterialShaderPermutation::makeShaderKey(
    nvrhi::GraphicsAPI api,
    ShaderCompilerUtils::ShaderProfile profile) const
{
    return caustica::makeShaderLibraryKey(shaderFilePath, caustica::shader::fromNvrhiGraphicsApi(api), macros, profile);
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
        .macros = caustica::render::BuildMaterialShaderMacros(0),
    };
    InitializeStableShaderIdentity(ubershader);
    m_ubershader = std::make_shared<MaterialShaderPermutation>(ubershader);
    m_ubershader->uniqueMaterialName = "Ubershader";

    // Map materials to offline-compilable feature tiers (multiple materials can share a tier).
    m_shaderPermutations.clear();
    m_shaderPermutationTable.clear();
    for (auto& materialPT : m_materials)
    {
        MaterialShaderPermutation variant = materialPT->computeShaderPermutation(m_relativeShaderSourcePath);
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

        std::string proposedName = materialPT->uniqueFullName();
        if (ptVariant->uniqueMaterialName == "" || (ptVariant->uniqueMaterialName.compare(proposedName)>0) )  // keep the "smallest" name out of all - still not deterministic between different scenes but goodenough!
            ptVariant->uniqueMaterialName = proposedName;

        materialPT->bakedShaderPermutation = ptVariant;
    }
}

void MaterialGpuCache::createRenderPassesAndLoadMaterials(nvrhi::IBindingLayout* bindlessLayout, caustica::render::RenderDevice& renderDevice, const std::shared_ptr<caustica::Scene>& scene, const std::filesystem::path& sceneFilePath, const std::filesystem::path & mediaPath )
{
    info("MaterialGpuCache: createRenderPassesAndLoadMaterials begin");
    assert(!mediaPath.empty());
    //m_bindlessLayout = bindlessLayout;
    m_renderDevice = &renderDevice;

    {
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        bufferDesc.keepInitialState = true;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.byteSize = sizeof(PTMaterialData) * CAUSTICA_MATERIAL_MAX_COUNT;
        bufferDesc.structStride = sizeof(PTMaterialData);
        bufferDesc.debugName = "PTMaterialDataStorage";
        m_materialData = m_device->createBuffer(bufferDesc);
        m_materialDataWasReset = true;
    }

    if (m_mediaPath == "") // first time load all
    {
        info("MaterialGpuCache: first material load begin");
        m_mediaPath = mediaPath;
        m_sceneDirectory = sceneFilePath.parent_path();
        if (!sceneFilePath.empty() && sceneFilePath.filename() == "__CAUSTICA_INLINE_SCENE_JSON__")
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
  
        std::unordered_set<std::string> materialsPTUniqueNames;

        int initializedFromEngineCount = 0;

        auto& materials = scene->GetMaterials();
        for (auto& material : materials)
        {
            std::shared_ptr<MaterialEx> materialEx = std::dynamic_pointer_cast<MaterialEx>(material);
            if (materialEx == nullptr)
            {
                assert(false && "Is there something wrong with ExtendedSceneTypeFactory::CreateMaterial()?");
                continue;
            }
            else
            {
                if (IsBuiltinModelFileName(material->modelFileName))
                {
                    materialEx->ptData = importFromEngineMaterial(*material);
                }
                else
                {
                    std::shared_ptr<PTMaterial> loaded = load(material->modelFileName, material->name);
                    if (loaded != nullptr)
                    {
                        materialEx->ptData = loaded;
                    }
                    else // ...and if we didn't find it in our .scene.materials.json, then import from the engine material
                    {
                        std::shared_ptr<PTMaterial> materialPT = importFromEngineMaterial(*material);
                        materialEx->ptData = materialPT;
                        initializedFromEngineCount++;
                    }
                }
                materialEx->ptData->engineMaterialCounterpart = materialEx.get(); // keep the link - only needed if using material animation from the engine
                materialEx->ptData->runtimeMaterialGpuCache = this;

                std::string keyName = materialEx->ptData->modelName+"."+materialEx->ptData->name;
                auto existing = materialsPTUniqueNames.find(keyName);
                if (existing != materialsPTUniqueNames.end() )
                {
                    caustica::warning("Potential error while loading/converting materials for scene '%s' - there are at least two materials with the same name key '%s'.\nThis is not supported and will result in errors.\nIt's possible to fix some name collisions by including Material::materialIndexInModel into the name.",
                        sceneFilePath.string().c_str(), keyName.c_str());
                }
                else
                    materialsPTUniqueNames.insert(keyName);

                m_materials.push_back(materialEx->ptData);

                m_materialsGPU.push_back(PTMaterialData{});
                materialEx->ptData->gpuDataIndex = uint(m_materialsGPU.size() - 1);
                materialEx->ptData->gpuDataDirty = true;
                assert(m_materialsGPU.size() <= CAUSTICA_MATERIAL_MAX_COUNT);
            }
        }

        // sort by name so when we're saving it's consistent
        std::sort(m_materials.begin(), m_materials.end(), [](const auto & a, const auto & b) { return a->name < b->name; } );

        if (initializedFromEngineCount > 0)
            caustica::warning("There were %d materials not found in RTXPT material materials folder '%s'; consider doing Scene->Materials->Advanced->Save", initializedFromEngineCount, m_materialsPath.string().c_str());

        m_deferredTextureLoadInProgress = true;
        info("MaterialGpuCache: first material load end, materials=%zu", m_materials.size());
    }

    completeDeferredTexturesLoad(nullptr);

    info("MaterialGpuCache: record material textures begin");
    m_uniqueNames.clear(); // must be done before initializeUniqueDeterministicName
    for (auto& materialPT : m_materials)
    {
        recordTexture(materialPT->baseTexture);
        recordTexture(materialPT->occlusionRoughnessMetallicTexture);
        recordTexture(materialPT->normalTexture);
        recordTexture(materialPT->emissiveTexture);
        recordTexture(materialPT->transmissionTexture);

        initializeUniqueDeterministicName(materialPT);
    }
    info("MaterialGpuCache: record material textures end, uniqueTextures=%zu", m_textures.size());
    info("MaterialGpuCache: bake shader permutations begin");
    bakeShaderPermutations();
    info("MaterialGpuCache: bake shader permutations end");
    info("MaterialGpuCache: createRenderPassesAndLoadMaterials end");
}

// NOTE: this also handles some of the geometry data and mixed geometry&material stuff - it might be a good idea to rethink whether it needs to live outside of material baker
void UpdateSubInstanceData(SubInstanceData & ret, const std::shared_ptr<caustica::Scene> & scene, const std::shared_ptr<MeshInfo>& mesh, const caustica::MeshGeometry& geometry, uint meshGeometryIndex, const PTMaterial& material)
{
    if (!mesh || mesh->geometries.empty() || meshGeometryIndex >= mesh->geometries.size())
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

    const std::shared_ptr<MeshInfo>& meshRef = mesh;
    ret.FlagsAndAlphaInfo = 0;
    if (alphaTest)
    {
        ret.FlagsAndAlphaInfo |= SubInstanceData::Flags_AlphaTested;

        assert(meshRef->buffers->hasAttribute(VertexAttribute::TexCoord1));
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

    if (!meshRef->geometries[0])
        return;

    uint globalGeometryIndex = meshRef->geometries[0]->globalGeometryIndex + meshGeometryIndex;
    uint globalMaterialIndex = material.gpuDataIndex;
    ret.GlobalGeometryIndex_PTMaterialDataIndex = (globalGeometryIndex << 16) | globalMaterialIndex;

#if SUBINSTANCEDATA_EXTENDED
    GeometryData * gdata = scene->GetGeometryData(geometry);
    if( gdata != nullptr )
    {
        GeometryData & geometryData = *gdata;
        assert( geometryData.indexBufferIndex < 0xFFFF );
        assert( geometryData.vertexBufferIndex < 0xFFFF );
        ret.IndexBufferIndex_VertexBufferIndex = (geometryData.indexBufferIndex << 16) | geometryData.vertexBufferIndex;
        ret.IndexOffset = geometryData.indexOffset;
        ret.TexCoord1Offset = geometryData.texCoord1Offset;
    }
    else
    {
        assert( false );
    }
#endif

}

void MaterialGpuCache::update(nvrhi::ICommandList* commandList, const std::shared_ptr<caustica::Scene>& scene, std::vector<SubInstanceData>& subInstanceData)
{
    RAII_SCOPE( commandList->beginMarker("MaterialGpuCache");, commandList->endMarker(); );

    completeDeferredTexturesLoad(commandList);

    bool needsUpload = false;
    for (auto& materialPT : m_materials)
    {
        if (materialPT->useEngineEmissiveIntensity && materialPT->engineMaterialCounterpart != nullptr && materialPT->emissiveIntensity != materialPT->engineMaterialCounterpart->emissiveIntensity)
        {
            materialPT->emissiveIntensity = materialPT->engineMaterialCounterpart->emissiveIntensity;
            materialPT->gpuDataDirty = true;
        }

        if (!materialPT->gpuDataDirty && !m_materialDataWasReset)
            continue;

        materialPT->fillData(m_materialsGPU[materialPT->gpuDataIndex]);
        materialPT->gpuDataDirty = false;
        needsUpload = true;
    }

    if ( needsUpload )
    {
        commandList->writeBuffer( m_materialData, m_materialsGPU.data(), m_materialsGPU.size() * sizeof(PTMaterialData), 0 );
        m_materialDataWasReset = false;
    }

    // NOTE: this also handles some of the geometry data and mixed geometry&material stuff - it might be a good idea to rethink whether it needs to live outside of material baker
    const auto& instances = scene->GetMeshInstances();
    auto* entityWorld = scene->GetEntityWorld();
    for (const ecs::Entity entity : instances)
    {
        if (!entityWorld)
            continue;
        if (!entityWorld->world().isAlive(entity))
            continue;
        const auto* meshComp = entityWorld->world().get<scene::MeshInstanceComponent>(entity);
        if (!meshComp || !meshComp->mesh)
            continue;

        const auto& mesh = meshComp->mesh;
        if (meshComp->geometryInstanceIndex < 0)
            continue;

        const size_t firstSubInstanceIndex = static_cast<size_t>(meshComp->geometryInstanceIndex);
        for (size_t geometryIndex = 0; geometryIndex < mesh->geometries.size(); ++geometryIndex)
        {
            const auto& geometry = mesh->geometries[geometryIndex];
            const size_t subInstanceIndex = firstSubInstanceIndex + geometryIndex;
            if (!geometry || subInstanceIndex >= subInstanceData.size())
            {
                assert(false && "Sub-instance data is out of sync with scene geometry instances");
                continue;
            }

            assert( geometry->material != nullptr && "No handling for null materials!" );
            std::shared_ptr<PTMaterial> materialPT = PTMaterial::safeCast(geometry->material);
            assert(materialPT != nullptr && "Unknown error - should never have happened" );
            if (!materialPT)
                continue;

            UpdateSubInstanceData(subInstanceData[subInstanceIndex], scene, mesh, *geometry, geometryIndex, *materialPT);
        }
    }
}

/*
void MaterialGpuCache::loadAll(std::unordered_map<std::string, std::shared_ptr<PTMaterial>>& container)
{
    std::ifstream inFile(m_sceneMaterialsFilePath);

    if (!inFile.is_open())
        { caustica::warning("No RTXPT material definition file found at '%s' - consider doing Scene->Materials->Advanced->Save", m_sceneMaterialsFilePath.string().c_str()); return; }

    Json::Value rootJ;
    inFile >> rootJ;

    int version = -1;
    rootJ["RTXPTMaterials"]["version"] >> version;
    if (version != 1)
        { caustica::warning("Malformed or unsupported RTXPT material definition file version '%s' - consider doing Scene->Materials->Advanced->Save", m_sceneMaterialsFilePath.string().c_str()); return; }

    Json::Value materialsJ;

    materialsJ = rootJ["materials"];
    if (materialsJ.empty() || !materialsJ.isArray())
        { caustica::warning("Malformed or empty material definition file '%s' - consider doing Scene->Materials->Advanced->Save", m_sceneMaterialsFilePath.string().c_str()); return; }

    for ( Json::Value materialJ : materialsJ )
    {
        std::shared_ptr<PTMaterial> materialPT = materialPT->fromJson(materialJ, m_mediaPath, m_textureCache);
        if (materialPT == nullptr)
            { caustica::warning("Error while reading material in material definition file '%s'", m_sceneMaterialsFilePath.string().c_str()); continue; }
        
        auto existing = container.find(materialPT->name);
        if (existing != container.end())
            { caustica::warning("Duplicated materials with name '%s' found in material definition file '%s' - subsequent instances ignored.", materialPT->name.c_str(), m_sceneMaterialsFilePath.string().c_str()); assert( false ); continue; }
        else
            container.insert( make_pair(materialPT->name, materialPT) );
    }
}*/

bool MaterialGpuCache::loadSingle(PTMaterialBase & material)
{
    std::filesystem::path inPath = getMaterialStoragePath(material);

    Json::Value rootJ;
    if ( !caustica::json::LoadFromFile(inPath, rootJ) )
    {
        caustica::warning("No RTXPT material definition file found '%s'- consider doing Scene->Materials->Advanced->Save All", inPath.string().c_str());
        return false;
    }
    assert( material.name != "" );
    m_deferredTextureLoadInProgress = true;
    return material.read(rootJ, m_mediaPath, m_textureCache, m_sceneDirectory);
}

bool MaterialGpuCache::saveSingle(PTMaterialBase & material)
{
    if (!EnsureDirectoryExists(m_materialsPath))
        return false;
    std::filesystem::path outPath = m_materialsPath;
    if (!material.sharedWithAllScenes)
        outPath = m_materialsSceneSpecializedPath;
    if (!EnsureDirectoryExists(outPath))

    assert( material.modelName != "" && material.name != "" );
    outPath = getMaterialStoragePath(material);

    Json::Value rootJ;
    //rootJ["RTXPTMaterialVersion"] = 1;

    material.write(rootJ);

    // TODO: refactor, replace with caustica::json::SaveToFile
 
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
    for (auto& materialPT : m_materials)
    {
        Json::Value materialJ;
        materialPT->Write(materialJ, m_mediaPath);
        materialsJ.append(materialJ);
    }

    rootJ["materials"] = materialsJ;

    std::ofstream outFile(m_sceneMaterialsFilePath, std::ios::trunc);

    Json::StreamWriterBuilder builder;
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    
    writer->write(rootJ, &outFile);
    outFile.close();
#else
    for (auto& materialPT : m_materials)
        saveSingle(*materialPT);
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
                for (auto& materialPT : m_materials)
                    materialPT->sharedWithAllScenes = true;
            ImGui::SameLine();
            if (ImGui::Button("Not Shared"))
                for (auto& materialPT : m_materials)
                    materialPT->sharedWithAllScenes = false;
        }

        if( ImGui::Button("Save all") )
            saveAll();
    }

    return resetAccumulation;
}

MaterialShaderPermutation PTMaterial::computeShaderPermutation(const std::string& defaultShaderPath)
{
    const caustica::render::MaterialFeatureMask featureMask = caustica::render::ComputeMaterialFeatureMask(*this);
    const uint32_t tierIndex = caustica::render::MapFeatureMaskToTier(featureMask);
    return MaterialShaderPermutation{
        .shaderFilePath = defaultShaderPath,
        .macros = caustica::render::BuildMaterialShaderMacros(tierIndex),
    };
}

std::shared_ptr<PTMaterial> MaterialGpuCache::findByUniqueId(const std::string & name)
{
    for( int i = 0; i < m_materials.size(); i++)
        if (EqualsIgnoreCase(name, m_materials[i]->uniqueFullName()))
            return m_materials[i];

    return nullptr;
}
