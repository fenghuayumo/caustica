#pragma once

#include <core/command_line.h>
#include <core/progress.h>
#include <render/Core/TextureUtils.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>

namespace caustica
{
class BindingCache;
class CommonRenderPasses;
class RenderCore;
class TextureLoader;
}

class GameScene;
class SceneManager;
struct PathTracerSettings;

namespace caustica::render
{
class PathTracingWorldRenderer;
class SceneGaussianSplatPasses;
class SceneLightingPasses;
struct RenderRuntimeState;
struct RenderSessionState;
class SessionDiagnostics;
}

#if CAUSTICA_WITH_PYTHON
class PythonScripting;
#endif

namespace caustica::editor
{
class EditorCameraController;
class EditorUIState;

// Coordinates scene load/unload hooks across render passes, editor state, and game sample.
class SceneLifecycleCoordinator
{
public:
    struct Context
    {
        SceneManager* sceneManager = nullptr;
        caustica::RenderCore* renderCore = nullptr;
        caustica::BindingCache* bindingCache = nullptr;
        caustica::render::PathTracingWorldRenderer* worldRenderer = nullptr;
        caustica::render::SceneLightingPasses* lightingPasses = nullptr;
        caustica::render::SceneGaussianSplatPasses* gaussianSplatPasses = nullptr;
        EditorUIState* editor = nullptr;
        caustica::render::RenderSessionState* sessionState = nullptr;
        caustica::render::RenderRuntimeState* renderState = nullptr;
        PathTracerSettings* settings = nullptr;
        caustica::render::SessionDiagnostics* diagnostics = nullptr;
        const CommandLineOptions* cmdLine = nullptr;
        ProgressBar* progressLoading = nullptr;
        caustica::TextureLoader* textureLoader = nullptr;
        caustica::CommonRenderPasses* commonPasses = nullptr;
        ::GameScene* sampleGame = nullptr;
        EditorCameraController* cameraController = nullptr;
        double* sceneTime = nullptr;
        std::map<std::shared_ptr<caustica::LoadedTexture>, TextureCompressionType>* uncompressedTextures = nullptr;
        std::function<uint32_t()> frameIndex;
        std::function<std::filesystem::path()> assetsRoot;
        std::function<void()> postSceneLoad;
        std::function<bool(const std::string&)> setCameraPosDirUp;
#if CAUSTICA_WITH_PYTHON
        PythonScripting* pythonScripting = nullptr;
#endif
    };

    explicit SceneLifecycleCoordinator(Context context);

    void updateContext(Context context);

    void refreshEnvironmentMapMediaList();
    bool setCurrentScene(const std::string& sceneName, bool forceReload = false);
    void onSceneUnloading();
    void onSceneLoaded();

private:
    Context m_ctx;
};

} // namespace caustica::editor
