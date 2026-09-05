#include "MaterialEditorGui.h"

#include "ui/ui_macros.h"

#include <render/passes/lighting/MaterialGpuCache.h>
#include <core/scope.h>
#include <imgui.h>

#include <string>

using namespace caustica::math;

namespace caustica::editor
{

bool DrawStandardMaterialEditor(StandardMaterial& material, MaterialGpuCache& cache)
{
    bool update = false;

    // Snapshot RT / hit-group / OMM inputs. Shading-only edits must not bump
    // materialStateRevision — that forces full AS + OMM + SBT rebuilds.
    const bool rtAlphaTestingBefore = material.enableAlphaTesting;
    const bool rtTransmissionBefore = material.enableTransmission;
    const bool rtExcludeFromNEEBefore = material.excludeFromNEE;
    const bool rtSkipRenderBefore = material.skipRender;
    const bool rtThinSurfaceBefore = material.thinSurface;
    const bool rtBaseTextureBefore = material.enableBaseTexture;
    const float rtAlphaCutoffBefore = material.alphaCutoff;
    const MaterialShaderPermutationKey rtMspBefore(material.computeShaderPermutation(""));

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
            UI_SCOPED_DISABLE(!material.enableTransmission);
            update |= ImGui::Checkbox("Thin surface", &material.thinSurface);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Material has no volumetric properties - used for double sided thin surfaces like leafs.\nNon-transparent materials are always considered thin surface.");

        update |= ImGui::Checkbox("Ignore by NEE shadow ray (bias!)", &material.excludeFromNEE);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ignored for shadow rays during Next Event Estimation\nNote: this isn't physically correct - it adds bias!");

        update |= ImGui::SliderFloat("Shadow NoL Fadeout", &material.shadowNoLFadeout, 0.0f, 0.2f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Low tessellation geometry often has triangle (flat) normals that differ significantly from shading normals. \n"
            "This causes shading vs shadow discrepancy that exposes triangle edges. One way to mitigate this (other than \n"
            "having more detailed mesh) is to add additional shadowing falloff to hide the seam. This setting is not \n"
            "physically correct and adds bias. Setting of 0 means no fadeout (default).");

        update |= ImGui::Checkbox("Unlit, receive shadows", &material.unlitReceiveShadows);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Display the material's base color without BRDF, direct, or indirect lighting,\n"
            "but keep shadows from sampled lights.");
        {
            UI_SCOPED_DISABLE(!material.unlitReceiveShadows);
            update |= ImGui::SliderFloat("Unlit shadow strength", &material.unlitShadowStrength, 0.0f, 1.0f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Artistic control for how strongly sampled-light visibility darkens the unlit color.\n"
                "0 keeps the base color fully visible; 1 applies the full shadow mask.");
        }

