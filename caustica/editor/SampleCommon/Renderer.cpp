#include "SampleCommon/Renderer.h"
#include "caustica.h"
#include "SampleCommon/SampleCommon.h"

#include <scene/Scene.h>
#include <scene/SceneGraph.h>
#include <scene/camera/Camera.h>
#include <render/Core/View.h>
#include <core/path_utils.h>

#include <fstream>
#include <limits>

using namespace caustica;

namespace
{
    // Moved from caustica.cpp anonymous namespace
    dm::float4x4 MakePinholeIntrinsicsProjection(
        float fx, float fy, float cx, float cy, float width, float height, float zNear)
    {
        width = std::max(width, 1.0f);
        height = std::max(height, 1.0f);
        const float xScale = 2.0f * fx / width;
        const float yScale = 2.0f * fy / height;
        const float xOffset = 2.0f * cx / width - 1.0f;
        const float yOffset = 1.0f - 2.0f * cy / height;
        return dm::float4x4(
            xScale, 0.0f, 0.0f, 0.0f,
            0.0f, yScale, 0.0f, 0.0f,
            xOffset, yOffset, 0.0f, 1.0f,
            0.0f, 0.0f, zNear, 0.0f);
    }
}

Renderer::Renderer(Sample& owner) : m_owner(owner) {}
Renderer::~Renderer() = default;

// =============================================================================
// Camera
// =============================================================================

dm::float2 Renderer::computeCameraJitter(uint32_t frameIndex) const
{
    auto& s = m_owner;
    if (!s.m_ui.RealtimeMode || s.m_ui.RealtimeAA == 0 ||
        s.m_temporalAntiAliasingPass == nullptr || s.m_ui.DbgFreezeRealtimeNoiseSeed)
        return dm::float2(0, 0);
    return s.m_temporalAntiAliasingPass->GetCurrentPixelOffset();
}

void Renderer::updateCameraFromScene(const std::shared_ptr<PerspectiveCamera>& sceneCamera)
{
    auto& s = m_owner;
    dm::affine3 viewToWorld = sceneCamera->GetViewToWorldMatrix();
    dm::float3 cameraPos = viewToWorld.m_translation;
    s.m_camera.LookAt(cameraPos, cameraPos + viewToWorld.m_linear.row2, viewToWorld.m_linear.row1);
    s.m_cameraVerticalFOV = sceneCamera->verticalFov;
    s.m_cameraZNear = sceneCamera->zNear;

    auto sceneCameraEx = std::dynamic_pointer_cast<caustica::PerspectiveCamera>(sceneCamera);
    if (sceneCameraEx != nullptr)
    {
        ToneMappingParameters defaults;
        s.m_ui.ToneMappingParams.autoExposure = sceneCameraEx->enableAutoExposure.value_or(defaults.autoExposure);
        s.m_ui.ToneMappingParams.exposureCompensation = sceneCameraEx->exposureCompensation.value_or(defaults.exposureCompensation);
        s.m_ui.ToneMappingParams.exposureValue = sceneCameraEx->exposureValue.value_or(defaults.exposureValue);
        s.m_ui.ToneMappingParams.exposureValueMin = sceneCameraEx->exposureValueMin.value_or(defaults.exposureValueMin);
        s.m_ui.ToneMappingParams.exposureValueMax = sceneCameraEx->exposureValueMax.value_or(defaults.exposureValueMax);
    }
}

void Renderer::updateViews(nvrhi::IFramebuffer* framebuffer)
{
    auto& s = m_owner;
    if (s.m_temporalAntiAliasingPass)
        s.m_temporalAntiAliasingPass->SetJitter(s.m_ui.TemporalAntiAliasingJitter);

    nvrhi::Viewport windowViewport(float(s.m_renderSize.x), float(s.m_renderSize.y));
    s.m_view->SetViewport(windowViewport);
    float outputAspectRatio = s.m_displayAspectRatio;
    const dm::float4x4 projection = s.m_cameraUseCustomIntrinsics
        ? MakePinholeIntrinsicsProjection(
            s.m_cameraIntrinsics.x, s.m_cameraIntrinsics.y,
            s.m_cameraIntrinsics.z, s.m_cameraIntrinsics.w,
            s.m_cameraIntrinsicsViewport.x, s.m_cameraIntrinsicsViewport.y,
            s.m_cameraZNear)
        : dm::perspProjD3DStyleReverse(s.m_cameraVerticalFOV, outputAspectRatio, s.m_cameraZNear);
    s.m_view->SetMatrices(s.m_camera.GetWorldToViewMatrix(), projection);
    s.m_view->SetPixelOffset(computeCameraJitter(s.m_sampleIndex));
    s.m_view->UpdateCache();
    if ((s.m_frameIndex & 0xFFFFFFFF) == 0)
    {
        s.m_viewPrevious->SetMatrices(s.m_view->GetViewMatrix(), s.m_view->GetProjectionMatrix());
        s.m_viewPrevious->SetPixelOffset(s.m_view->GetPixelOffset());
        s.m_viewPrevious->UpdateCache();
    }
}

