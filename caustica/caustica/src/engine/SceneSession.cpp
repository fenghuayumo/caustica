#include <engine/SceneSession.h>

#include <scene/SceneManager.h>
#include <assets/loader/ShaderFactory.h>
#include <backend/GpuDevice.h>

namespace caustica
{

SceneSession::SceneSession() = default;
SceneSession::~SceneSession() = default;
SceneSession::SceneSession(SceneSession&&) noexcept = default;
SceneSession& SceneSession::operator=(SceneSession&&) noexcept = default;

bool SceneSession::create(
    GpuDevice& gpuDevice,
    ShaderFactory& shaderFactory,
    const std::shared_ptr<TextureLoader>& textureLoader,
    const std::shared_ptr<SceneTypeFactory>& sceneTypeFactory,
    std::function<void()> onLoaded,
    std::function<void()> onUnloading)
{
    manager = std::make_unique<::SceneManager>(
        gpuDevice,
        shaderFactory,
        textureLoader,
        sceneTypeFactory);
    manager->setLoadingCallbacks(std::move(onLoaded), std::move(onUnloading));
    return true;
}

void SceneSession::reset()
{
    manager.reset();
}

} // namespace caustica
