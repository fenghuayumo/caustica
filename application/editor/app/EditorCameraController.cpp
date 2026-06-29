#include "EditorCameraController.h"

#include <core/path_utils.h>
#include <render/Passes/PostProcess/ToneMappingPasses.h>
#include <render/WorldRenderer/PathTracingWorldRenderer.h>
#include <scene/camera/Camera.h>

#include <cmath>

namespace caustica::editor
{

EditorCameraController::EditorCameraController(Context context)
    : m_ctx(std::move(context))
{
}

void EditorCameraController::updateContext(Context context)
{
    m_ctx = std::move(context);
}

float EditorCameraController::getVerticalFOV() const
{
    return m_ctx.renderCore ? m_ctx.renderCore->camera().verticalFOV() : 0.0f;
}

const caustica::FirstPersonCamera& EditorCameraController::camera() const
{
    return m_ctx.renderCore->camera().camera();
}

const std::shared_ptr<caustica::PlanarView>& EditorCameraController::view() const
{
    return m_ctx.renderCore->camera().view();
}

void EditorCameraController::markCameraChanged()
{
    if (m_ctx.settings)
        m_ctx.settings->ResetAccumulation = true;
    if (m_ctx.worldRenderer)
        m_ctx.worldRenderer->setGaussianSplatTemporalReset(true);
}

void EditorCameraController::setVerticalFOV(float cameraFOV)
{
    if (!m_ctx.renderCore)
        return;

    m_ctx.renderCore->camera().setVerticalFOV(cameraFOV);
    markCameraChanged();
}

void EditorCameraController::setIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
{
    if (!m_ctx.renderCore)
        return;

    m_ctx.renderCore->camera().setIntrinsics(fx, fy, cx, cy, width, height);
    markCameraChanged();
}

void EditorCameraController::clearIntrinsics()
{
    if (!m_ctx.renderCore)
        return;

    m_ctx.renderCore->camera().clearIntrinsics();
    markCameraChanged();
}

std::string EditorCameraController::getPosDirUpString() const
{
    return m_ctx.renderCore ? m_ctx.renderCore->camera().getPosDirUpString() : std::string{};
}

bool EditorCameraController::setFromPosDirUpString(const std::string& value)
{
    return m_ctx.renderCore && m_ctx.renderCore->camera().setFromPosDirUpString(value);
}

void EditorCameraController::saveToFile() const
{
    if (!m_ctx.renderCore)
        return;

    caustica::math::float4x4 projMatrix = m_ctx.renderCore->camera().view()->GetProjectionMatrix();
    float tanHalfFOVY = 1.0f / (projMatrix.m_data[1 * 4 + 1]);
    float fovY = atanf(tanHalfFOVY) * 2.0f;

    m_ctx.renderCore->camera().saveToFile(
        caustica::GetDirectoryWithExecutable() / "campos.txt",
        m_ctx.renderCore->camera().zNear(),
        fovY);
}

void EditorCameraController::loadFromFile()
{
    if (m_ctx.renderCore)
        m_ctx.renderCore->camera().loadFromFile(caustica::GetDirectoryWithExecutable() / "campos.txt");
}

void EditorCameraController::syncFromSceneCamera(const std::shared_ptr<caustica::PerspectiveCamera>& sceneCamera)
{
    if (!m_ctx.renderCore || !m_ctx.settings)
        return;

    m_ctx.renderCore->camera().updateFromSceneCamera(sceneCamera);

    auto sceneCameraEx = std::dynamic_pointer_cast<caustica::PerspectiveCamera>(sceneCamera);
    if (sceneCameraEx == nullptr)
        return;

    ToneMappingParameters defaults;
    m_ctx.settings->ToneMappingParams.autoExposure =
        sceneCameraEx->enableAutoExposure.value_or(defaults.autoExposure);
    m_ctx.settings->ToneMappingParams.exposureCompensation =
        sceneCameraEx->exposureCompensation.value_or(defaults.exposureCompensation);
    m_ctx.settings->ToneMappingParams.exposureValue =
        sceneCameraEx->exposureValue.value_or(defaults.exposureValue);
    m_ctx.settings->ToneMappingParams.exposureValueMin =
        sceneCameraEx->exposureValueMin.value_or(defaults.exposureValueMin);
    m_ctx.settings->ToneMappingParams.exposureValueMax =
        sceneCameraEx->exposureValueMax.value_or(defaults.exposureValueMax);
}

} // namespace caustica::editor
