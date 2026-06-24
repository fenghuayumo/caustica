#include <render/Passes/Lighting/MaterialsBaker.h>

#include <assets/loader/ShaderFactory.h>
#include <render/Core/FramebufferFactory.h>
#include <render/Core/CommonRenderPasses.h>
#include <assets/cache/TextureCache.h>

#include <engine/UserInterfaceUtils.h>

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
#include <render/Core/ScopedPerfMarker.h>
#include <ui/ui_macros.h>
#include <render/Core/TextureUtils.h>
#include <scene/Scene.h>

#include <core/json.h>
#include <json/json.h>

#include <algorithm>
#include <fstream>

#include <unordered_set>

#include <cctype>      // std::tolower
#include <cstring>

#include <SampleUI.h>

#include <render/Passes/Debug/picosha2.h>

#include <shaders/bindless.h>

using namespace caustica::math;
using namespace caustica;

static std::string kNoModel = "<no_model>";

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

void PTTexture::InitFromLoadedTexture(std::shared_ptr<caustica::LoadedTexture> & loaded, bool _sRGB, bool _normalMap, const std::filesystem::path & mediaPath)
{
    if (loaded == nullptr)
    { LocalPath = ""; sRGB = false; Loaded = nullptr; NormalMap = false; Enabled = false; return; }

    LocalPath = std::filesystem::relative(loaded->path, mediaPath);
    sRGB = _sRGB;
    Loaded = loaded;
    NormalMap = _normalMap;
    Enabled = true;
}

std::shared_ptr<PTMaterial> PTMaterial::SafeCast(const std::shared_ptr<Material>& bridgeMaterial)
{
    if (bridgeMaterial == nullptr)
        return nullptr;
    assert(std::dynamic_pointer_cast<MaterialEx>(bridgeMaterial) != nullptr);
    return std::static_pointer_cast<MaterialEx>(bridgeMaterial)->ptData;
}

void PTMaterial::Write(Json::Value& output)
{
    auto saveTexture = [ ](Json::Value& output, const PTTexture & texture, const std::string& name)
    {
        if (texture.Loaded == nullptr)
            return;
        Json::Value texJ;
        texJ["sRGB"] = texture.sRGB;
        texJ["NormalMap"] = texture.NormalMap;
        texJ["path"] = texture.LocalPath.string();
        output[name] = texJ;
    };

    //output["name"] = Name;
    output["version"] = 1;

    saveTexture(output, BaseTexture, "BaseTexture");
    saveTexture(output, OcclusionRoughnessMetallicTexture, "OcclusionRoughnessMetallicTexture");
    saveTexture(output, NormalTexture, "NormalTexture");
    saveTexture(output, EmissiveTexture, "EmissiveTexture");
    saveTexture(output, TransmissionTexture, "TransmissionTexture");

#define STORE_FIELD(NAME) output[#NAME] << NAME;

    STORE_FIELD(BaseOrDiffuseColor);
    STORE_FIELD(SpecularColor);
    STORE_FIELD(EmissiveColor);

    STORE_FIELD(MaterialModel);
    STORE_FIELD(BaseWeight);
    STORE_FIELD(SpecularWeight);
    STORE_FIELD(Anisotropy);
    STORE_FIELD(FuzzWeight);
    STORE_FIELD(FuzzColor);
    STORE_FIELD(FuzzRoughness);

    STORE_FIELD(EmissiveIntensity);
    STORE_FIELD(Metalness);
    STORE_FIELD(Roughness);
    STORE_FIELD(Opacity);
    STORE_FIELD(TransmissionFactor);
    STORE_FIELD(DiffuseTransmissionFactor);
    STORE_FIELD(NormalTextureScale);
    STORE_FIELD(IoR);

    STORE_FIELD(UseSpecularGlossModel);
    STORE_FIELD(EnableBaseTexture);
    STORE_FIELD(EnableOcclusionRoughnessMetallicTexture);
    STORE_FIELD(EnableNormalTexture);
    STORE_FIELD(EnableEmissiveTexture);
    STORE_FIELD(EnableTransmissionTexture);
    STORE_FIELD(EnableAlphaTesting);
    STORE_FIELD(AlphaCutoff);
    STORE_FIELD(EnableTransmission);
    STORE_FIELD(MetalnessInRedChannel);
    STORE_FIELD(ThinSurface);
    STORE_FIELD(ExcludeFromNEE);
    STORE_FIELD(PSDExclude);
    STORE_FIELD(PSDDominantDeltaLobe);
    STORE_FIELD(PSDBlockMotionVectorsAtSurfaceType);
    STORE_FIELD(NestedPriority);

    STORE_FIELD(VolumeAttenuationDistance);
    STORE_FIELD(VolumeAttenuationColor);

    STORE_FIELD(ShadowNoLFadeout);

    STORE_FIELD(EnableAsAnalyticLightProxy);

    STORE_FIELD(IgnoreMeshTangentSpace);

    STORE_FIELD(UseEngineEmissiveIntensity);

    STORE_FIELD(SkipRender);

    if (IsOpenPBRMaterialModel(MaterialModel))
    {
        Json::Value& openPBR = output["OpenPBR"];
        openPBR["base_weight"] << BaseWeight;
        openPBR["base_color"] << BaseOrDiffuseColor;
        openPBR["base_metalness"] << Metalness;
        openPBR["specular_weight"] << SpecularWeight;
        openPBR["specular_color"] << SpecularColor;
        openPBR["specular_roughness"] << Roughness;
        openPBR["specular_roughness_anisotropy"] << Anisotropy;
        openPBR["specular_ior"] << IoR;
        openPBR["transmission_weight"] << TransmissionFactor;
        openPBR["transmission_diffuse_weight"] << DiffuseTransmissionFactor;
        openPBR["geometry_opacity"] << Opacity;
        openPBR["geometry_thin_walled"] << ThinSurface;
        openPBR["emission_color"] << EmissiveColor;
        openPBR["emission_luminance"] << EmissiveIntensity;
        openPBR["fuzz_weight"] << FuzzWeight;
        openPBR["fuzz_color"] << FuzzColor;
        openPBR["fuzz_roughness"] << FuzzRoughness;
    }
}

