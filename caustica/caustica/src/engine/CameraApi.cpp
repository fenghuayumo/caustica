#include <engine/App.h>
#include <engine/AppResources.h>
#include <cassert>
#include <engine/CameraApi.h>
#include <engine/SceneQuery.h>
#include <render/core/CameraController.h>

namespace caustica
{

const std::vector<ecs::Entity>& sceneCameraEntities(const App& app)
{
    static const std::vector<ecs::Entity> kEmpty;
    auto* ew = entityWorld(app);
    return ew ? ew->cameraEntitiesInRegistrationOrder() : kEmpty;
}

uint32_t sceneCameraCount(const App& app)
{
    return static_cast<uint32_t>(sceneCameraEntities(app).size()) + 1;
}

uint32_t& selectedCameraIndex(App& app)
{
    assert(cameraController(app));
    return cameraController(app)->selectedCameraIndex();
}

float cameraVerticalFOV(const App& app)
{
    assert(cameraController(app));
    return cameraController(app)->verticalFOV();
}

float cameraZNear(const App& app)
{
    assert(cameraController(app));
    return cameraController(app)->zNear();
}

FirstPersonCamera& currentCamera(App& app)
{
    assert(cameraController(app));
    return cameraController(app)->camera();
}

const FirstPersonCamera& currentCamera(const App& app)
{
    assert(cameraController(app));
    return cameraController(app)->camera();
}

void markCameraChanged(App& app)
{
    if (auto* camera = cameraController(app))
        camera->markCameraChanged();
}

const std::shared_ptr<PlanarView>& currentView(const App& app)
{
    assert(cameraController(app));
    return cameraController(app)->view();
}

const PlanarView& view(const App& app)
{
    assert(cameraController(app));
    return *cameraController(app)->view();
}

std::string currentCameraPosDirUp(const App& app)
{
    assert(cameraController(app));
    return cameraController(app)->getPosDirUpString();
}

bool setCurrentCameraPosDirUp(App& app, const std::string& val)
{
    assert(cameraController(app));
    return cameraController(app)->setFromPosDirUpString(val);
}

void setCameraVerticalFOV(App& app, float cameraFOV)
{
    assert(cameraController(app));
    cameraController(app)->setVerticalFOVInteractive(cameraFOV);
}

void setCameraIntrinsics(App& app, float fx, float fy, float cx, float cy, float width, float height)
{
    assert(cameraController(app));
    cameraController(app)->setIntrinsicsInteractive(fx, fy, cx, cy, width, height);
}

void clearCameraIntrinsics(App& app)
{
    assert(cameraController(app));
    cameraController(app)->clearIntrinsicsInteractive();
}

void saveCurrentCamera(const App& app)
{
    assert(cameraController(app));
    cameraController(app)->saveToDefaultFile();
}

void loadCurrentCamera(App& app)
{
    assert(cameraController(app));
    cameraController(app)->loadFromDefaultFile();
}

} // namespace caustica
