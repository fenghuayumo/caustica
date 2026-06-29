#pragma once

#include <render/Core/CameraController.h>
#include <render/Core/PathTracerSettings.h>

#include <memory>
#include <string>

namespace caustica
{
class PerspectiveCamera;
class RenderCore;
}

namespace caustica::render
{
class PathTracingWorldRenderer;
}

namespace caustica::editor
{

// Editor-side camera controls with path-tracer side effects (accumulation reset, etc.).
class EditorCameraController
{
public:
    struct Context
    {
        caustica::RenderCore* renderCore = nullptr;
        PathTracerSettings* settings = nullptr;
        caustica::render::PathTracingWorldRenderer* worldRenderer = nullptr;
    };

    explicit EditorCameraController(Context context);

    void updateContext(Context context);

    float getVerticalFOV() const;
    void setVerticalFOV(float cameraFOV);
    void setIntrinsics(float fx, float fy, float cx, float cy, float width, float height);
    void clearIntrinsics();

    std::string getPosDirUpString() const;
    bool setFromPosDirUpString(const std::string& value);
    void saveToFile() const;
    void loadFromFile();

    void syncFromSceneCamera(const std::shared_ptr<caustica::PerspectiveCamera>& sceneCamera);

    const caustica::FirstPersonCamera& camera() const;
    const std::shared_ptr<caustica::PlanarView>& view() const;

private:
    void markCameraChanged();

    Context m_ctx;
};

} // namespace caustica::editor
