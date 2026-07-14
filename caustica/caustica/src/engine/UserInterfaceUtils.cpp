#include <engine/UserInterfaceUtils.h>
#include <scene/SceneObjects.h>
#include <platform/file_dialog.h>
#include <core/log.h>
#include <core/string_utils.h>

#include <filesystem>
#include <imgui.h>

#ifndef _WIN32
#include <unistd.h>
#include <cstdio>
#include <climits>
#else
#include <Windows.h>
#include <ShlObj.h>
#define PATH_MAX MAX_PATH
#endif // _WIN32

using namespace caustica;
using namespace caustica;
using namespace caustica::math;

bool caustica::materialEditor(caustica::Material* material, bool allowMaterialDomainChanges)
{
    bool update = false;

    float itemWidth = ImGui::CalcItemWidth();

    if (allowMaterialDomainChanges)
    {
        update |= ImGui::Combo("Material Domain", (int*)&material->domain,
            "Opaque\0Alpha-tested\0Alpha-blended\0Transmissive\0"
            "Transmissive alpha-tested\0Transmissive alpha-blended\0");
    }
    else
    {
        ImGui::Text("Material Domain: %s", materialDomainToString(material->domain));
    }

    auto getShortTexturePath = [](const std::string& fullPath)
    {
        return std::filesystem::path(fullPath).filename().generic_string();
    };

    const ImVec4 filenameColor = ImVec4(0.474f, 0.722f, 0.176f, 1.0f);

    update |= ImGui::Checkbox("Double-Sided", &material->doubleSided);
    
    if (material->useSpecularGlossModel)
    {
        if (material->baseOrDiffuseTexture)
        {
            update |= ImGui::Checkbox("Use Diffuse Texture", &material->enableBaseOrDiffuseTexture);
            ImGui::SameLine();
            ImGui::TextColored(filenameColor, "%s", getShortTexturePath(material->baseOrDiffuseTexture->path).c_str());
        }

        update |= ImGui::ColorEdit3(material->enableBaseOrDiffuseTexture ? "Diffuse Factor" : "Diffuse Color", material->baseOrDiffuseColor.data(), ImGuiColorEditFlags_Float);

        if (material->metalRoughOrSpecularTexture)
        {
            update |= ImGui::Checkbox("Use Specular Texture", &material->enableMetalRoughOrSpecularTexture);
            ImGui::SameLine();
            ImGui::TextColored(filenameColor, "%s", getShortTexturePath(material->metalRoughOrSpecularTexture->path).c_str());
        }

        update |= ImGui::ColorEdit3(material->enableMetalRoughOrSpecularTexture ? "Specular Factor" : "Specular Color", material->specularColor.data(), ImGuiColorEditFlags_Float);

        float glossiness = 1.0f - material->roughness;
        update |= ImGui::SliderFloat(material->enableMetalRoughOrSpecularTexture ? "Glossiness Factor" : "Glossiness", &glossiness, 0.f, 1.f);
        material->roughness = 1.0f - glossiness;
    }
    else
    {
        if (material->baseOrDiffuseTexture)
        {
            update |= ImGui::Checkbox("Use Base Color Texture", &material->enableBaseOrDiffuseTexture);
            ImGui::SameLine();
            ImGui::TextColored(filenameColor, "%s", getShortTexturePath(material->baseOrDiffuseTexture->path).c_str());
        }

        update |= ImGui::ColorEdit3(material->enableBaseOrDiffuseTexture ? "Base Color Factor" : "Base Color", material->baseOrDiffuseColor.data(), ImGuiColorEditFlags_Float);

        if (material->metalRoughOrSpecularTexture)
        {
            update |= ImGui::Checkbox("Use Metal-Rough Texture", &material->enableMetalRoughOrSpecularTexture);
            ImGui::SameLine();
            ImGui::TextColored(filenameColor, "%s", getShortTexturePath(material->metalRoughOrSpecularTexture->path).c_str());
        }

        update |= ImGui::SliderFloat(material->enableMetalRoughOrSpecularTexture ? "Metalness Factor" : "Metalness", &material->metalness, 0.f, 1.f);
        update |= ImGui::SliderFloat(material->enableMetalRoughOrSpecularTexture ? "Roughness Factor" : "Roughness", &material->roughness, 0.f, 1.f);
    }

    if (material->domain == MaterialDomain::AlphaBlended || material->domain == MaterialDomain::TransmissiveAlphaBlended)
    {
        if (material->baseOrDiffuseTexture)
            update |= ImGui::SliderFloat("Opacity Factor", &material->opacity, 0.f, 2.f);
        else
            update |= ImGui::SliderFloat("Opacity", &material->opacity, 0.f, 1.f);
    }
    else if (material->domain == MaterialDomain::AlphaTested || material->domain == MaterialDomain::TransmissiveAlphaTested)
    {
        if (material->baseOrDiffuseTexture)
            update |= ImGui::SliderFloat("Alpha Cutoff", &material->alphaCutoff, 0.f, 1.f);
    }

    if (material->normalTexture)
    {
        update |= ImGui::Checkbox("Use Normal Texture", &material->enableNormalTexture);
        ImGui::SameLine();
        ImGui::TextColored(filenameColor, "%s", getShortTexturePath(material->normalTexture->path).c_str());
    }

    if (material->enableNormalTexture)
    {
        ImGui::SetNextItemWidth(itemWidth - 31.f);
        update |= ImGui::SliderFloat("##NormalTextureScale", &material->normalTextureScale, -2.f, 2.f);
        ImGui::SameLine(0.f, 5.f);
        ImGui::SetNextItemWidth(26.f);
        if (ImGui::Button("1.0"))
        {
            material->normalTextureScale = 1.f;
            update = true;
        }
        ImGui::SameLine();
        ImGui::Text("Normal Scale");
    }

    if (material->occlusionTexture)
    {
        update |= ImGui::Checkbox("Use Occlusion Texture", &material->enableOcclusionTexture);
        ImGui::SameLine();
        ImGui::TextColored(filenameColor, "%s", getShortTexturePath(material->occlusionTexture->path).c_str());
    }
    
    if (material->enableOcclusionTexture)
        update |= ImGui::SliderFloat("Occlusion Strength", &material->occlusionStrength, 0.f, 1.f);

    if (material->emissiveTexture)
    {
        update |= ImGui::Checkbox("Use Emissive Texture", &material->enableEmissiveTexture);
        ImGui::SameLine();
        ImGui::TextColored(filenameColor, "%s", getShortTexturePath(material->emissiveTexture->path).c_str());
    }
    
    update |= ImGui::ColorEdit3("Emissive Color", material->emissiveColor.data(), ImGuiColorEditFlags_Float);
    update |= ImGui::SliderFloat("Emissive Intensity", &material->emissiveIntensity, 0.f, 1000.f, "%.3f", ImGuiSliderFlags_Logarithmic);

    if (material->domain == MaterialDomain::Transmissive ||
        material->domain == MaterialDomain::TransmissiveAlphaTested ||
        material->domain == MaterialDomain::TransmissiveAlphaBlended)
    {
        if (material->transmissionTexture)
        {
            update |= ImGui::Checkbox("Use Transmission Texture", &material->enableTransmissionTexture);
            ImGui::SameLine();
            ImGui::TextColored(filenameColor, "%s", getShortTexturePath(material->transmissionTexture->path).c_str());
        }

        update |= ImGui::SliderFloat("Transmission Factor", &material->transmissionFactor, 0.f, 1.f);
    }

    if (material->opacityTexture)
    {
        update |= ImGui::Checkbox("Use Alpha Mask Texture", &material->enableOpacityTexture);
        ImGui::SameLine();
        ImGui::TextColored(filenameColor, "%s", getShortTexturePath(material->opacityTexture->path).c_str());
    }
    
    return update;
}

