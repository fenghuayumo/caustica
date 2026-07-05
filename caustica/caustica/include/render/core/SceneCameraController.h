#pragma once

#include <render/core/CameraController.h>
#include <render/core/PathTracerSettings.h>
#include <scene/SceneEcs.h>

#include <memory>
#include <string>

namespace caustica
{
class PerspectiveCamera;
}

namespace caustica::render
{
class WorldRenderer;
}

namespace caustica
{

// Scene camera controls with path-tracer side effects (accumulation reset, etc.).
class SceneCameraController
{
public:
    SceneCameraController() = default;

    void bind(CameraController& camera,
        PathTracerSettings& settings,
        caustica::render::WorldRenderer* worldRenderer);

    float getVerticalFOV() const;
    void setVerticalFOV(float cameraFOV);
    void setIntrinsics(float fx, float fy, float cx, float cy, float width, float height);
    void clearIntrinsics();

    std::string getPosDirUpString() const;
    bool setFromPosDirUpString(const std::string& value);
    void saveToFile() const;
    void loadFromFile();

    void syncFromSceneCamera(const std::shared_ptr<PerspectiveCamera>& sceneCamera);
    void syncFromSceneCamera(const scene::PerspectiveCameraData& camData, const dm::daffine3& globalTransform);

    const FirstPersonCamera& camera() const;
    const std::shared_ptr<PlanarView>& view() const;

private:
    void markCameraChanged();

    CameraController* m_camera = nullptr;
    PathTracerSettings* m_settings = nullptr;
    caustica::render::WorldRenderer* m_worldRenderer = nullptr;
};

} // namespace caustica