        update |= ImGui::Checkbox("Enable as analytic light proxy", &material.enableAsAnalyticLightProxy);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Any scene object with this material will look at it's parent node in the scenegraph; if the parent contains\n"
            "an analytic light, the rays falling of this surface will also output radiance from the analytic light.\n"
            "The more closely the object's mesh resembles the analytic light, the more physically correct results will be.\n");

        update |= ImGui::Checkbox("Emissive intensity from engine material", &material.useEngineEmissiveIntensity);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Engine materials can have emissive intensity animation attached.\n");

        update |= ImGui::Checkbox("Ignore mesh tangent space", &material.ignoreMeshTangentSpace);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("This will ignore tangent space loaded from the mesh and generate new one - can help issues with normals.");

        update |= ImGui::Checkbox("Skip render", &material.skipRender);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ignore all meshes with this material - sometimes easier than removing the object itself.\nNote: this will not remove it as an emissive light on the light sampling side!");

        std::string fullName = material.uniqueFullName();
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

    drawTextureToggle("Use base_color texture", material.baseTexture, material.enableBaseTexture);

    update |= ImGui::ColorEdit3(material.enableBaseTexture ? "base_color factor" : "base_color", material.baseOrDiffuseColor.data(), ImGuiColorEditFlags_Float);
    update |= ImGui::SliderFloat("base_weight", &material.baseWeight, 0.f, 1.f);
    update |= ImGui::SliderFloat("base_diffuse_roughness", &material.baseDiffuseRoughness, 0.f, 1.f);

    drawTextureToggle("Use base_metalness/specular_roughness texture", material.occlusionRoughnessMetallicTexture, material.enableOcclusionRoughnessMetallicTexture);

    update |= ImGui::SliderFloat(material.enableOcclusionRoughnessMetallicTexture ? "base_metalness factor" : "base_metalness", &material.metalness, 0.f, 1.f);
    update |= ImGui::SliderFloat("specular_weight", &material.specularWeight, 0.f, 2.f);
    update |= ImGui::ColorEdit3("specular_color", material.specularColor.data(), ImGuiColorEditFlags_Float);
    update |= ImGui::SliderFloat(material.enableOcclusionRoughnessMetallicTexture ? "specular_roughness factor" : "specular_roughness", &material.roughness, 0.f, 1.f);
    update |= ImGui::SliderFloat("specular_roughness_anisotropy", &material.anisotropy, -1.f, 1.f);
    update |= ImGui::InputFloat("specular_ior", &material.IoR);
    if (material.IoR < 1.0f) { material.IoR = 1.0f; update = true; }

    update |= ImGui::SliderFloat("fuzz_weight", &material.fuzzWeight, 0.f, 1.f);
    update |= ImGui::ColorEdit3("fuzz_color", material.fuzzColor.data(), ImGuiColorEditFlags_Float);
    update |= ImGui::SliderFloat("fuzz_roughness", &material.fuzzRoughness, 0.f, 1.f);

    if (ImGui::CollapsingHeader("Coat", ImGuiTreeNodeFlags_DefaultOpen))
    {
        update |= ImGui::SliderFloat("coat_weight", &material.coatWeight, 0.f, 1.f);
        update |= ImGui::ColorEdit3("coat_color", material.coatColor.data(), ImGuiColorEditFlags_Float);
        update |= ImGui::SliderFloat("coat_roughness", &material.coatRoughness, 0.f, 1.f);
        update |= ImGui::SliderFloat("coat_roughness_anisotropy", &material.coatAnisotropy, -1.f, 1.f);
        update |= ImGui::InputFloat("coat_ior", &material.coatIor);
        if (material.coatIor < 1.0f) { material.coatIor = 1.0f; update = true; }
        update |= ImGui::SliderFloat("coat_darkening", &material.coatDarkening, 0.f, 1.f);
    }

    if (ImGui::CollapsingHeader("Subsurface"))
    {
        update |= ImGui::SliderFloat("subsurface_weight", &material.subsurfaceWeight, 0.f, 1.f);
        update |= ImGui::ColorEdit3("subsurface_color", material.subsurfaceColor.data(), ImGuiColorEditFlags_Float);
        update |= ImGui::InputFloat("subsurface_radius", &material.subsurfaceRadius);
        if (material.subsurfaceRadius < 0.0f) { material.subsurfaceRadius = 0.0f; update = true; }
        update |= ImGui::InputFloat3("subsurface_radius_scale", material.subsurfaceRadiusScale.data());
        material.subsurfaceRadiusScale = math::max(material.subsurfaceRadiusScale, math::float3(0.0f));
        update |= ImGui::SliderFloat("subsurface_scatter_anisotropy", &material.subsurfaceAnisotropy, -1.f, 1.f);
    }

    if (ImGui::CollapsingHeader("Hair"))
    {
        update |= ImGui::Checkbox("Enable hair BCSDF", &material.enableHair);
        UI_SCOPED_DISABLE(!material.enableHair);

        int hairModel = static_cast<int>(material.hair.model);
        if (ImGui::Combo("Hair model", &hairModel, "Far-Field BCSDF\0Chiang BCSDF\0\0"))
        {
            material.hair.model = static_cast<caustica::Material::HairParams::Model>(hairModel);
            update = true;
        }
        update |= ImGui::ColorEdit3("Hair base color", material.hair.baseColor.data(), ImGuiColorEditFlags_Float);
        update |= ImGui::SliderFloat("Hair melanin", &material.hair.melanin, 0.f, 1.f);
        update |= ImGui::SliderFloat("Hair melanin redness", &material.hair.melaninRedness, 0.f, 1.f);
        update |= ImGui::SliderFloat("Hair longitudinal roughness", &material.hair.longitudinalRoughness, 0.001f, 1.f);
        update |= ImGui::SliderFloat("Hair azimuthal roughness", &material.hair.azimuthalRoughness, 0.001f, 1.f);
        update |= ImGui::SliderFloat("Hair IOR", &material.hair.ior, 1.001f, 2.f);
        update |= ImGui::SliderFloat("Hair cuticle angle", &material.hair.cuticleAngle, -15.f, 15.f);

        const bool farField = material.hair.model == caustica::Material::HairParams::Model::FarField;
        UI_SCOPED_DISABLE(!farField);
        update |= ImGui::SliderFloat("Hair diffuse weight", &material.hair.diffuseReflectionWeight, 0.f, 1.f);
        update |= ImGui::ColorEdit3("Hair diffuse tint", material.hair.diffuseReflectionTint.data(), ImGuiColorEditFlags_Float);
    }

    if (ImGui::CollapsingHeader("Thin Film"))
    {
        update |= ImGui::SliderFloat("thin_film_weight", &material.thinFilmWeight, 0.f, 1.f);
        update |= ImGui::SliderFloat("thin_film_thickness", &material.thinFilmThickness, 0.f, 3.f);
        update |= ImGui::InputFloat("thin_film_ior", &material.thinFilmIor);
        if (material.thinFilmIor < 1.0f) { material.thinFilmIor = 1.0f; update = true; }
    }

    update |= ImGui::SliderFloat("geometry_opacity", &material.opacity, 0.f, 1.f);
    update |= ImGui::Checkbox("geometry_thin_walled", &material.thinSurface);

    drawTextureToggle("Use transmission_weight texture", material.transmissionTexture, material.enableTransmissionTexture);

    float previousTransmissionFactor = material.transmissionFactor;
    float previousDiffuseTransmissionFactor = material.diffuseTransmissionFactor;
    update |= ImGui::SliderFloat("transmission_weight", &material.transmissionFactor, 0.f, 1.f);
    update |= ImGui::SliderFloat("transmission_diffuse_weight", &material.diffuseTransmissionFactor, 0.f, 1.f);
    update |= ImGui::ColorEdit3("transmission_color", material.transmissionColor.data(), ImGuiColorEditFlags_Float);
    update |= ImGui::InputFloat("transmission_depth", &material.transmissionDepth);
    if (material.transmissionDepth < 0.0f) { material.transmissionDepth = 0.0f; update = true; }
    update |= ImGui::ColorEdit3("transmission_scatter", material.transmissionScatter.data(), ImGuiColorEditFlags_Float);
    update |= ImGui::SliderFloat("transmission_scatter_anisotropy", &material.transmissionScatterAnisotropy, -1.f, 1.f);
    update |= ImGui::SliderFloat("transmission_dispersion_scale", &material.transmissionDispersionScale, 0.f, 1.f);
    update |= ImGui::InputFloat("transmission_dispersion_abbe_number", &material.transmissionDispersionAbbeNumber);
    if (material.transmissionDispersionAbbeNumber < 0.0f) { material.transmissionDispersionAbbeNumber = 0.0f; update = true; }

    bool openPBRTransmissionEnabled = (material.transmissionFactor > 0.f) || (material.diffuseTransmissionFactor > 0.f);
    if (openPBRTransmissionEnabled != material.enableTransmission)
    {
        material.enableTransmission = openPBRTransmissionEnabled;
        update = true;
    }
    if (previousTransmissionFactor != material.transmissionFactor || previousDiffuseTransmissionFactor != material.diffuseTransmissionFactor)
        material.enableTransmission = openPBRTransmissionEnabled;

    if (material.enableTransmission && !material.thinSurface)
    {
        update |= ImGui::InputFloat("volume_attenuation_distance", &material.volumeAttenuationDistance);
        if (material.volumeAttenuationDistance < 0.0f) { material.volumeAttenuationDistance = 0.0f; update = true; }
        update |= ImGui::ColorEdit3("volume_attenuation_color", material.volumeAttenuationColor.data(), ImGuiColorEditFlags_Float);
        update |= ImGui::InputInt("nested_priority", &material.nestedPriority);
        if (material.nestedPriority < 0 || material.nestedPriority > 14) { material.nestedPriority = math::clamp(material.nestedPriority, 0, 14); update = true; }
    }

    update |= ImGui::Checkbox("geometry_enable_alpha_test", &material.enableAlphaTesting);

    if (material.enableAlphaTesting && material.baseTexture.loaded)
    {
        update |= ImGui::SliderFloat("geometry_alpha_cutoff", &material.alphaCutoff, 0.f, 1.f);
    }

    drawTextureToggle("Use geometry_normal texture", material.normalTexture, material.enableNormalTexture);

    if (material.enableNormalTexture)
    {
        ImGui::SetNextItemWidth(itemWidth - 31.f);
        update |= ImGui::SliderFloat("###normtexscale", &material.normalTextureScale, -2.f, 2.f);
        ImGui::SameLine(0.f, 5.f);
        ImGui::SetNextItemWidth(26.f);
        if (ImGui::Button("1.0"))
        {
            material.normalTextureScale = 1.f;
            update = true;
        }
        ImGui::SameLine();
        ImGui::Text("geometry_normal_scale");
    }

    drawTextureToggle("Use coat_normal texture", material.coatNormalTexture, material.enableCoatNormalTexture);
    if (material.enableCoatNormalTexture)
    {
        ImGui::SetNextItemWidth(itemWidth - 31.f);
        update |= ImGui::SliderFloat("###coatnormtexscale", &material.coatNormalTextureScale, -2.f, 2.f);
        ImGui::SameLine(0.f, 5.f);
        ImGui::SetNextItemWidth(26.f);
        if (ImGui::Button("1.0##coat_normal"))
        {
            material.coatNormalTextureScale = 1.f;
            update = true;
        }
        ImGui::SameLine();
        ImGui::Text("coat_normal_scale");
    }

    drawTextureToggle("Use emission_color texture", material.emissiveTexture, material.enableEmissiveTexture);

    update |= ImGui::ColorEdit3("emission_color", material.emissiveColor.data(), ImGuiColorEditFlags_Float);
    update |= ImGui::SliderFloat("emission_luminance", &material.emissiveIntensity, 0.f, 100000.f, "%.3f", ImGuiSliderFlags_Logarithmic);

    if (ImGui::CollapsingHeader("Path Decomposition"))
    {
        ImGui::Indent();

        update |= ImGui::Combo("Block mv-s at surface", (int*)&material.psdBlockMotionVectorsAtSurfaceType, "Off\0AutoLow\0AutoHigh\0Full\0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Curved surfaces cause motion vectors on reflected or transmitted\nsegments to be incorrect and are better disabled.\n"
            "When this is enabled, motion for all de-composited paths will come\nfrom this surface. AutoLow and AutoHigh will attempt to set the flag based on\n surface curvature (with Low and High sensitivities).");

        bool psdEnable = !material.psdExclude; // makes more sense from UI perspective - avoids double negative
        update |= ImGui::Checkbox("Enable delta lobe decomposition", &psdEnable);
        material.psdExclude = !psdEnable;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Some materials/meshes look best without decomposition.");
        
        {
            UI_SCOPED_DISABLE(material.psdExclude);
            int dominantDeltaLobeP1 = math::clamp(material.psdDominantDeltaLobe, -1, 2) + 1;
            update |= ImGui::Combo("Dominant bounce", &dominantDeltaLobeP1, "None (surface)\0Transparency\0Reflection\0Coat\0\0");
            material.psdDominantDeltaLobe = math::clamp(dominantDeltaLobeP1 - 1, -1, 2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Determines which surface will:\n * provide motion vectors for denoising\n * get ReSTIR DI lighting\n * get 'boost samples' for NEE lighting");
        }
        ImGui::Unindent();
    }

    if (ImGui::CollapsingHeader("Save/load"))
    {
        RAII_SCOPE( ImGui::Indent();, ImGui::Unindent(); );

        ImGui::Checkbox("Share with all scenes", &material.sharedWithAllScenes);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("if checked, material saved to /Assets/materials/ and \nshared between all scenes; otherwise it is saved under \n/Assets/materials/<scene-stem>/ for the current scene");

        auto matPath = cache.getMaterialStoragePath(material);

        ImGui::TextWrapped("File name: %s", matPath.string().c_str());

        if (ImGui::Button("load"))
        {
            cache.loadSingle(material);
            update = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Save"))
            cache.saveSingle(material);
    }

    // mark for update
    material.gpuDataDirty |= update;
    if (update)
    {
        const MaterialShaderPermutationKey rtMspAfter(material.computeShaderPermutation(""));
        const bool rayTracingStateChanged =
            rtAlphaTestingBefore != material.enableAlphaTesting
            || rtTransmissionBefore != material.enableTransmission
            || rtExcludeFromNEEBefore != material.excludeFromNEE
            || rtSkipRenderBefore != material.skipRender
            || rtThinSurfaceBefore != material.thinSurface
            || rtBaseTextureBefore != material.enableBaseTexture
            || rtAlphaCutoffBefore != material.alphaCutoff
            || !(rtMspBefore == rtMspAfter);
        // Color/roughness/metalness etc. only need the material CB upload above.
        if (rayTracingStateChanged)
            cache.notifyMaterialEdited();
    }

    return update;
}

} // namespace caustica::editor
