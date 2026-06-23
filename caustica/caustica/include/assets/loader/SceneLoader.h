#pragma once

#include <core/vfs/VFS.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <thread>

namespace caustica
{

// =============================================================================
// SceneLoader — async/sync scene loading with thread management.
//
// Owns the loading thread and loading state.  The actual load work is
// injected via setLoadFunc().  Call update() once per frame to join a
// finished thread and fire the onLoaded callback.
//
// Typical usage from SceneRender:
//   m_Loader.setLoadFunc([this](auto fs, auto& path) {
//       return LoadScene(fs, path);   // subclass virtual
//   });
//   m_Loader.onLoaded   = [this] { SceneLoaded(); };
//   m_Loader.onUnloading = [this] { SceneUnloading(); };
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

    // --- Configuration ---
    void setLoadFunc(LoadFunc func) { m_loadFunc = std::move(func); }
    void setAsyncEnabled(bool enabled) { m_asyncLoad = enabled; }

    // --- State ---
    bool isLoaded() const { return m_loaded; }
    bool isLoading() const { return m_thread != nullptr; }

    // --- Lifecycle ---
    //
    // Starts loading.  In async mode, spawns a thread and returns immediately;
    // the caller should poll update() each frame.  In sync mode the load
    // function runs on the calling thread and the function returns with the
    // work already done (isLoaded() will be true, but onLoaded is NOT fired
    // automatically — the caller decides when to invoke it).
    void beginLoading(std::shared_ptr<IFileSystem> fs,
        const std::filesystem::path& path);

    // Call once per frame.  If an async load completed, joins the thread
    // and fires onLoaded().
    void update();

    // Force-reset internal state.  Does NOT fire onUnloading.
    void reset();

    // --- Callbacks ---
    std::function<void()> onLoaded;
    std::function<void()> onUnloading;

private:
    LoadFunc                       m_loadFunc;
    std::unique_ptr<std::thread>    m_thread;
    bool m_loaded    = false;
    bool m_asyncLoad = true;
};

} // namespace caustica
