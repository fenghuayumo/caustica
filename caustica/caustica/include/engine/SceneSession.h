#pragma once

#include <functional>
#include <memory>

class SceneManager;

namespace caustica
{

class GpuDevice;
class SceneTypeFactory;
class ShaderFactory;
class TextureLoader;

// App-owned SceneManager lifetime (load / switch / structure edit gate).
struct SceneSession
{
    SceneSession();
    ~SceneSession();

    SceneSession(const SceneSession&) = delete;
    SceneSession& operator=(const SceneSession&) = delete;
    SceneSession(SceneSession&&) noexcept;
    SceneSession& operator=(SceneSession&&) noexcept;

    std::unique_ptr<::SceneManager> manager;

    bool create(
        GpuDevice& gpuDevice,
        ShaderFactory& shaderFactory,
        const std::shared_ptr<TextureLoader>& textureLoader,
        const std::shared_ptr<SceneTypeFactory>& sceneTypeFactory,
        std::function<void()> onLoaded,
        std::function<void()> onUnloading);

    void reset();
};

} // namespace caustica
