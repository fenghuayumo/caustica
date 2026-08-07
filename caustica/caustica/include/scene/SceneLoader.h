#pragma once

#include <core/task/TaskRuntime.h>
#include <core/vfs/VFS.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>

namespace caustica
{

// =============================================================================
// SceneLoader — async/sync scene loading via TaskRuntime (ADR 0001).
//
// Owns the load TaskHandle and loading state. The actual load work is
// injected via setLoadFunc(). Call update() once per frame to join a
// finished task and fire the onLoaded callback.
// =============================================================================
class SceneLoader
{
public:
    using LoadFunc = std::function<bool(
        std::shared_ptr<IFileSystem>, const std::filesystem::path&)>;

    SceneLoader() = default;
    ~SceneLoader();

    SceneLoader(const SceneLoader&) = delete;
    SceneLoader& operator=(const SceneLoader&) = delete;

    void setLoadFunc(LoadFunc func) { m_loadFunc = std::move(func); }
    void setAsyncEnabled(bool enabled) { m_asyncLoad = enabled; }

    bool isLoaded() const { return m_loaded.load(std::memory_order_acquire); }
    bool isLoading() const { return static_cast<bool>(m_task); }

    void beginLoading(std::shared_ptr<IFileSystem> fs,
        const std::filesystem::path& path);

    void update();
    void reset();

    std::function<void()> onLoaded;
    std::function<void()> onUnloading;

private:
    friend struct ImportJob;

    bool invokeLoadFunc(std::shared_ptr<IFileSystem> fs, const std::filesystem::path& path);
    void notifyLoadFinished(bool ok);

    LoadFunc m_loadFunc;
    task::TaskHandle m_task;
    std::atomic<bool> m_loaded{false};
    std::atomic<bool> m_loadFinished{false};
    bool m_asyncLoad = true;
};

} // namespace caustica