void Renderer::saveCurrentCamera() const
{
    auto& s = m_owner;
    dm::float3 worldPos = s.m_camera.GetPosition();
    dm::float3 worldDir = s.m_camera.GetDir();
    dm::float3 worldUp = s.m_camera.GetUp();
    dm::dquat rotation;
    dm::affine3 sceneWorldToView = dm::scaling(dm::float3(1.f, 1.f, -1.f)) *
        dm::inverse(s.m_camera.GetWorldToViewMatrix());
    dm::decomposeAffine<double>(dm::daffine3(sceneWorldToView), nullptr, &rotation, nullptr);

    dm::float4x4 projMatrix = s.m_view->GetProjectionMatrix();
    float tanHalfFOVY = 1.0f / (projMatrix.m_data[1 * 4 + 1]);
    float fovY = atanf(tanHalfFOVY) * 2.0f;

    bool autoExposure = s.m_ui.ToneMappingParams.autoExposure;
    float exposureCompensation = s.m_ui.ToneMappingParams.exposureCompensation;
    float exposureValue = s.m_ui.ToneMappingParams.exposureValue;

    std::ofstream file;
    file.open(caustica::GetDirectoryWithExecutable() / "campos.txt", std::ios_base::out | std::ios_base::trunc);
    if (file.is_open())
    {
        file << worldPos.x << " " << worldPos.y << " " << worldPos.z << " " << std::endl;
        file << worldDir.x << " " << worldDir.y << " " << worldDir.z << " " << std::endl;
        file << worldUp.x << " " << worldUp.y << " " << worldUp.z << " " << std::endl;
        file << std::endl;
        file << "below is the camera node that can be included into the *.scene.json;" << std::endl;
        file << "'Cameras' node goes into 'Graph' array" << std::endl;
        file << std::endl;
        file << "{" << std::endl;
        file << "    \"name\": \"Cameras\"," << std::endl;
        file << "        \"children\" : [" << std::endl;
        file << "    {" << std::endl;
        file << "        \"name\": \"Default\"," << std::endl;
        file << "        \"type\" : \"PerspectiveCameraEx\"," << std::endl;
        file << "        \"translation\" : [" << std::to_string(worldPos.x) << ", "
            << std::to_string(worldPos.y) << ", " << std::to_string(worldPos.z) << "]," << std::endl;
        file << "        \"rotation\" : [" << std::to_string(rotation.x) << ", "
            << std::to_string(rotation.y) << ", " << std::to_string(rotation.z) << ", "
            << std::to_string(rotation.w) << "]," << std::endl;
        file << "        \"verticalFov\" : " << std::to_string(fovY) << "," << std::endl;
        file << "        \"zNear\" : " << std::to_string(s.m_cameraZNear) << "," << std::endl;
        file << "        \"enableAutoExposure\" : " << (autoExposure ? "true" : "false") << "," << std::endl;
        file << "        \"exposureCompensation\" : " << std::to_string(exposureCompensation) << "," << std::endl;
        file << "        \"exposureValue\" : " << std::to_string(exposureValue) << std::endl;
        file << "    }" << std::endl;
        file << "        ]" << std::endl;
        file << "}," << std::endl;
        file.close();
    }
}

void Renderer::loadCurrentCamera()
{
    auto& s = m_owner;
    dm::float3 worldPos, worldDir, worldUp;
    std::ifstream file;
    file.open(caustica::GetDirectoryWithExecutable() / "campos.txt", std::ios_base::in);
    if (file.is_open())
    {
        file >> worldPos.x >> std::ws >> worldPos.y >> std::ws >> worldPos.z;
        file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        file >> worldDir.x >> std::ws >> worldDir.y >> std::ws >> worldDir.z;
        file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        file >> worldUp.x >> std::ws >> worldUp.y >> std::ws >> worldUp.z;
        file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        file.close();
        s.m_camera.LookAt(worldPos, worldPos + worldDir, worldUp);
    }
}

std::string Renderer::getCurrentCameraPosDirUp() const
{
    auto& s = m_owner;
    auto toString = [](const dm::float3& val) {
        return std::to_string(val.x) + "," + std::to_string(val.y) + "," + std::to_string(val.z);
    };
    return toString(s.m_camera.GetPosition()) + "," +
           toString(s.m_camera.GetDir()) + "," +
           toString(s.m_camera.GetUp());
}

bool Renderer::setCurrentCameraPosDirUp(const std::string& val)
{
    auto& s = m_owner;
    if (val == "") return false;
    bool ok = true;
    dm::float3 worldPos, worldDir, worldUp;
    std::string temp = val;
    ok &= ParseFloat3Consume(temp, worldPos);
    ok &= ParseFloat3Consume(temp, worldDir);
    ok &= ParseFloat3Consume(temp, worldUp);
    if (ok)
        s.m_camera.LookAt(worldPos, worldPos + worldDir, worldUp);
    return ok;
}

void Renderer::setCameraVerticalFOV(float fov)
{
    auto& s = m_owner;
    s.m_cameraVerticalFOV = fov;
    s.m_cameraUseCustomIntrinsics = false;
    s.m_ui.ResetAccumulation = true;
    s.m_gaussianSplatTemporalReset = true;
}

void Renderer::setCameraIntrinsics(float fx, float fy, float cx, float cy, float w, float h)
{
    auto& s = m_owner;
    if (fx <= 0.0f || fy <= 0.0f || w <= 0.0f || h <= 0.0f)
        return;
    s.m_cameraIntrinsics = dm::float4(fx, fy, cx, cy);
    s.m_cameraIntrinsicsViewport = dm::float2(w, h);
    s.m_cameraVerticalFOV = 2.0f * std::atan(h / (2.0f * fy));
    s.m_cameraUseCustomIntrinsics = true;
    s.m_ui.ResetAccumulation = true;
    s.m_gaussianSplatTemporalReset = true;
}

void Renderer::clearCameraIntrinsics()
{
    auto& s = m_owner;
    s.m_cameraUseCustomIntrinsics = false;
    s.m_ui.ResetAccumulation = true;
    s.m_gaussianSplatTemporalReset = true;
}

// =============================================================================
// Acceleration structures
// =============================================================================

void Renderer::transitionMeshBuffersToReadOnly()
{
    auto& s = m_owner;
    for (const auto& skinnedInstance : s.m_scene->GetSceneGraph()->GetSkinnedMeshInstances())
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