bool caustica::LightEditor_Directional(caustica::DirectionalLight& light)
{
    bool changed = false;
    double3 direction = light.getDirection();
    if (azimuthElevationSliders(direction, true))
    {
        if (ecs::isValid(light.ownerEntity))
            light.updateCachedDirection(direction);
        changed = true;
    }
    changed |= ImGui::ColorEdit3("Color", &light.color.x, ImGuiColorEditFlags_Float);
    changed |= ImGui::SliderFloat("Irradiance", &light.irradiance, 0.f, 100.f, "%.2f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::SliderFloat("Angular Size", &light.angularSize, 0.1f, 20.f);
    return changed;
}

bool caustica::LightEditor_Point(caustica::PointLight& light)
{
    bool changed = false;
    changed |= ImGui::SliderFloat("Radius", &light.radius, 0.01f, 1.f, "%.3f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::ColorEdit3("Color", &light.color.x, ImGuiColorEditFlags_Float);
    changed |= ImGui::SliderFloat("Intensity", &light.intensity, 0.f, 100.f, "%.2f", ImGuiSliderFlags_Logarithmic);
    return changed;
}

bool caustica::LightEditor_Spot(caustica::SpotLight& light)
{
    bool changed = false;
    double3 direction = light.getDirection();
    if (azimuthElevationSliders(direction, false))
    {
        if (ecs::isValid(light.ownerEntity))
            light.updateCachedDirection(direction);
        changed = true;
    }
    changed |= ImGui::SliderFloat("Radius", &light.radius, 0.01f, 1.f, "%.3f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::ColorEdit3("Color", &light.color.x, ImGuiColorEditFlags_Float);
    changed |= ImGui::SliderFloat("Intensity", &light.intensity, 0.f, 100.f, "%.2f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::SliderFloat("Inner Angle", &light.innerAngle, 0.f, 180.f);
    changed |= ImGui::SliderFloat("Outer Angle", &light.outerAngle, 0.f, 180.f);
    return changed;
}

bool caustica::lightEditor(caustica::Light& light)
{
    switch (light.getLightType())
    {
    case LightType_Directional:
        return LightEditor_Directional(static_cast<DirectionalLight&>(light));
    case LightType_Point:
        return LightEditor_Point(static_cast<PointLight&>(light));
    case LightType_Spot:
        return LightEditor_Spot(static_cast<SpotLight&>(light));
    default:
        return false;
    }
}

bool caustica::azimuthElevationSliders(dm::double3& direction, bool negative)
{
    double3 normalizedDir = normalize(direction);
    if (negative) normalizedDir = -normalizedDir;

    double azimuth = degrees(atan2(normalizedDir.z, normalizedDir.x));
    double elevation = degrees(asin(normalizedDir.y));
    const double minAzimuth = -180.0;
    const double maxAzimuth = 180.0;
    const double minElevation = -90.0;
    const double maxElevation = 90.0;

    bool changed = false;
    changed |= ImGui::SliderScalar("Azimuth", ImGuiDataType_Double, &azimuth, &minAzimuth, &maxAzimuth, "%.1f deg", ImGuiSliderFlags_NoRoundToFormat);
    changed |= ImGui::SliderScalar("Elevation", ImGuiDataType_Double, &elevation, &minElevation, &maxElevation, "%.1f deg", ImGuiSliderFlags_NoRoundToFormat);

    if (changed)
    {
        azimuth = radians(azimuth);
        elevation = radians(elevation);

        direction.y = sin(elevation);
        direction.x = cos(azimuth) * cos(elevation);
        direction.z = sin(azimuth) * cos(elevation);

        if (negative)
            direction = -direction;
    }

    return changed;
}