bool PTMaterial::Read(
    Json::Value& input,
    const std::filesystem::path& mediaPath,
    const std::shared_ptr<caustica::TextureCache>& textureCache,
    const std::filesystem::path& sceneDirectory)
{
    // int version = -1;
    // input["version"] >> version;
    // if (version != 1)
    // {
    //     caustica::warning("Unsupported/missing material version"); return nullptr;
    // }

    const bool hasMaterialModelField = input.isMember("MaterialModel");
    const bool hasOpenPBRBlock = input.isMember("OpenPBR");
    const bool hasTopLevelOpenPBRFields = HasOpenPBRLiteFields(input);

    auto loadTexture = [ & ](Json::Value& input, PTTexture& output, const std::string& name)
    {
        output = PTTexture();
        Json::Value texJ = input[name];

        if (texJ.empty())
            return;

        std::string localPath;
        texJ["path"] >> localPath; output.LocalPath = localPath;
        if (output.LocalPath == "")
        {
            caustica::warning("Path for texture is empty"); return;
        }
        texJ["sRGB"] >> output.sRGB;
        texJ["NormalMap"] >> output.NormalMap;

        std::filesystem::path storagePath;
        std::filesystem::path fullPath = ResolveMaterialTexturePath(
            output.LocalPath,
            sceneDirectory,
            mediaPath,
            storagePath);
        output.LocalPath = storagePath;

        output.Loaded = textureCache->LoadTextureFromFileDeferred(fullPath, output.sRGB);
        output.Enabled = output.Loaded != nullptr;
    };

    loadTexture(input, this->BaseTexture, "BaseTexture");
    loadTexture(input, this->OcclusionRoughnessMetallicTexture, "OcclusionRoughnessMetallicTexture");
    loadTexture(input, this->NormalTexture, "NormalTexture");
    loadTexture(input, this->EmissiveTexture, "EmissiveTexture");
    loadTexture(input, this->TransmissionTexture, "TransmissionTexture");

#define LOAD_FIELD(NAME) input[#NAME] >> this->NAME;

    LOAD_FIELD(BaseOrDiffuseColor);
    LOAD_FIELD(SpecularColor);
    LOAD_FIELD(EmissiveColor);

    LOAD_FIELD(MaterialModel);
    LOAD_FIELD(BaseWeight);
    LOAD_FIELD(SpecularWeight);
    LOAD_FIELD(Anisotropy);
    LOAD_FIELD(FuzzWeight);
    LOAD_FIELD(FuzzColor);
    LOAD_FIELD(FuzzRoughness);

    LOAD_FIELD(EmissiveIntensity);
    LOAD_FIELD(Metalness);
    LOAD_FIELD(Roughness);
    LOAD_FIELD(Opacity);
    LOAD_FIELD(TransmissionFactor);
    LOAD_FIELD(DiffuseTransmissionFactor);
    LOAD_FIELD(NormalTextureScale);
    LOAD_FIELD(IoR);

    LOAD_FIELD(UseSpecularGlossModel);
    LOAD_FIELD(EnableBaseTexture);
    LOAD_FIELD(EnableOcclusionRoughnessMetallicTexture);
    LOAD_FIELD(EnableNormalTexture);
    LOAD_FIELD(EnableEmissiveTexture);
    LOAD_FIELD(EnableTransmissionTexture);
    LOAD_FIELD(EnableAlphaTesting);
    LOAD_FIELD(AlphaCutoff);
    LOAD_FIELD(EnableTransmission);
    LOAD_FIELD(MetalnessInRedChannel);
    LOAD_FIELD(ThinSurface);
    LOAD_FIELD(ExcludeFromNEE);
    LOAD_FIELD(PSDExclude);
    LOAD_FIELD(PSDBlockMotionVectorsAtSurfaceType);

    LOAD_FIELD(PSDDominantDeltaLobe);
    LOAD_FIELD(NestedPriority);

    LOAD_FIELD(VolumeAttenuationDistance);
    LOAD_FIELD(VolumeAttenuationColor);

    LOAD_FIELD(ShadowNoLFadeout);

    LOAD_FIELD(EnableAsAnalyticLightProxy);

    LOAD_FIELD(IgnoreMeshTangentSpace);

    LOAD_FIELD(UseEngineEmissiveIntensity);
    if (!input.isMember("UseEngineEmissiveIntensity") && input.isMember("UseDonutEmissiveIntensity"))
        input["UseDonutEmissiveIntensity"] >> UseEngineEmissiveIntensity;

    LOAD_FIELD(SkipRender);

    auto readOpenPBRLite = [this](const Json::Value& openPBR)
    {
        if (!openPBR.isObject())
            return;

        MaterialModel = "OpenPBR";

        if (!openPBR.isMember("specular_color"))
            SpecularColor = dm::float3(1.f);

        ReadJsonMember(openPBR, "base_weight", BaseWeight);
        ReadJsonMember(openPBR, "base_color", BaseOrDiffuseColor);
        ReadJsonMember(openPBR, "base_metalness", Metalness);

        ReadJsonMember(openPBR, "specular_weight", SpecularWeight);
        ReadJsonMember(openPBR, "specular_color", SpecularColor);
        ReadJsonMember(openPBR, "specular_roughness", Roughness);
        ReadJsonMember(openPBR, "specular_roughness_anisotropy", Anisotropy);
        ReadJsonMember(openPBR, "specular_ior", IoR);

        bool hasSpecularTransmission = ReadJsonMember(openPBR, "transmission_weight", TransmissionFactor);
        bool hasDiffuseTransmission = ReadJsonMember(openPBR, "transmission_diffuse_weight", DiffuseTransmissionFactor);
        EnableTransmission |= hasSpecularTransmission || hasDiffuseTransmission || TransmissionFactor > 0.0f || DiffuseTransmissionFactor > 0.0f;

        ReadJsonMember(openPBR, "geometry_opacity", Opacity);
        ReadJsonMember(openPBR, "geometry_thin_walled", ThinSurface);

        ReadJsonMember(openPBR, "emission_color", EmissiveColor);
        ReadJsonMember(openPBR, "emission_luminance", EmissiveIntensity);

        ReadJsonMember(openPBR, "fuzz_weight", FuzzWeight);
        ReadJsonMember(openPBR, "fuzz_color", FuzzColor);
        ReadJsonMember(openPBR, "fuzz_roughness", FuzzRoughness);

        UseSpecularGlossModel = false;
    };

    if (hasOpenPBRBlock)
        readOpenPBRLite(input["OpenPBR"]);
    else if ((hasMaterialModelField && IsOpenPBRMaterialModel(MaterialModel)) || hasTopLevelOpenPBRFields)
        readOpenPBRLite(input);
    else if (!hasMaterialModelField && !UseSpecularGlossModel)
    {
        MaterialModel = "OpenPBR";
        SpecularColor = dm::float3(1.f);
    }
    else if (!hasMaterialModelField && UseSpecularGlossModel)
        MaterialModel = "RTXPT";

    BaseWeight = std::clamp(BaseWeight, 0.0f, 1.0f);
    SpecularWeight = std::max(SpecularWeight, 0.0f);
    Anisotropy = std::clamp(Anisotropy, -1.0f, 1.0f);
    FuzzWeight = std::clamp(FuzzWeight, 0.0f, 1.0f);
    FuzzRoughness = std::clamp(FuzzRoughness, 0.0f, 1.0f);

    return true;
}

std::shared_ptr<PTMaterial> PTMaterial::FromJson(
    Json::Value& input,
    const std::filesystem::path& mediaPath,
    const std::shared_ptr<caustica::TextureCache>& textureCache,
    const std::string& modelName,
    const std::string& name,
    const std::filesystem::path& sceneDirectory)
{
    std::shared_ptr<PTMaterial> material = std::make_shared<PTMaterial>();

    material->Read(input, mediaPath, textureCache, sceneDirectory);
    material->Name = name;
    material->ModelName = modelName;

    return material;
}

PTTexture& PTMaterial::GetTexture(PTMaterialTextureSlot slot)
{
    switch (slot)
    {
    case PTMaterialTextureSlot::Base:
        return BaseTexture;
    case PTMaterialTextureSlot::OcclusionRoughnessMetallic:
        return OcclusionRoughnessMetallicTexture;
    case PTMaterialTextureSlot::Normal:
        return NormalTexture;
    case PTMaterialTextureSlot::Emissive:
        return EmissiveTexture;
    case PTMaterialTextureSlot::Transmission:
        return TransmissionTexture;
    default:
        assert(false);
        return BaseTexture;
    }
}

const PTTexture& PTMaterial::GetTexture(PTMaterialTextureSlot slot) const
{
    return const_cast<PTMaterial*>(this)->GetTexture(slot);
}

bool PTMaterial::IsTextureEnabled(PTMaterialTextureSlot slot) const
{
    switch (slot)
    {
    case PTMaterialTextureSlot::Base:
        return EnableBaseTexture;
    case PTMaterialTextureSlot::OcclusionRoughnessMetallic:
        return EnableOcclusionRoughnessMetallicTexture;
    case PTMaterialTextureSlot::Normal:
        return EnableNormalTexture;
    case PTMaterialTextureSlot::Emissive:
        return EnableEmissiveTexture;
    case PTMaterialTextureSlot::Transmission:
        return EnableTransmissionTexture;
    default:
        assert(false);
        return false;
    }
}

void PTMaterial::SetTextureEnabled(PTMaterialTextureSlot slot, bool enabled)
{
    switch (slot)
    {
    case PTMaterialTextureSlot::Base:
        EnableBaseTexture = enabled;
        break;
    case PTMaterialTextureSlot::OcclusionRoughnessMetallic:
        EnableOcclusionRoughnessMetallicTexture = enabled;
        break;
    case PTMaterialTextureSlot::Normal:
        EnableNormalTexture = enabled;
        break;
    case PTMaterialTextureSlot::Emissive:
        EnableEmissiveTexture = enabled;
        break;
    case PTMaterialTextureSlot::Transmission:
        EnableTransmissionTexture = enabled;
        break;
    default:
        assert(false);
        break;
    }
}

