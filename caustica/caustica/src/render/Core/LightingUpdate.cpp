#include <render/Core/LightingUpdate.h>
#include <render/Core/RenderCore.h>
#include <render/Core/BindingCache.h>
#include <render/Core/CommonRenderPasses.h>
#include <render/Passes/Lighting/Distant/EnvMapBaker.h>
#include <render/Passes/Lighting/Distant/EnvMapImportanceSamplingBaker.h>
#include <render/Passes/Lighting/LightsBaker.h>
#include <render/Passes/Lighting/MaterialsBaker.h>
#include <render/Passes/OMM/OmmBaker.h>
#include <render/Passes/Gaussian/GaussianSplatEmissionProxy.h>
#include <scene/Scene.h>
#include <scene/SceneGraph.h>
#include <core/scope.h>
#include <shaders/light_cb.h>
#include <shaders/SampleConstantBuffer.h>

using namespace caustica::math;

namespace caustica
{

namespace
{

bool gaussianSplatEmissionEnabled(const PathTracerSettings& settings)
{
    return settings.EnableGaussianSplats
        && settings.GaussianSplatAsEmitter
        && settings.GaussianSplatEmissionIntensity > 0.0f
        && settings.GaussianSplatEmissionMaxProxyCount > 0;
}

} // namespace

void RenderCore::preUpdateLighting(PreUpdateLightingParams& params)
{
    nvrhi::ICommandList* commandList = params.commandList;
    if (commandList == nullptr || params.envMapBaker == nullptr)
        return;

    RAII_SCOPE(commandList->beginMarker("PreUpdateLighting");, commandList->endMarker(););

    nvrhi::TextureHandle preUpdateCube = params.envMapBaker->GetEnvMapCube();
    params.envMapBaker->PreUpdate(
        commandList, params.commonPasses, params.envMapActualPath, params.sceneDirectory);

    if (preUpdateCube != params.envMapBaker->GetEnvMapCube())
        params.needNewBindings = true;
}

void RenderCore::updateLighting(UpdateLightingParams& params)
{
    nvrhi::ICommandList* commandList = params.commandList;
    if (commandList == nullptr || params.envMapBaker == nullptr || params.lightsBaker == nullptr
        || params.bindingCache == nullptr || params.lights == nullptr || !params.scene)
    {
        return;
    }

    RAII_SCOPE(commandList->beginMarker("UpdateLighting");, commandList->endMarker(););

    EMB_DirectionalLight dirLights[EnvMapBaker::c_MaxDirLights];
    uint32_t dirLightCount = 0;
    {
        const float3 rotationInRadians = radians(params.settings.EnvironmentMapParams.RotationXYZ);
        const affine3 rotationTransform = dm::rotation(rotationInRadians);
        for (const auto& light : *params.lights)
        {
            std::shared_ptr<DirectionalLight> dirLight = std::dynamic_pointer_cast<DirectionalLight>(light);
            if (dirLight == nullptr)
                continue;

            LightConstants lightConstants;
            dirLight->FillLightConstants(lightConstants);

            const float minAngularSize = PI_f / (params.envMapBaker->GetTargetCubeResolution() / 2.0f);
            assert(lightConstants.angularSizeOrInvRange >= minAngularSize);

            dirLights[dirLightCount].AngularSize =
                std::max(lightConstants.angularSizeOrInvRange, minAngularSize);
            dirLights[dirLightCount].ColorIntensity =
                float4(lightConstants.color, lightConstants.intensity);
            dirLights[dirLightCount].Direction =
                rotationTransform.transformVector(lightConstants.direction);
            dirLightCount++;
        }
    }

    if (params.envMapBaker->Update(
            commandList,
            *params.bindingCache,
            params.commonPasses,
            EnvMapBaker::BakeSettings{ .EnvMapRadianceScale = params.envMapRadianceScale },
            params.sceneTime,
            dirLights,
            dirLightCount,
            !params.settings.RealtimeMode || !params.settings.EnableAnimations))
    {
        params.settings.ResetAccumulation = true;
    }

    LightsBaker::BakeSettings settings;
    settings.ImportanceSamplingType = static_cast<uint>(params.settings.NEEType);
    settings.CameraPosition = m_camera.camera().GetPosition();
    settings.CameraDirection = m_camera.camera().GetDir();
    settings.ViewProjMatrix = m_camera.view()->GetViewProjectionMatrix();
    settings.MouseCursorPos = params.settings.MousePos;
    settings.GlobalTemporalFeedbackWeight = params.settings.NEEAT_GlobalTemporalFeedbackWeight;
    settings.LocalToGlobalSampleRatio = params.settings.ActualNEEAT_LocalToGlobalSampleRatio();
    settings.UseApproximateMIS = params.settings.ActualUseApproximateMIS();
    settings.DistantVsLocalImportanceScale = params.settings.NEEAT_Distant_vs_Local_Importance;
    settings.ResetFeedback = params.settings.ResetAccumulation && !params.settings.RealtimeMode
#if 1
        || params.settings.ResetRealtimeCaches
#endif
        ;
    settings.PrevViewportSize = float2(
        static_cast<float>(m_camera.viewPrevious()->GetViewExtent().width()),
        static_cast<float>(m_camera.viewPrevious()->GetViewExtent().height()));
    settings.ViewportSize = float2(
        static_cast<float>(m_camera.view()->GetViewExtent().width()),
        static_cast<float>(m_camera.view()->GetViewExtent().height()));
    settings.EnvMapParams = LightsBakerEnvMapParams{
        .Transform = params.envMapSceneParams.Transform,
        .InvTransform = params.envMapSceneParams.InvTransform,
        .ColorMultiplier = params.envMapSceneParams.ColorMultiplier,
        .Enabled = params.envMapSceneParams.Enabled,
    };
    settings.FrameIndex = params.frameIndex;

    if (params.gaussianSplatEmissionProxies != nullptr
        && !params.gaussianSplatEmissionProxies->empty()
        && gaussianSplatEmissionEnabled(params.settings))
    {
        settings.GaussianSplatEmissionProxies = params.gaussianSplatEmissionProxies;
        settings.GaussianSplatEmissionObjectToWorld = float4x4::identity();
        settings.GaussianSplatEmissionIntensity = params.settings.GaussianSplatEmissionIntensity;
    }

    params.lightsBaker->UpdateBegin(
        commandList,
        *params.bindingCache,
        settings,
        params.sceneTime,
        params.scene,
        params.materialsBaker,
        params.ommBaker,
        m_accelStructs.getSubInstanceBuffer(),
        m_accelStructs.getSubInstanceData(),
        params.envMapBaker->GetImportanceSampling()->GetRadianceAndImportanceMap());
}

} // namespace caustica
