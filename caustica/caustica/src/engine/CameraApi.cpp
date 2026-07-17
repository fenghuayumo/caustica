#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/AppResources.h>
#include <cassert>
#include <engine/CameraApi.h>
#include <engine/SceneQuery.h>
#include <engine/SceneApiInternal.h>
#include <render/core/CameraController.h>

namespace caustica
{

uint sceneCameraCount(const App& app)
{
    auto scenePtr = activeScene(app);
    if (!scenePtr)
        return 1;
    return (uint)scenePtr->getCameraEntities().size() + 1;
}

uint& selectedCameraIndex(App& app)
{
    assert(detail::sessionCamera(app));
    return detail::sessionCamera(app)->selectedCameraIndex();
}

float cameraVerticalFOV(const App& app)
{
    assert(detail::sessionCamera(app));
    return detail::sessionCamera(app)->verticalFOV();
}

const FirstPersonCamera& currentCamera(const App& app)
{
    assert(detail::sessionCamera(app));
    return detail::sessionCamera(app)->camera();
}

const std::shared_ptr<PlanarView>& currentView(const App& app)
{
    assert(detail::sessionCamera(app));
    return detail::sessionCamera(app)->view();
}

const PlanarView& view(const App& app)
{
    assert(detail::sessionCamera(app));
    return *detail::sessionCamera(app)->view();
}

std::string currentCameraPosDirUp(const App& app)
{
    assert(detail::sessionCamera(app));
    return detail::sessionCamera(app)->getPosDirUpString();
}

bool setCurrentCameraPosDirUp(App& app, const std::string& val)
{
    assert(detail::sessionCamera(app));
    return detail::sessionCamera(app)->setFromPosDirUpString(val);
}

void setCameraVerticalFOV(App& app, float cameraFOV)
{
    assert(detail::sessionCamera(app));
    detail::sessionCamera(app)->setVerticalFOVInteractive(cameraFOV);
}

void setCameraIntrinsics(App& app, float fx, float fy, float cx, float cy, float width, float height)
{
    assert(detail::sessionCamera(app));
    detail::sessionCamera(app)->setIntrinsicsInteractive(fx, fy, cx, cy, width, height);
}

void clearCameraIntrinsics(App& app)
{
    assert(detail::sessionCamera(app));
    detail::sessionCamera(app)->clearIntrinsicsInteractive();
}

void saveCurrentCamera(const App& app)
{
    assert(detail::sessionCamera(app));
    detail::sessionCamera(app)->saveToDefaultFile();
}

void loadCurrentCamera(App& app)
{
    assert(detail::sessionCamera(app));
    detail::sessionCamera(app)->loadFromDefaultFile();
}

} // namespace caustica