bool PTMaterial::EditorGUI(MaterialsBaker & baker)
{
    bool update = false;

    float itemWidth = ImGui::CalcItemWidth();

    auto getShortTexturePath = [ ](const PTTexture & texture) -> std::string
    {
        if( texture.Loaded == nullptr ) return "<nullptr>";
        return texture.LocalPath.string();
    };

    if (ImGui::CollapsingHeader("Special Properties"))
    {
        ImGui::Indent();
        {
            UI_SCOPED_DISABLE(!EnableTransmission);
            update |= ImGui::Checkbox("Thin surface", &ThinSurface);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Material has no volumetric properties - used for double sided thin surfaces like leafs.\nNon-transparent materials are always considered thin surface.");

        update |= ImGui::Checkbox("Ignore by NEE shadow ray (bias!)", &ExcludeFromNEE);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ignored for shadow rays during Next Event Estimation\nNote: this isn't physically correct - it adds bias!");

        update |= ImGui::SliderFloat("Shadow NoL Fadeout", &ShadowNoLFadeout, 0.0f, 0.2f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Low tessellation geometry often has triangle (flat) normals that differ significantly from shading normals. \n"
            "This causes shading vs shadow discrepancy that exposes triangle edges. One way to mitigate this (other than \n"
            "having more detailed mesh) is to add additional shadowing falloff to hide the seam. This setting is not \n"
            "physically correct and adds bias. Setting of 0 means no fadeout (default).");

        update |= ImGui::Checkbox("Enable as analytic light proxy", &EnableAsAnalyticLightProxy);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Any scene object with this material will look at it's parent node in the scenegraph; if the parent contains\n"
            "an analytic light, the rays falling of this surface will also output radiance from the analytic light.\n"
            "The more closely the object's mesh resembles the analytic light, the more physically correct results will be.\n");

        update |= ImGui::Checkbox("Emissive intensity from engine material", &UseEngineEmissiveIntensity);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Engine materials can have emissive intensity animation attached; this allows RTXPT to use it\n");

        update |= ImGui::Checkbox("Ignore mesh tangent space", &IgnoreMeshTangentSpace);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("This will ignore tangent space loaded from the mesh and generate new one - can help issues with normals.");

        update |= ImGui::Checkbox("Skip render", &SkipRender);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ignore all meshes with this material - sometimes easier than removing the object itself.\nNote: this will not remove it as an emissive light on the light sampling side!");

        std::string fullName = UniqueFullName();
        ImGui::TextWrapped("Full, unique ID: %s", fullName.c_str());
        if (ImGui::Button("Copy to clipboard"))
            ImGui::SetClipboardText(fullName.c_str());

        ImGui::Unindent();
    }

    const ImVec4 filenameColor = ImVec4(0.474f, 0.722f, 0.176f, 1.0f);

    auto drawTextureToggle = [&](const char* label, PTTexture& texture, bool& enabled)
    {
        if (texture.Loaded != nullptr)
        {
            update |= ImGui::Checkbox(label, &enabled);
            ImGui::SameLine();
            ImGui::TextColored(filenameColor, "%s", getShortTexturePath(texture).c_str());
        }
    };

    bool useOpenPBRLite = IsOpenPBRMaterialModel(MaterialModel);
    int materialModelIndex = useOpenPBRLite ? 0 : 1;
    if (ImGui::Combo("Material Model", &materialModelIndex, "OpenPBR-lite\0RTXPT legacy\0\0"))
    {
        useOpenPBRLite = (materialModelIndex == 0);
        MaterialModel = useOpenPBRLite ? "OpenPBR" : "RTXPT";
        if (useOpenPBRLite)
        {
            UseSpecularGlossModel = false;
            const float* specularColor = SpecularColor.data();
            if (specularColor[0] == 0.f && specularColor[1] == 0.f && specularColor[2] == 0.f)
                SpecularColor = dm::float3(1.f);
        }
        update = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("OpenPBR-lite exposes standard material parameter names and maps them onto the RTXPT backend.");

    if (useOpenPBRLite && UseSpecularGlossModel)
    {
        UseSpecularGlossModel = false;
        update = true;
    }

    if (useOpenPBRLite)
    {
        drawTextureToggle("Use base_color texture", BaseTexture, EnableBaseTexture);

        update |= ImGui::ColorEdit3(EnableBaseTexture ? "base_color factor" : "base_color", BaseOrDiffuseColor.data(), ImGuiColorEditFlags_Float);
        update |= ImGui::SliderFloat("base_weight", &BaseWeight, 0.f, 1.f);

        drawTextureToggle("Use base_metalness/specular_roughness texture", OcclusionRoughnessMetallicTexture, EnableOcclusionRoughnessMetallicTexture);

        update |= ImGui::SliderFloat(EnableOcclusionRoughnessMetallicTexture ? "base_metalness factor" : "base_metalness", &Metalness, 0.f, 1.f);
        update |= ImGui::SliderFloat("specular_weight", &SpecularWeight, 0.f, 2.f);
        update |= ImGui::ColorEdit3("specular_color", SpecularColor.data(), ImGuiColorEditFlags_Float);
        update |= ImGui::SliderFloat(EnableOcclusionRoughnessMetallicTexture ? "specular_roughness factor" : "specular_roughness", &Roughness, 0.f, 1.f);
        update |= ImGui::SliderFloat("specular_roughness_anisotropy", &Anisotropy, -1.f, 1.f);
        update |= ImGui::InputFloat("specular_ior", &IoR);
        if (IoR < 1.0f) { IoR = 1.0f; update = true; }

        update |= ImGui::SliderFloat("fuzz_weight", &FuzzWeight, 0.f, 1.f);
        update |= ImGui::ColorEdit3("fuzz_color", FuzzColor.data(), ImGuiColorEditFlags_Float);
        update |= ImGui::SliderFloat("fuzz_roughness", &FuzzRoughness, 0.f, 1.f);

        update |= ImGui::SliderFloat("geometry_opacity", &Opacity, 0.f, 1.f);
        update |= ImGui::Checkbox("geometry_thin_walled", &ThinSurface);

        drawTextureToggle("Use transmission_weight texture", TransmissionTexture, EnableTransmissionTexture);

        float previousTransmissionFactor = TransmissionFactor;
        float previousDiffuseTransmissionFactor = DiffuseTransmissionFactor;
        update |= ImGui::SliderFloat("transmission_weight", &TransmissionFactor, 0.f, 1.f);
        update |= ImGui::SliderFloat("transmission_diffuse_weight", &DiffuseTransmissionFactor, 0.f, 1.f);

        bool openPBRTransmissionEnabled = (TransmissionFactor > 0.f) || (DiffuseTransmissionFactor > 0.f);
        if (openPBRTransmissionEnabled != EnableTransmission)
        {
            EnableTransmission = openPBRTransmissionEnabled;
            update = true;
        }
        if (previousTransmissionFactor != TransmissionFactor || previousDiffuseTransmissionFactor != DiffuseTransmissionFactor)
            EnableTransmission = openPBRTransmissionEnabled;

        if (EnableTransmission && !ThinSurface)
        {
            update |= ImGui::InputFloat("volume_attenuation_distance", &VolumeAttenuationDistance);
            if (VolumeAttenuationDistance < 0.0f) { VolumeAttenuationDistance = 0.0f; update = true; }
            update |= ImGui::ColorEdit3("volume_attenuation_color", VolumeAttenuationColor.data(), ImGuiColorEditFlags_Float);
            update |= ImGui::InputInt("nested_priority", &NestedPriority);
            if (NestedPriority < 0 || NestedPriority > 14) { NestedPriority = dm::clamp(NestedPriority, 0, 14); update = true; }
        }
    }
    else if (UseSpecularGlossModel)
    {
        drawTextureToggle("Use Base (Diffuse) Texture", BaseTexture, EnableBaseTexture);

        update |= ImGui::ColorEdit3(EnableBaseTexture ? "Diffuse Factor" : "Diffuse Color", BaseOrDiffuseColor.data(), ImGuiColorEditFlags_Float);

        drawTextureToggle("Use Specular Texture", OcclusionRoughnessMetallicTexture, EnableOcclusionRoughnessMetallicTexture);

        update |= ImGui::ColorEdit3(EnableOcclusionRoughnessMetallicTexture ? "Specular Factor" : "Specular Color", SpecularColor.data(), ImGuiColorEditFlags_Float);

        float glossiness = 1.0f - Roughness;
        update |= ImGui::SliderFloat(EnableOcclusionRoughnessMetallicTexture ? "Glossiness Factor" : "Glossiness", &glossiness, 0.f, 1.f);
        Roughness = 1.0f - glossiness;
    }
    else
    {
        drawTextureToggle("Use Base (Diffuse) Texture", BaseTexture, EnableBaseTexture);

        update |= ImGui::ColorEdit3(EnableBaseTexture ? "Base Color Factor" : "Base Color", BaseOrDiffuseColor.data(), ImGuiColorEditFlags_Float);

        drawTextureToggle("Use Metal-Rough Texture", OcclusionRoughnessMetallicTexture, EnableOcclusionRoughnessMetallicTexture);

        update |= ImGui::SliderFloat(EnableOcclusionRoughnessMetallicTexture ? "Metalness Factor" : "Metalness", &Metalness, 0.f, 1.f);
        update |= ImGui::SliderFloat(EnableOcclusionRoughnessMetallicTexture ? "Roughness Factor" : "Roughness", &Roughness, 0.f, 1.f);
    }

    update |= ImGui::Checkbox(useOpenPBRLite ? "geometry_enable_alpha_test" : "Enable Alpha Testing", &EnableAlphaTesting);

    if (EnableAlphaTesting && BaseTexture.Loaded)
    {
        update |= ImGui::SliderFloat(useOpenPBRLite ? "geometry_alpha_cutoff" : "Alpha Cutoff", &AlphaCutoff, 0.f, 1.f);
    }

    drawTextureToggle(useOpenPBRLite ? "Use geometry_normal texture" : "Use Normal Texture", NormalTexture, EnableNormalTexture);

    if (EnableNormalTexture)
    {
        ImGui::SetNextItemWidth(itemWidth - 31.f);
        update |= ImGui::SliderFloat("###normtexscale", &NormalTextureScale, -2.f, 2.f);
        ImGui::SameLine(0.f, 5.f);
        ImGui::SetNextItemWidth(26.f);
        if (ImGui::Button("1.0"))
        {
            NormalTextureScale = 1.f;
            update = true;
        }
        ImGui::SameLine();
        ImGui::Text(useOpenPBRLite ? "geometry_normal_scale" : "Normal Scale");
    }

    drawTextureToggle(useOpenPBRLite ? "Use emission_color texture" : "Use Emissive Texture", EmissiveTexture, EnableEmissiveTexture);

    update |= ImGui::ColorEdit3(useOpenPBRLite ? "emission_color" : "Emissive Color", EmissiveColor.data(), ImGuiColorEditFlags_Float);
    update |= ImGui::SliderFloat(useOpenPBRLite ? "emission_luminance" : "Emissive Intensity", &EmissiveIntensity, 0.f, 100000.f, "%.3f", ImGuiSliderFlags_Logarithmic);

    if (!useOpenPBRLite)
    {
        update |= ImGui::Checkbox("Enable Transmission", &EnableTransmission);

        if (EnableTransmission)   // transmissive
        {
            update |= ImGui::InputFloat("Index of Refraction", &IoR);
            if (IoR < 1.0f) { IoR = 1.0f; update = true; }

            drawTextureToggle("Use Transmission Texture", TransmissionTexture, EnableTransmissionTexture);

            update |= ImGui::SliderFloat("Transmission Factor", &TransmissionFactor, 0.f, 1.f);
            update |= ImGui::SliderFloat("Diff Transmission Factor", &DiffuseTransmissionFactor, 0.f, 1.f);

            if (!ThinSurface)
            {
                update |= ImGui::InputFloat("Attenuation Distance", &VolumeAttenuationDistance);
                if (VolumeAttenuationDistance < 0.0f) { VolumeAttenuationDistance = 0.0f; update = true; }

                update |= ImGui::ColorEdit3("Attenuation Color", VolumeAttenuationColor.data(), ImGuiColorEditFlags_Float);

                update |= ImGui::InputInt("Nested Priority", &NestedPriority);
                if (NestedPriority < 0 || NestedPriority > 14) { NestedPriority = dm::clamp(NestedPriority, 0, 14); update = true; }
            }
            else
            {
                ImGui::Text("Thin surface transmissive materials have no volume properties");
            }
        }
    }

    if (ImGui::CollapsingHeader("Path Decomposition"))
    {
        ImGui::Indent();

        update |= ImGui::Combo("Block mv-s at surface", (int*)&PSDBlockMotionVectorsAtSurfaceType, "Off\0AutoLow\0AutoHigh\0Full\0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Curved surfaces cause motion vectors on reflected or transmitted\nsegments to be incorrect and are better disabled.\n"
            "When this is enabled, motion for all de-composited paths will come\nfrom this surface. AutoLow and AutoHigh will attempt to set the flag based on\n surface curvature (with Low and High sensitivities).");

        bool psdEnable = !PSDExclude; // makes more sense from UI perspective - avoids double negative
        update |= ImGui::Checkbox("Enable delta lobe decomposition", &psdEnable);
        PSDExclude = !psdEnable;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Some materials/meshes look best without decomposition.");
        
        {
            UI_SCOPED_DISABLE(PSDExclude);
            int dominantDeltaLobeP1 = dm::clamp(PSDDominantDeltaLobe, -1, 1) + 1;
            update |= ImGui::Combo("Dominant bounce", &dominantDeltaLobeP1, "None (surface)\0Transparency\0Reflection\0\0");
            PSDDominantDeltaLobe = dm::clamp(dominantDeltaLobeP1 - 1, -1, 1);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Determines which surface will:\n * provide motion vectors for denoising\n * get ReSTIR DI lighting\n * get 'boost samples' for NEE lighting");
        }
        ImGui::Unindent();
    }

    if (ImGui::CollapsingHeader("Save/Load"))
    {
        RAII_SCOPE( ImGui::Indent();, ImGui::Unindent(); );

        ImGui::Checkbox("Share with all scenes", &SharedWithAllScenes);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("if checked, material saved to /Assets/Materials/ path and \nshared between all scenes; otherwise it will be saved to \n/Assets/Materials/SceneName specific to current scene");

        auto matPath = baker.GetMaterialStoragePath(*this);

        ImGui::TextWrapped("File name: %s", matPath.string().c_str());

        if (ImGui::Button("Load"))
        {
            baker.LoadSingle(*this);
            update = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Save"))
            baker.SaveSingle(*this);
    }

    // mark for update
    GPUDataDirty |= update;

    return update;
}

static void GetBindlessTextureIndex(const std::shared_ptr<LoadedTexture>& texture, uint& outEncodedInfo, unsigned int& flags, unsigned int textureBit)
{
    // if bit not set, don't set the texture; if texture unavailable - remove the texture bit!
    if ((flags & textureBit) == 0 || texture == nullptr || texture->texture == nullptr || texture->bindlessDescriptor.Get() == ~0u)
    {
        outEncodedInfo = 0xFFFFFFFF;
        flags &= ~textureBit; // remove flag
        return;
    }

    uint bindlessDescIndex = texture->bindlessDescriptor.Get();
    assert(bindlessDescIndex <= 0xFFFF);
    bindlessDescIndex &= 0xFFFF;

    const auto desc = texture->texture->getDesc();
    float baseLODf = caustica::math::log2f((float)desc.width * desc.height);
    uint baseLOD = (uint)(baseLODf + 0.5f);
    uint mipLevels = desc.mipLevels;
    assert(baseLOD >= 0 && baseLOD <= 255);
    assert(mipLevels >= 0 && mipLevels <= 255);

    outEncodedInfo = (baseLOD << 24) | (mipLevels << 16) | bindlessDescIndex;
}

bool PTMaterial::IsEmissive() const
{
    return (EmissiveIntensity > 0) && (caustica::math::any(EmissiveColor>0.0f)) || UseEngineEmissiveIntensity;  // UseEngineEmissiveIntensity can animate on/off so just assume we're emissive and pay the cost
}

void PTMaterial::FillData(PTMaterialData & data)
{
    // flags

    data.Flags = 0;

    if (UseSpecularGlossModel)
        data.Flags |= PTMaterialFlags_UseSpecularGlossModel;

    if (BaseTexture.Loaded && EnableBaseTexture)
        data.Flags |= PTMaterialFlags_UseBaseOrDiffuseTexture;

    if (OcclusionRoughnessMetallicTexture.Loaded && EnableOcclusionRoughnessMetallicTexture)
        data.Flags |= PTMaterialFlags_UseMetalRoughOrSpecularTexture;

    if (EmissiveTexture.Loaded && EnableEmissiveTexture)
        data.Flags |= PTMaterialFlags_UseEmissiveTexture;

    if (NormalTexture.Loaded && EnableNormalTexture)
        data.Flags |= PTMaterialFlags_UseNormalTexture;

    if (TransmissionTexture.Loaded && EnableTransmissionTexture && EnableTransmission)
        data.Flags |= PTMaterialFlags_UseTransmissionTexture;

    if (MetalnessInRedChannel)
        data.Flags |= PTMaterialFlags_MetalnessInRedChannel;

    if (ThinSurface || !EnableTransmission) // materials with no transmission are automatically considered thin surface - simplifies a lot of things
        data.Flags |= PTMaterialFlags_ThinSurface;

    if (PSDExclude)
        data.Flags |= PTMaterialFlags_PSDExclude;

    if (PSDBlockMotionVectorsAtSurfaceType % 2)
        data.Flags |= PTMaterialFlags_PSDBlockMVsAtSurfaceTypeB0;

    if (PSDBlockMotionVectorsAtSurfaceType / 2)
        data.Flags |= PTMaterialFlags_PSDBlockMVsAtSurfaceTypeB1;

    if (EnableAsAnalyticLightProxy)
        data.Flags |= PTMaterialFlags_EnableAsAnalyticLightProxy;

    if (IgnoreMeshTangentSpace)
        data.Flags |= PTMaterialFlags_IgnoreMeshTangentSpace;

    if (IsOpenPBRMaterialModel(MaterialModel))
        data.Flags |= PTMaterialFlags_UseOpenPBRMaterialModel;

    // free parameters

    data.BaseOrDiffuseColor = BaseOrDiffuseColor;
    data.SpecularColor = SpecularColor;
    data.EmissiveColor = EmissiveColor * EmissiveIntensity;
    data.Roughness = Roughness;
    data.Metalness = Metalness;
    data.BaseWeight = std::clamp(BaseWeight, 0.0f, 1.0f);
    data.SpecularWeight = std::max(SpecularWeight, 0.0f);
    data.Anisotropy = std::clamp(Anisotropy, -1.0f, 1.0f);
    data.FuzzWeight = std::clamp(FuzzWeight, 0.0f, 1.0f);
    data.FuzzColor = FuzzColor;
    data.FuzzRoughness = std::clamp(FuzzRoughness, 0.0f, 1.0f);
    data.NormalTextureScale = NormalTextureScale;
    data.TransmissionFactor = (EnableTransmission)?(TransmissionFactor):(0);
    data.DiffuseTransmissionFactor = (EnableTransmission)?(DiffuseTransmissionFactor):(0);
    data.Opacity = Opacity;
    data.AlphaCutoff = AlphaCutoff;
    data.IoR = IoR;
    data.Volume.AttenuationColor    = VolumeAttenuationColor;
    data.Volume.AttenuationDistance = VolumeAttenuationDistance;

    // bindless textures

    GetBindlessTextureIndex(BaseTexture.Loaded, data.BaseOrDiffuseTextureIndex, data.Flags, PTMaterialFlags_UseBaseOrDiffuseTexture);
    GetBindlessTextureIndex(OcclusionRoughnessMetallicTexture.Loaded, data.MetalRoughOrSpecularTextureIndex, data.Flags, PTMaterialFlags_UseMetalRoughOrSpecularTexture);
    GetBindlessTextureIndex(EmissiveTexture.Loaded, data.EmissiveTextureIndex, data.Flags, PTMaterialFlags_UseEmissiveTexture);
    GetBindlessTextureIndex(NormalTexture.Loaded, data.NormalTextureIndex, data.Flags, PTMaterialFlags_UseNormalTexture);
    GetBindlessTextureIndex(TransmissionTexture.Loaded, data.TransmissionTextureIndex, data.Flags, PTMaterialFlags_UseTransmissionTexture);

    data.Flags |= (uint)(min(NestedPriority, kMaterialMaxNestedPriority)) << PTMaterialFlags_NestedPriorityShift;
    data.Flags |= (uint)(clamp(PSDDominantDeltaLobe + 1, 0, 7)) << PTMaterialFlags_PSDDominantDeltaLobeP1Shift;

    data.ShadowNoLFadeout = std::clamp(ShadowNoLFadeout, 0.0f, 0.25f);

    data._padding0 = data._padding1 = 42;
}

std::filesystem::path MaterialsBaker::GetMaterialStoragePath(PTMaterialBase& material)
{
    std::filesystem::path matPath = m_materialsPath;
    if (!material.SharedWithAllScenes)
        matPath = m_materialsSceneSpecializedPath;
    std::string fileName = material.Name + c_MaterialsExtension;
    if (material.ModelName != kNoModel)
        fileName = material.ModelName + "." + fileName;
    matPath /= fileName;
    return matPath;
}

MaterialsBaker::MaterialsBaker(const std::string & relativeShaderSourcePath, nvrhi::IDevice* device, std::shared_ptr<caustica::TextureCache> textureCache, std::shared_ptr<caustica::ShaderFactory> shaderFactory)
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
        return material.UseSpecularGlossModel;
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
    const std::shared_ptr<LoadedTexture>& loaded,
    bool enabled)
{
    if (material.EngineMaterialCounterpart == nullptr)
        return;

    switch (slot)
    {
    case PTMaterialTextureSlot::Base:
        material.EngineMaterialCounterpart->baseOrDiffuseTexture = loaded;
        material.EngineMaterialCounterpart->enableBaseOrDiffuseTexture = enabled;
        break;
    case PTMaterialTextureSlot::OcclusionRoughnessMetallic:
        material.EngineMaterialCounterpart->metalRoughOrSpecularTexture = loaded;
        material.EngineMaterialCounterpart->enableMetalRoughOrSpecularTexture = enabled;
        break;
    case PTMaterialTextureSlot::Normal:
        material.EngineMaterialCounterpart->normalTexture = loaded;
        material.EngineMaterialCounterpart->enableNormalTexture = enabled;
        break;
    case PTMaterialTextureSlot::Emissive:
        material.EngineMaterialCounterpart->emissiveTexture = loaded;
        material.EngineMaterialCounterpart->enableEmissiveTexture = enabled;
        break;
    case PTMaterialTextureSlot::Transmission:
        material.EngineMaterialCounterpart->transmissionTexture = loaded;
        material.EngineMaterialCounterpart->enableTransmissionTexture = enabled;
        break;
    default:
        assert(false);
        break;
    }
}

void MaterialsBaker::RecordTexture(const PTTexture& texture)
{
    if (texture.Loaded == nullptr)
        return;

    assert(texture.LocalPath != "");

    auto existing = m_textures.find(texture.LocalPath.generic_string());
    if (existing != m_textures.end())
    {
        if (existing->second.NormalMap != texture.NormalMap)
        {
            caustica::warning("Texture with path '%s' is used as a NormalMap and not a NormalMap - this is not supported, expect errors.", texture.LocalPath.string().c_str());
            assert(false);
        }
        if (existing->second.sRGB != texture.sRGB)
        {
            caustica::warning("Texture with path '%s' is marked as both sRGB and not sRGB in different places - this is not supported, expect errors.", texture.LocalPath.string().c_str());
            assert(false);
        }
    }
    else
    {
        m_textures.insert(std::make_pair(texture.LocalPath.generic_string(), texture));
    }
}

bool MaterialsBaker::SetMaterialTexture(
    PTMaterial& material,
    PTMaterialTextureSlot slot,
    const std::filesystem::path& localPath,
    std::optional<bool> sRGB,
    std::optional<bool> normalMap)
{
    if (localPath.empty())
    {
        ClearMaterialTexture(material, slot);
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

    PTTexture& texture = material.GetTexture(slot);
    texture.LocalPath = storagePath;
    texture.sRGB = sRGB.value_or(DefaultTextureSRGB(material, slot));
    texture.NormalMap = normalMap.value_or(DefaultTextureNormalMap(slot));
    texture.Loaded = m_textureCache->LoadTextureFromFileDeferred(fullPath, texture.sRGB);
    texture.Enabled = texture.Loaded != nullptr;

    if (texture.Loaded == nullptr ||
        (!m_textureCache->IsTextureLoaded(texture.Loaded) && !m_textureCache->IsTextureFinalized(texture.Loaded)))
    {
        texture = PTTexture();
        texture.Enabled = false;
        return false;
    }

    material.SetTextureEnabled(slot, true);
    UpdateEngineMaterialTexture(material, slot, texture.Loaded, true);
    material.GPUDataDirty = true;

    m_deferredTextureLoadInProgress = true;
    RecordTexture(texture);
    return true;
}

void MaterialsBaker::ClearMaterialTexture(PTMaterial& material, PTMaterialTextureSlot slot)
{
    PTTexture& texture = material.GetTexture(slot);
    texture = PTTexture();
    texture.Enabled = false;

    material.SetTextureEnabled(slot, false);
    UpdateEngineMaterialTexture(material, slot, nullptr, false);
    material.GPUDataDirty = true;
}

void MaterialsBaker::InitializeUniqueDeterministicName(const std::shared_ptr<PTMaterialBase> & material)
{
    std::string hashBase = material->ModelName + "_" + material->Name;
    uint evenShorterHash = ShortHash(HashMyString(hashBase));
    
    std::string uniqueName = StripNonAsciiAlnum(material->Name).substr(0, 16) + "_" + HexString(evenShorterHash);

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
    material->UniqueName = uniqueName;
}

void MaterialsBaker::Clear() 
{
    for (auto& material : m_materials)
    {
        if (material)
            material->RuntimeBaker = nullptr;
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

MaterialsBaker::~MaterialsBaker()
{
    Clear();
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

std::shared_ptr<PTMaterial> MaterialsBaker::ImportFromEngineMaterial(caustica::Material& material)
{
    std::shared_ptr<PTMaterial> materialPT = std::make_shared<PTMaterial>();

    materialPT->Name = material.name;
    materialPT->ModelName = ModelNameFromModelFileName(material.modelFileName);

    materialPT->BaseTexture.InitFromLoadedTexture(material.baseOrDiffuseTexture, true, false, m_mediaPath);

    if( material.useSpecularGlossModel ) // spec-gloss model is a special case hack where we use metalRoughOrSpecularTexture to store specular color, which is handled as sRGB
        materialPT->OcclusionRoughnessMetallicTexture.InitFromLoadedTexture(material.metalRoughOrSpecularTexture, true, false, m_mediaPath);
    else
        materialPT->OcclusionRoughnessMetallicTexture.InitFromLoadedTexture(material.metalRoughOrSpecularTexture, false, false, m_mediaPath);

    materialPT->NormalTexture.InitFromLoadedTexture(material.normalTexture, false, true, m_mediaPath);
    materialPT->EmissiveTexture.InitFromLoadedTexture(material.emissiveTexture, true, false, m_mediaPath);
    materialPT->TransmissionTexture.InitFromLoadedTexture(material.transmissionTexture, false, false, m_mediaPath);

    // Toggles for the textures. Only effective if the corresponding texture is non-null.
    materialPT->EnableBaseTexture = material.enableBaseOrDiffuseTexture;
    materialPT->EnableOcclusionRoughnessMetallicTexture = material.enableMetalRoughOrSpecularTexture;
    materialPT->EnableNormalTexture = material.enableNormalTexture;
    materialPT->EnableEmissiveTexture = material.enableEmissiveTexture;
    materialPT->EnableTransmissionTexture = material.enableTransmissionTexture;

    materialPT->BaseOrDiffuseColor = material.baseOrDiffuseColor;
    materialPT->SpecularColor = material.specularColor;
    materialPT->EmissiveColor = material.emissiveColor;

    materialPT->EmissiveIntensity = material.emissiveIntensity;
    materialPT->Metalness = material.metalness;
    materialPT->Roughness = material.roughness;
    materialPT->Opacity = material.opacity;
    materialPT->AlphaCutoff = material.alphaCutoff;
    materialPT->TransmissionFactor = material.transmissionFactor;
    //materialPT->DiffuseTransmissionFactor = material.diffuseTransmissionFactor;
    materialPT->NormalTextureScale = material.normalTextureScale;
    //materialPT->IoR = material.ior;
    materialPT->UseSpecularGlossModel = material.useSpecularGlossModel;
    materialPT->MetalnessInRedChannel = material.metalnessInRedChannel;

    materialPT->EnableAlphaTesting = (material.domain == MaterialDomain::AlphaTested || material.domain == MaterialDomain::TransmissiveAlphaTested);
    materialPT->EnableTransmission = (material.domain == MaterialDomain::Transmissive || material.domain == MaterialDomain::TransmissiveAlphaBlended || material.domain == MaterialDomain::TransmissiveAlphaTested);

    return materialPT;
}

std::shared_ptr<PTMaterial> MaterialsBaker::Load(const std::string & modelFileName, const std::string & name)
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

    std::shared_ptr<PTMaterial> materialPT = materialPT->FromJson(rootJ, m_mediaPath, m_textureCache, modelName, name, m_sceneDirectory);
    if (materialPT == nullptr)
    {
        caustica::warning("Error while parsing material file '%s'", actualLoadedFileName.c_str()); 
        return nullptr;
    }
    materialPT->SharedWithAllScenes = shared; // this property is not loaded from the file, but determined based on where the file was loaded from
    return materialPT;
}

void MaterialsBaker::SceneReloaded()
{
    Clear();
}

void MaterialsBaker::CompleteDeferredTexturesLoad(nvrhi::ICommandList* commandList)
{
    if (m_deferredTextureLoadInProgress)
    {
        if (commandList != nullptr)
        {
            commandList->close();
            m_device->executeCommandList(commandList);
        }

        // In case new textures were loaded, we need to make sure they were uploaded properly
        m_textureCache->ProcessRenderingThreadCommands(*m_commonPasses, 0.f);
        m_textureCache->LoadingFinished();
        m_deferredTextureLoadInProgress = false;

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
    if (msp.Macros.size() == 1
        && msp.Macros[0].name == "CAUSTICA_MATERIAL_PERMUTATIONS_ENABLED"
        && msp.Macros[0].definition == "0")
    {
        msp.StableShaderName = "Ubershader";
        msp.StableShaderID = -1;
        return;
    }

    const std::string stableKey = msp.ShaderFilePath + ", " + MacrosToString(msp.Macros);
    const auto hash = HashMyString(stableKey);
    const std::string hashHex = picosha2::bytes_to_hex_string(hash.begin(), hash.end());

    msp.StableShaderName = "M" + hashHex.substr(0, 16);
    const uint32_t stableID =
        (uint32_t(hash[0]) << 24) |
        (uint32_t(hash[1]) << 16) |
        (uint32_t(hash[2]) << 8) |
        uint32_t(hash[3]);
    msp.StableShaderID = int(stableID & 0x7fffffff);
}

// MaterialShaderPermutation::MaterialShaderPermutation(const std::string & shaderFilePath, const std::string & closestHitName, const std::string & anyHitName, const std::vector<std::pair<std::string, std::string>> & macros )


MaterialShaderPermutationKey::MaterialShaderPermutationKey( const MaterialShaderPermutation & msp )
    : FullKey( msp.ShaderFilePath + ", " + /*msp.ClosestHitName + ", " + msp.AnyHitName + ", " +*/ MacrosToString(msp.Macros) )
{
    Hash = ShortHash(HashMyString(FullKey));
}

void MaterialsBaker::BakeShaderPermutations()
{
    // first generate ubershader variant - that will likely go away in the future
    std::vector<caustica::ShaderMacro> macros;
    macros.push_back({ "CAUSTICA_MATERIAL_PERMUTATIONS_ENABLED", "0" });
    MaterialShaderPermutation ubershader{ .ShaderFilePath = m_relativeShaderSourcePath, .Macros = macros };
    InitializeStableShaderIdentity(ubershader);
    m_ubershader = std::make_shared<MaterialShaderPermutation>(ubershader);
    m_ubershader->UniqueMaterialName = "Ubershader";

    // now generate per-material permutations (some materials will automatically share same permutation)
    m_shaderPermutations.clear();
    m_shaderPermutationTable.clear();
    for (auto& materialPT : m_materials)
    {
        MaterialShaderPermutation variant = materialPT->ComputeShaderPermutation(m_relativeShaderSourcePath);
        InitializeStableShaderIdentity(variant);
        MaterialShaderPermutationKey key(variant);
        std::shared_ptr<MaterialShaderPermutation> ptVariant;
        auto findV = m_shaderPermutations.find(variant);
        if (findV == m_shaderPermutations.end())
        {
            ptVariant = std::make_shared<MaterialShaderPermutation>(variant);
            m_shaderPermutations.insert(std::make_pair(key, ptVariant));
            ptVariant->IndexInTable = (int)m_shaderPermutationTable.size();
            m_shaderPermutationTable.push_back(ptVariant);
        }
        else
        {
            ptVariant = findV->second;
            assert( ptVariant->Macros == variant.Macros );
        }

        std::string proposedName = materialPT->UniqueFullName();
        if (ptVariant->UniqueMaterialName == "" || (ptVariant->UniqueMaterialName.compare(proposedName)>0) )  // keep the "smallest" name out of all - still not deterministic between different scenes but goodenough!
            ptVariant->UniqueMaterialName = proposedName;

        materialPT->BakedShaderPermutation = ptVariant;
    }
}

void MaterialsBaker::CreateRenderPassesAndLoadMaterials(nvrhi::IBindingLayout* bindlessLayout, std::shared_ptr<caustica::CommonRenderPasses> commonPasses, const std::shared_ptr<caustica::Scene>& scene, const std::filesystem::path& sceneFilePath, const std::filesystem::path & mediaPath )
{
    assert(!mediaPath.empty());
    //m_bindlessLayout = bindlessLayout;
    m_commonPasses = commonPasses;

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

        std::shared_ptr<SceneGraph> sceneGraph = scene->GetSceneGraph();
        auto& materials = sceneGraph->GetMaterials();
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
                    materialEx->ptData = ImportFromEngineMaterial(*material);
                }
                else
                {
                    std::shared_ptr<PTMaterial> loaded = Load(material->modelFileName, material->name);
                    if (loaded != nullptr)
                    {
                        materialEx->ptData = loaded;
                    }
                    else // ...and if we didn't find it in our .scene.materials.json, then import from the engine material
                    {
                        std::shared_ptr<PTMaterial> materialPT = ImportFromEngineMaterial(*material);
                        materialEx->ptData = materialPT;
                        initializedFromEngineCount++;
                    }
                }
                materialEx->ptData->EngineMaterialCounterpart = materialEx.get(); // keep the link - only needed if using material animation from the engine
                materialEx->ptData->RuntimeBaker = this;

                std::string keyName = materialEx->ptData->ModelName+"."+materialEx->ptData->Name;
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
                materialEx->ptData->GPUDataIndex = uint(m_materialsGPU.size() - 1);
                materialEx->ptData->GPUDataDirty = true;
                assert(m_materialsGPU.size() <= CAUSTICA_MATERIAL_MAX_COUNT);
            }
        }

        // sort by name so when we're saving it's consistent
        std::sort(m_materials.begin(), m_materials.end(), [](const auto & a, const auto & b) { return a->Name < b->Name; } );

        if (initializedFromEngineCount > 0)
            caustica::warning("There were %d materials not found in RTXPT material materials folder '%s'; consider doing Scene->Materials->Advanced->Save", initializedFromEngineCount, m_materialsPath.string().c_str());

        m_deferredTextureLoadInProgress = true;
    }

    CompleteDeferredTexturesLoad(nullptr);

    m_uniqueNames.clear(); // must be done before InitializeUniqueDeterministicName
    for (auto& materialPT : m_materials)
    {
        RecordTexture(materialPT->BaseTexture);
        RecordTexture(materialPT->OcclusionRoughnessMetallicTexture);
        RecordTexture(materialPT->NormalTexture);
        RecordTexture(materialPT->EmissiveTexture);
        RecordTexture(materialPT->TransmissionTexture);

        InitializeUniqueDeterministicName(materialPT);
    }
    BakeShaderPermutations();
}

// NOTE: this also handles some of the geometry data and mixed geometry&material stuff - it might be a good idea to rethink whether it needs to live outside of material baker
void UpdateSubInstanceData(SubInstanceData & ret, const std::shared_ptr<caustica::Scene> & scene, const caustica::MeshInstance& meshInstance, const caustica::MeshGeometry& geometry, uint meshGeometryIndex, const PTMaterial& material)
{
    bool alphaTest = material.HasAlphaTest();

    // we need alpha texture for alpha testing to work - disable otherwise
    if (alphaTest && (!material.EnableBaseTexture || material.BaseTexture.Loaded == nullptr))
        alphaTest = false;


    float alphaCutoff = 0.0;

    const std::shared_ptr<MeshInfo>& mesh = meshInstance.GetMesh();
    ret.FlagsAndAlphaInfo = 0;
    if (alphaTest)
    {
        ret.FlagsAndAlphaInfo |= SubInstanceData::Flags_AlphaTested;

        assert(mesh->buffers->hasAttribute(VertexAttribute::TexCoord1));
        assert(material.EnableBaseTexture && material.BaseTexture.Loaded != nullptr); // disable alpha testing if this happens to be possible
        // ret.AlphaCutoff = material.alphaCutoff;
        alphaCutoff = material.AlphaCutoff;

        uint alphaTextureIndex = material.BaseTexture.Loaded->bindlessDescriptor.Get();
        assert(alphaTextureIndex < 0xFFFF);
        // ret.AlphaTextureIndex = material.BaseTexture.Loaded->bindlessDescriptor.Get();

        ret.FlagsAndAlphaInfo |= alphaTextureIndex & 0xFFFF;
    }
    ret.EmissiveLightMappingOffset = 0xFFFFFFFF;

    uint quantizedAlphaCutoff = (uint)(dm::clamp(alphaCutoff, 0.0f, 1.0f) * 255.0f + 0.5f); assert(quantizedAlphaCutoff < 256);
    ret.FlagsAndAlphaInfo |= (quantizedAlphaCutoff << SubInstanceData::Flags_AlphaOffsetOffset);

    if (material.ExcludeFromNEE)
        ret.FlagsAndAlphaInfo |= SubInstanceData::Flags_ExcludeFromNEE;

    uint globalGeometryIndex = mesh->geometries[0]->globalGeometryIndex + meshGeometryIndex;
    uint globalMaterialIndex = material.GPUDataIndex;
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

void MaterialsBaker::Update(nvrhi::ICommandList* commandList, const std::shared_ptr<caustica::Scene>& scene, std::vector<SubInstanceData>& subInstanceData)
{
    RAII_SCOPE( commandList->beginMarker("MaterialsBaker");, commandList->endMarker(); );

    CompleteDeferredTexturesLoad(commandList);

    bool needsUpload = false;
    for (auto& materialPT : m_materials)
    {
        if (materialPT->UseEngineEmissiveIntensity && materialPT->EngineMaterialCounterpart != nullptr && materialPT->EmissiveIntensity != materialPT->EngineMaterialCounterpart->emissiveIntensity)
        {
            materialPT->EmissiveIntensity = materialPT->EngineMaterialCounterpart->emissiveIntensity;
            materialPT->GPUDataDirty = true;
        }

        if (!materialPT->GPUDataDirty && !m_materialDataWasReset)
            continue;

        materialPT->FillData(m_materialsGPU[materialPT->GPUDataIndex]);
        materialPT->GPUDataDirty = false;
        needsUpload = true;
    }

    if ( needsUpload )
    {
        commandList->writeBuffer( m_materialData, m_materialsGPU.data(), m_materialsGPU.size() * sizeof(PTMaterialData), 0 );
        m_materialDataWasReset = false;
    }

    // NOTE: this also handles some of the geometry data and mixed geometry&material stuff - it might be a good idea to rethink whether it needs to live outside of material baker
    uint subInstanceIndex = 0;
    const auto& instances = scene->GetSceneGraph()->GetMeshInstances();
    for (const auto& instance : instances)
    {
        const auto& mesh = instance->GetMesh();
        uint32_t firstGeometryInstanceIndex = instance->GetGeometryInstanceIndex();
        for (size_t geometryIndex = 0; geometryIndex < mesh->geometries.size(); ++geometryIndex, subInstanceIndex++)
        {
            const auto& geometry = mesh->geometries[geometryIndex];
            assert( geometry->material != nullptr && "No handling for null materials!" );
            std::shared_ptr<PTMaterial> materialPT = PTMaterial::SafeCast(geometry->material);
            assert(materialPT != nullptr && "Unknown error - should never have happened" );

            UpdateSubInstanceData(subInstanceData[subInstanceIndex], scene, *instance, *geometry, geometryIndex, *materialPT);
        }
    }
}

/*
void MaterialsBaker::LoadAll(std::unordered_map<std::string, std::shared_ptr<PTMaterial>>& container)
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
        std::shared_ptr<PTMaterial> materialPT = materialPT->FromJson(materialJ, m_mediaPath, m_textureCache);
        if (materialPT == nullptr)
            { caustica::warning("Error while reading material in material definition file '%s'", m_sceneMaterialsFilePath.string().c_str()); continue; }
        
        auto existing = container.find(materialPT->Name);
        if (existing != container.end())
            { caustica::warning("Duplicated materials with name '%s' found in material definition file '%s' - subsequent instances ignored.", materialPT->Name.c_str(), m_sceneMaterialsFilePath.string().c_str()); assert( false ); continue; }
        else
            container.insert( make_pair(materialPT->Name, materialPT) );
    }
}*/

bool MaterialsBaker::LoadSingle(PTMaterialBase & material)
{
    std::filesystem::path inPath = GetMaterialStoragePath(material);

    Json::Value rootJ;
    if ( !caustica::json::LoadFromFile(inPath, rootJ) )
    {
        caustica::warning("No RTXPT material definition file found '%s'- consider doing Scene->Materials->Advanced->Save All", inPath.string().c_str());
        return false;
    }
    assert( material.Name != "" );
    m_deferredTextureLoadInProgress = true;
    return material.Read(rootJ, m_mediaPath, m_textureCache, m_sceneDirectory);
}

bool MaterialsBaker::SaveSingle(PTMaterialBase & material)
{
    if (!EnsureDirectoryExists(m_materialsPath))
        return false;
    std::filesystem::path outPath = m_materialsPath;
    if (!material.SharedWithAllScenes)
        outPath = m_materialsSceneSpecializedPath;
    if (!EnsureDirectoryExists(outPath))

    assert( material.ModelName != "" && material.Name != "" );
    outPath = GetMaterialStoragePath(material);

    Json::Value rootJ;
    //rootJ["RTXPTMaterialVersion"] = 1;

    material.Write(rootJ);

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

void MaterialsBaker::SaveAll()
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
        SaveSingle(*materialPT);
#endif
}

bool MaterialsBaker::DebugGUI(float indent)
{
    RAII_SCOPE(ImGui::PushID("MaterialsBakerDebugGUI"); , ImGui::PopID(); );
    
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

        ImGui::Text("Set all SharedWithAllScenes props to");
        {
            RAII_SCOPE(ImGui::Indent();, ImGui::Unindent(););
            if (ImGui::Button("Shared"))
                for (auto& materialPT : m_materials)
                    materialPT->SharedWithAllScenes = true;
            ImGui::SameLine();
            if (ImGui::Button("Not Shared"))
                for (auto& materialPT : m_materials)
                    materialPT->SharedWithAllScenes = false;
        }

        if( ImGui::Button("Save all") )
            SaveAll();
    }

    return resetAccumulation;
}

MaterialShaderPermutation PTMaterial::ComputeShaderPermutation(const std::string& defaultShaderPath)
{
    std::vector<caustica::ShaderMacro> macros;
    macros.push_back({ "CAUSTICA_MATERIAL_PERMUTATIONS_ENABLED", "1" });

    PTMaterialData data; FillData(data);

    // props.AlphaTest = material.EnableAlphaTesting;
    bool hasTransmission = this->EnableTransmission;
    bool isEmissive = this->IsEmissive();
    bool isAnalyticProxy = this->EnableAsAnalyticLightProxy;
    bool effectiveIsThinSurface = (data.Flags & PTMaterialFlags_ThinSurface) != 0; // this includes non-transmission too which forces thin surface!
    // props.NoTransmission = !props.HasTransmission;
    // props.NoTextures = (!material.EnableBaseTexture || material.BaseTexture.Loaded == nullptr)
    //     && (!material.EnableEmissiveTexture || material.EmissiveTexture.Loaded == nullptr)
    //     && (!material.EnableNormalTexture || material.NormalTexture.Loaded == nullptr)
    //     && (!material.EnableOcclusionRoughnessMetallicTexture || material.OcclusionRoughnessMetallicTexture.Loaded == nullptr)
    //     && (!material.EnableTransmissionTexture || material.TransmissionTexture.Loaded == nullptr);
    
    // macros.push_back( {"CAUSTICA_MATERIAL_EXCLUDE_FROM_NEE",   ExcludeFromNEE?"1":"0" } );


    // //why is it slower with this instead of faster?
    // macros.push_back( {"CAUSTICA_MATERIAL_USE_NORMAL_TEXTURE", ((data.Flags & PTMaterialFlags_UseNormalTexture) != 0)?"1":"0" } );

    // there's something wrong here
    // static const float kMinGGXRoughness = 0.08f; // see BxDF.hlsli, kMinGGXAlpha constant: kMinGGXRoughness must match sqrt(kMinGGXAlpha)!
    // bool onlyDeltaLobes = ((hasTransmission && this->TransmissionFactor == 1.0) || (this->Metalness == 1)) && (this->Roughness < kMinGGXRoughness) && (data.Flags & PTMaterialFlags_UseMetalRoughOrSpecularTexture) == 0;
    // macros.push_back({ "CAUSTICA_MATERIAL_ONLY_DELTA_LOBES", onlyDeltaLobes ? "1" : "0" });

#if 0 // more variants, more divergence - perf impact is largely scene dependent, sometimes it's better to specialize and sometimes not
    macros.push_back({ "CAUSTICA_MATERIAL_THIN_SURFACE", effectiveIsThinSurface ? "1" : "0" });

    macros.push_back({ "CAUSTICA_MATERIAL_HAS_TRANSMISSION", EnableTransmission ? "1" : "0" });
#endif

#if 0
    macros.push_back({ "CAUSTICA_MATERIAL_IS_EMISSIVE", isEmissive ? "1" : "0" });
    macros.push_back({ "CAUSTICA_MATERIAL_IS_ANALYTIC_LIGHT_PROXY", isAnalyticProxy ? "1" : "0" });
#else // bunch them together and only use them disable emissive & analytic paths together
    if (!isEmissive && !isAnalyticProxy)
    {
        macros.push_back({ "CAUSTICA_MATERIAL_IS_EMISSIVE", "0" });
        macros.push_back({ "CAUSTICA_MATERIAL_IS_ANALYTIC_LIGHT_PROXY", "0" });
    }
#endif 

#if 0 // NUCLEAR OPTION: make every material have its own shader - can be useful for tracking down weird perf issues
    macros.push_back({ "CAUSTICA_MATERIAL_UNIQUE_NAME", this->UniqueFullName() });
#endif

    // next:
    //     - CAUSTICA_MATERIAL_NO_ANALYTIC_LIGHT_PROXY
    //     - CAUSTICA_MATERIAL_NO_EMISSIVE             : - no need for emissive MIS code at all and a bunch of that stuff
    //     - thin surface


    return MaterialShaderPermutation{ .ShaderFilePath = defaultShaderPath, /*.ClosestHitName = "ClosestHit", .AnyHitName = "AnyHit",*/ .Macros = macros };
}

std::shared_ptr<PTMaterial> MaterialsBaker::FindByUniqueID(const std::string & name)
{
    for( int i = 0; i < m_materials.size(); i++)
        if (EqualsIgnoreCase(name, m_materials[i]->UniqueFullName()))
            return m_materials[i];

    return nullptr;
}
