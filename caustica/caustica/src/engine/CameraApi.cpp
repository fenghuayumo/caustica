#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/AppResources.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/CameraApi.h>
#include <engine/SceneQuery.h>
#include <engine/SceneApiInternal.h>
#include <render/core/CameraController.h>

using namespace caustica::render;

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
    assert(viewState(app));
    return viewState(app)->cameraController.getVerticalFOV();
}

const FirstPersonCamera& currentCamera(const App& app)
{
    assert(viewState(app));
    return viewState(app)->cameraController.camera();
}

const std::shared_ptr<PlanarView>& currentView(const App& app)
{
    assert(viewState(app));
    return viewState(app)->cameraController.view();
}

const PlanarView& view(const App& app)
{
    assert(viewState(app));
    return *viewState(app)->cameraController.view();
}

std::string currentCameraPosDirUp(const App& app)
{
    assert(viewState(app));
    return viewState(app)->cameraController.getPosDirUpString();
}

bool setCurrentCameraPosDirUp(App& app, const std::string& val)
{
    assert(viewState(app));
    return viewState(app)->cameraController.setFromPosDirUpString(val);
}

void setCameraVerticalFOV(App& app, float cameraFOV)
{
    assert(viewState(app));
    viewState(app)->cameraController.setVerticalFOV(cameraFOV);
}

void setCameraIntrinsics(App& app, float fx, float fy, float cx, float cy, float width, float height)
{
    assert(viewState(app));
    viewState(app)->cameraController.setIntrinsics(fx, fy, cx, cy, width, height);
}

void clearCameraIntrinsics(App& app)
{
    assert(viewState(app));
    viewState(app)->cameraController.clearIntrinsics();
}

void saveCurrentCamera(const App& app)
{
    assert(viewState(app));
    viewState(app)->cameraController.saveToFile();
}

void loadCurrentCamera(App& app)
{
    assert(viewState(app));
    viewState(app)->cameraController.loadFromFile();
}

} // namespace caustica
