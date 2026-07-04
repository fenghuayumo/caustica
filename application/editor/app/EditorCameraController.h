#pragma once

#include <render/Core/CameraController.h>
#include <render/Core/PathTracerSettings.h>
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

namespace caustica::editor
{

// Editor-side camera controls with path-tracer side effects (accumulation reset, etc.).
class EditorCameraController
{
public:
    EditorCameraController() = default;

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

    void syncFromSceneCamera(const std::shared_ptr<caustica::PerspectiveCamera>& sceneCamera);
    void syncFromSceneCamera(const caustica::scene::PerspectiveCameraData& camData, const dm::daffine3& globalTransform);

    const caustica::FirstPersonCamera& camera() const;
    const std::shared_ptr<caustica::PlanarView>& view() const;

private:
    void markCameraChanged();

    CameraController* m_camera = nullptr;
    PathTracerSettings* m_settings = nullptr;
    caustica::render::WorldRenderer* m_worldRenderer = nullptr;
};

} // namespace caustica::editor
