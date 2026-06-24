#include "SampleCommon/Renderer.h"
#include "PathTracerApp.h"
#include <render/Core/CameraController.h>
#include <render/Passes/PostProcess/ToneMappingPasses.h>
#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>

#include <scene/Scene.h>
#include <scene/SceneGraph.h>
#include <scene/camera/Camera.h>

#include <fstream>
#include <limits>

using namespace caustica;

Renderer::Renderer(PathTracerApp& owner) : m_owner(owner) {}
Renderer::~Renderer() = default;

caustica::CameraUpdateParams Renderer::makeCameraUpdateParams() const
{
    auto& s = m_owner;
    CameraUpdateParams params;
    params.renderSize = s.GetRenderSize();
    params.displayAspectRatio = s.m_displayAspectRatio;
    params.sampleIndex = s.m_sampleIndex;
    params.frameIndex = s.m_frameIndex;
    params.realtimeMode = s.m_settings.RealtimeMode;
    params.realtimeAA = s.m_settings.RealtimeAA;
    params.dbgFreezeRealtimeNoiseSeed = s.m_settings.DbgFreezeRealtimeNoiseSeed;
    params.temporalAAJitter = s.m_settings.TemporalAntiAliasingJitter;
    params.temporalAAPass = s.m_temporalAntiAliasingPass.get();
    return params;
}

// =============================================================================
// Camera
// =============================================================================

dm::float2 Renderer::computeCameraJitter(uint32_t frameIndex) const
{
    (void)frameIndex;
    return m_owner.m_renderCore.camera().computeJitter(makeCameraUpdateParams());
}

void Renderer::updateCameraFromScene(const std::shared_ptr<PerspectiveCamera>& sceneCamera)
{
    m_owner.m_renderCore.camera().updateFromSceneCamera(sceneCamera);

    auto sceneCameraEx = std::dynamic_pointer_cast<PerspectiveCamera>(sceneCamera);
    if (sceneCameraEx != nullptr)
    {
        ToneMappingParameters defaults;
        m_owner.m_settings.ToneMappingParams.autoExposure =
            sceneCameraEx->enableAutoExposure.value_or(defaults.autoExposure);
        m_owner.m_settings.ToneMappingParams.exposureCompensation =
            sceneCameraEx->exposureCompensation.value_or(defaults.exposureCompensation);
        m_owner.m_settings.ToneMappingParams.exposureValue =
            sceneCameraEx->exposureValue.value_or(defaults.exposureValue);
        m_owner.m_settings.ToneMappingParams.exposureValueMin =
            sceneCameraEx->exposureValueMin.value_or(defaults.exposureValueMin);
        m_owner.m_settings.ToneMappingParams.exposureValueMax =
            sceneCameraEx->exposureValueMax.value_or(defaults.exposureValueMax);
    }
}

void Renderer::updateViews(nvrhi::IFramebuffer* framebuffer)
{
    (void)framebuffer;
    m_owner.m_renderCore.camera().updateViews(makeCameraUpdateParams());
}

void Renderer::saveCurrentCamera() const
{
    dm::float4x4 projMatrix = m_owner.m_renderCore.camera().view()->GetProjectionMatrix();
    float tanHalfFOVY = 1.0f / (projMatrix.m_data[1 * 4 + 1]);
    float fovY = atanf(tanHalfFOVY) * 2.0f;

    m_owner.m_renderCore.camera().saveToFile(
        GetDirectoryWithExecutable() / "campos.txt",
        m_owner.m_renderCore.camera().zNear(),
        fovY);
}

void Renderer::loadCurrentCamera()
{
    m_owner.m_renderCore.camera().loadFromFile(GetDirectoryWithExecutable() / "campos.txt");
}

std::string Renderer::getCurrentCameraPosDirUp() const
{
    return m_owner.m_renderCore.camera().getPosDirUpString();
}

bool Renderer::setCurrentCameraPosDirUp(const std::string& val)
{
    return m_owner.m_renderCore.camera().setFromPosDirUpString(val);
}

void Renderer::setCameraVerticalFOV(float fov)
{
    m_owner.m_renderCore.camera().setVerticalFOV(fov);
    m_owner.m_settings.ResetAccumulation = true;
    m_owner.m_gaussianSplatTemporalReset = true;
}

void Renderer::setCameraIntrinsics(float fx, float fy, float cx, float cy, float w, float h)
{
    m_owner.m_renderCore.camera().setIntrinsics(fx, fy, cx, cy, w, h);
    m_owner.m_settings.ResetAccumulation = true;
    m_owner.m_gaussianSplatTemporalReset = true;
}

void Renderer::clearCameraIntrinsics()
{
    m_owner.m_renderCore.camera().clearIntrinsics();
    m_owner.m_settings.ResetAccumulation = true;
    m_owner.m_gaussianSplatTemporalReset = true;
}

// =============================================================================
// Acceleration structures
// =============================================================================

void Renderer::transitionMeshBuffersToReadOnly()
{
    auto& s = m_owner;
    for (const auto& skinnedInstance : s.GetScene()->GetSceneGraph()->GetSkinnedMeshInstances())
        s.m_commandList->setBufferState(skinnedInstance->GetMesh()->buffers->vertexBuffer,
            nvrhi::ResourceStates::ShaderResource);
    s.m_commandList->commitBarriers();
}

// =============================================================================
// Utilities
// =============================================================================

std::string Renderer::getResolutionInfo() const
{
    auto& s = m_owner;
    if (s.m_renderTargets == nullptr || s.m_renderTargets->OutputColor == nullptr)
        return "uninitialized";
    if (dm::all(s.m_renderSize == s.m_displaySize))
        return std::to_string(s.m_renderSize.x) + "x" + std::to_string(s.m_renderSize.y);
    else
        return std::to_string(s.m_renderSize.x) + "x" + std::to_string(s.m_renderSize.y)
            + "->" + std::to_string(s.m_displaySize.x) + "x" + std::to_string(s.m_displaySize.y);
}

float Renderer::getAvgTimePerFrame() const
{
    auto& s = m_owner;
    if (s.m_benchFrames == 0) return 0.0f;
    std::chrono::duration<double> elapsed = (s.m_benchLast - s.m_benchStart);
    return float(elapsed.count() / s.m_benchFrames);
}

void Renderer::debugDrawLine(dm::float3 start, dm::float3 stop, dm::float4 col1, dm::float4 col2)
{
    auto& s = m_owner;
    if (int(s.m_cpuSideDebugLines.size()) + 2 >= MAX_DEBUG_LINES) return;
    DebugLineStruct dls = { dm::float4(start, 1), col1 };
    DebugLineStruct dle = { dm::float4(stop, 1), col2 };
    s.m_cpuSideDebugLines.push_back(dls);
    s.m_cpuSideDebugLines.push_back(dle);
}
