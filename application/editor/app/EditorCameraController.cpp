#include "EditorCameraController.h"

#include <core/path_utils.h>
#include <render/Passes/PostProcess/ToneMappingPasses.h>
#include <render/WorldRenderer/WorldRenderer.h>
#include <scene/camera/Camera.h>
#include <scene/SceneCameraAccess.h>

#include <cmath>

namespace caustica::editor
{

void EditorCameraController::bind(caustica::RenderCore& renderCore,
    PathTracerSettings& settings,
    caustica::render::WorldRenderer* worldRenderer)
{
    m_renderCore = &renderCore;
    m_settings = &settings;
    m_worldRenderer = worldRenderer;
}

float EditorCameraController::getVerticalFOV() const
{
    return m_renderCore ? m_renderCore->camera().verticalFOV() : 0.0f;
}

const caustica::FirstPersonCamera& EditorCameraController::camera() const
{
    return m_renderCore->camera().camera();
}

const std::shared_ptr<caustica::PlanarView>& EditorCameraController::view() const
{
    return m_renderCore->camera().view();
}

void EditorCameraController::markCameraChanged()
{
    if (m_settings)
        m_settings->ResetAccumulation = true;
    if (m_worldRenderer)
        m_worldRenderer->setGaussianSplatTemporalReset(true);
}

void EditorCameraController::setVerticalFOV(float cameraFOV)
{
    if (!m_renderCore)
        return;

    m_renderCore->camera().setVerticalFOV(cameraFOV);
    markCameraChanged();
}

void EditorCameraController::setIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
{
    if (!m_renderCore)
        return;

    m_renderCore->camera().setIntrinsics(fx, fy, cx, cy, width, height);
    markCameraChanged();
}

void EditorCameraController::clearIntrinsics()
{
    if (!m_renderCore)
        return;

    m_renderCore->camera().clearIntrinsics();
    markCameraChanged();
}

std::string EditorCameraController::getPosDirUpString() const
{
    return m_renderCore ? m_renderCore->camera().getPosDirUpString() : std::string{};
}

bool EditorCameraController::setFromPosDirUpString(const std::string& value)
{
    return m_renderCore && m_renderCore->camera().setFromPosDirUpString(value);
}

void EditorCameraController::saveToFile() const
{
    if (!m_renderCore)
        return;

    caustica::math::float4x4 projMatrix = m_renderCore->camera().view()->GetProjectionMatrix();
    float tanHalfFOVY = 1.0f / (projMatrix.m_data[1 * 4 + 1]);
    float fovY = atanf(tanHalfFOVY) * 2.0f;

    m_renderCore->camera().saveToFile(
        caustica::GetDirectoryWithExecutable() / "campos.txt",
        m_renderCore->camera().zNear(),
        fovY);
}

void EditorCameraController::loadFromFile()
{
    if (m_renderCore)
        m_renderCore->camera().loadFromFile(caustica::GetDirectoryWithExecutable() / "campos.txt");
}

void EditorCameraController::syncFromSceneCamera(const std::shared_ptr<caustica::PerspectiveCamera>& sceneCamera)
{
    if (!m_renderCore || !m_settings)
        return;

    m_renderCore->camera().updateFromSceneCamera(sceneCamera);

    auto sceneCameraEx = std::dynamic_pointer_cast<caustica::PerspectiveCamera>(sceneCamera);
    if (sceneCameraEx == nullptr)
        return;

    ToneMappingParameters defaults;
    m_settings->ToneMappingParams.autoExposure =
        sceneCameraEx->enableAutoExposure.value_or(defaults.autoExposure);
    m_settings->ToneMappingParams.exposureCompensation =
        sceneCameraEx->exposureCompensation.value_or(defaults.exposureCompensation);
    m_settings->ToneMappingParams.exposureValue =
        sceneCameraEx->exposureValue.value_or(defaults.exposureValue);
    m_settings->ToneMappingParams.exposureValueMin =
        sceneCameraEx->exposureValueMin.value_or(defaults.exposureValueMin);
    m_settings->ToneMappingParams.exposureValueMax =
        sceneCameraEx->exposureValueMax.value_or(defaults.exposureValueMax);
}

void EditorCameraController::syncFromSceneCamera(
    const caustica::scene::PerspectiveCameraData& camData,
    const dm::daffine3& globalTransform)
{
    if (!m_renderCore || !m_settings)
        return;

    m_renderCore->camera().updateFromSceneCamera(camData, globalTransform);

    ToneMappingParameters defaults;
    m_settings->ToneMappingParams.autoExposure =
        camData.enableAutoExposure.value_or(defaults.autoExposure);
    m_settings->ToneMappingParams.exposureCompensation =
        camData.exposureCompensation.value_or(defaults.exposureCompensation);
    m_settings->ToneMappingParams.exposureValue =
        camData.exposureValue.value_or(defaults.exposureValue);
    m_settings->ToneMappingParams.exposureValueMin =
        camData.exposureValueMin.value_or(defaults.exposureValueMin);
    m_settings->ToneMappingParams.exposureValueMax =
        camData.exposureValueMax.value_or(defaults.exposureValueMax);

    markCameraChanged();
}

} // namespace caustica::editor
