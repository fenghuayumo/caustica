#include <render/core/SceneCameraController.h>

#include <core/path_utils.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <scene/camera/Camera.h>

#include <cmath>

namespace caustica
{

void SceneCameraController::bind(CameraController& camera,
    PathTracerSettings& settings,
    caustica::render::WorldRenderer* worldRenderer)
{
    m_camera = &camera;
    m_settings = &settings;
    m_worldRenderer = worldRenderer;
}

float SceneCameraController::getVerticalFOV() const
{
    return m_camera ? m_camera->verticalFOV() : 0.0f;
}

const FirstPersonCamera& SceneCameraController::camera() const
{
    return m_camera->camera();
}

const std::shared_ptr<PlanarView>& SceneCameraController::view() const
{
    return m_camera->view();
}

void SceneCameraController::markCameraChanged()
{
    if (m_settings)
        m_settings->ResetAccumulation = true;
    if (m_worldRenderer)
        m_worldRenderer->setGaussianSplatTemporalReset(true);
}

void SceneCameraController::setVerticalFOV(float cameraFOV)
{
    if (!m_camera)
        return;

    m_camera->setVerticalFOV(cameraFOV);
    markCameraChanged();
}

void SceneCameraController::setIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
{
    if (!m_camera)
        return;

    m_camera->setIntrinsics(fx, fy, cx, cy, width, height);
    markCameraChanged();
}

void SceneCameraController::clearIntrinsics()
{
    if (!m_camera)
        return;

    m_camera->clearIntrinsics();
    markCameraChanged();
}

std::string SceneCameraController::getPosDirUpString() const
{
    return m_camera ? m_camera->getPosDirUpString() : std::string{};
}

bool SceneCameraController::setFromPosDirUpString(const std::string& value)
{
    return m_camera && m_camera->setFromPosDirUpString(value);
}

void SceneCameraController::saveToFile() const
{
    if (!m_camera)
        return;

    math::float4x4 projMatrix = m_camera->view()->getProjectionMatrix();
    float tanHalfFOVY = 1.0f / (projMatrix.m_data[1 * 4 + 1]);
    float fovY = atanf(tanHalfFOVY) * 2.0f;

    m_camera->saveToFile(
        getDirectoryWithExecutable() / "campos.txt",
        m_camera->zNear(),
        fovY);
}

void SceneCameraController::loadFromFile()
{
    if (m_camera)
        m_camera->loadFromFile(getDirectoryWithExecutable() / "campos.txt");
}

} // namespace caustica
