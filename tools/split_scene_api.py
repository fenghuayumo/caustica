from pathlib import Path
import re

root = Path(r'd:\ProgramCode\C++\Render\caustica\caustica\caustica')
inc = root / 'include' / 'engine'
src = root / 'src' / 'engine'
text = (src / 'SceneApi.cpp').read_text(encoding='utf-8')
lines = text.splitlines(keepends=True)


def parse_funcs():
    funcs = []
    i = 315  # 0-based, start at gpuRender
    while i < len(lines):
        line = lines[i]
        if line.startswith('} // namespace caustica'):
            break
        if line and not line[0].isspace() and '(' in line and not line.strip().startswith('//'):
            sig_lines = [line]
            j = i
            while '{' not in ''.join(sig_lines) and j + 1 < len(lines):
                j += 1
                sig_lines.append(lines[j])
                if j > i + 8:
                    break
            if '{' not in ''.join(sig_lines):
                i += 1
                continue
            k = i
            while '{' not in lines[k]:
                k += 1
            depth = 0
            end = k
            while end < len(lines):
                for ch in lines[end]:
                    if ch == '{':
                        depth += 1
                    elif ch == '}':
                        depth -= 1
                        if depth == 0:
                            name_m = re.search(r'\b(\w+)\s*\(', ''.join(sig_lines))
                            name = name_m.group(1) if name_m else '?'
                            funcs.append((name, i + 1, end + 1, ''.join(lines[i:end + 1])))
                            i = end + 1
                            break
                else:
                    end += 1
                    continue
                break
            else:
                i += 1
            continue
        i += 1
    return funcs


funcs = parse_funcs()
by_name = {}
for name, a, b, body in funcs:
    by_name.setdefault(name, []).append((a, b, body))

modules = {
    'AppResources': [
        'gpuRender', 'gpuDevice', 'sceneManager', 'worldRenderer',
        'settings', 'runtimeState', 'diagnostics', 'cmdLine', 'viewState',
    ],
    'SceneQuery': [
        'activeScene', 'syncSceneAccess', 'entityWorld', 'sceneEcs',
        'availableScenes', 'currentSceneName', 'currentScenePath', 'isSceneStructureBusy',
        'shouldSkipRender', 'isSceneLoading', 'isSceneLoaded', 'shouldRenderWhenUnfocused',
        'findMaterial', 'findEntityByInstanceIndex',
    ],
    'CameraApi': [
        'sceneCameraCount', 'selectedCameraIndex', 'cameraVerticalFOV',
        'currentCamera', 'currentView', 'view',
        'currentCameraPosDirUp', 'setCurrentCameraPosDirUp',
        'setCameraVerticalFOV', 'setCameraIntrinsics', 'clearCameraIntrinsics',
        'saveCurrentCamera', 'loadCurrentCamera',
    ],
    'RenderSessionApi': [
        'debugDrawLine', 'envMapLocalPath', 'envMapOverrideSource', 'envMapMediaList',
        'setEnvMapOverrideSource',
        'loadGaussianSplatFile', 'gaussianSplatCount', 'gaussianSplatObjectCount', 'gaussianSplatFileName',
        'runGpuWorkOnRenderThread',
        'resolutionInfo', 'avgTimePerFrame',
        'requestMeshAccelRebuild',
        'ldrColorTexture', 'feedbackData', 'debugDeltaPathTree',
        'accumulationSampleIndex', 'renderSize', 'displaySize', 'accumulationCompleted', 'fpsInfo',
    ],
    'SceneSpawn': [
        'flushPendingStructureGpu', 'load', 'spawn', 'spawnFromFile', 'despawn',
    ],
    'SceneLifecycle': [
        'attachGpuRenderSubsystem', 'initStreamlineAndWindow', 'initializeScene',
        'setCurrentScene', 'onSceneUnloading', 'onSceneLoaded',
        'collectUncompressedTextures', 'hasAsyncLoadingInProgress',
    ],
    'RenderFrameApi': [
        'beginFrameScheduled', 'renderScene', 'afterWorldRenderScheduled',
        'animate', 'tickSimulationAndFrameTiming', 'backBufferResizing',
        'setSceneTime', 'sceneTime', 'sceneTimeRef',
    ],
}

all_assigned = set()
for names in modules.values():
    all_assigned.update(names)
missing = sorted(set(by_name) - all_assigned)
extra = sorted(all_assigned - set(by_name))
print('missing from modules', missing)
print('extra not found', extra)
if missing or extra:
    raise SystemExit('assignment mismatch')

headers = {
    'AppResources.h': r'''#pragma once

#include <core/command_line.h>
#include <render/AppDiagnostics.h>
#include <render/RenderRuntimeState.h>
#include <render/core/PathTracerSettings.h>

namespace caustica
{

class App;
class GpuDevice;
class GpuRenderSubsystem;
class SceneManager;
class SceneViewState;

namespace render
{
class WorldRenderer;
}

[[nodiscard]] GpuRenderSubsystem* gpuRender(const App& app);
[[nodiscard]] GpuDevice* gpuDevice(const App& app);
[[nodiscard]] SceneManager* sceneManager(const App& app);
[[nodiscard]] render::WorldRenderer* worldRenderer(const App& app);

[[nodiscard]] PathTracerSettings* settings(const App& app);
[[nodiscard]] render::RenderRuntimeState* runtimeState(const App& app);
[[nodiscard]] render::AppDiagnostics* diagnostics(const App& app);
[[nodiscard]] const CommandLineOptions* cmdLine(const App& app);
[[nodiscard]] SceneViewState* viewState(const App& app);

} // namespace caustica
''',
    'SceneQuery.h': r'''#pragma once

#include <engine/SceneAccess.h>
#include <ecs/Entity.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{

class App;
class Material;
class Scene;

[[nodiscard]] std::shared_ptr<Scene> activeScene(const App& app);
void syncSceneAccess(App& app);
[[nodiscard]] scene::SceneEntityWorld* entityWorld(const App& app);
[[nodiscard]] ecs::World* sceneEcs(const App& app);

[[nodiscard]] const std::vector<std::string>& availableScenes(const App& app);
[[nodiscard]] std::string currentSceneName(const App& app);
[[nodiscard]] std::filesystem::path currentScenePath(const App& app);
[[nodiscard]] bool isSceneStructureBusy(const App& app);
[[nodiscard]] bool isSceneLoading(const App& app);
[[nodiscard]] bool isSceneLoaded(const App& app);
[[nodiscard]] bool shouldSkipRender(const App& app);
[[nodiscard]] bool shouldRenderWhenUnfocused(const App& app);

[[nodiscard]] std::shared_ptr<Material> findMaterial(const App& app, int materialID);
[[nodiscard]] ecs::Entity findEntityByInstanceIndex(const App& app, int instanceIndex);

} // namespace caustica
''',
    'CameraApi.h': r'''#pragma once

#include <scene/camera/Camera.h>
#include <scene/View.h>

#include <memory>
#include <string>

namespace caustica
{

class App;
class PlanarView;

[[nodiscard]] uint sceneCameraCount(const App& app);
[[nodiscard]] uint& selectedCameraIndex(App& app);
[[nodiscard]] float cameraVerticalFOV(const App& app);
[[nodiscard]] const FirstPersonCamera& currentCamera(const App& app);
[[nodiscard]] const std::shared_ptr<PlanarView>& currentView(const App& app);
[[nodiscard]] const PlanarView& view(const App& app);

[[nodiscard]] std::string currentCameraPosDirUp(const App& app);
bool setCurrentCameraPosDirUp(App& app, const std::string& val);
void setCameraVerticalFOV(App& app, float cameraFOV);
void setCameraIntrinsics(App& app, float fx, float fy, float cx, float cy, float width, float height);
void clearCameraIntrinsics(App& app);
void saveCurrentCamera(const App& app);
void loadCurrentCamera(App& app);

} // namespace caustica
''',
    'SceneSpawn.h': r'''#pragma once

#include <assets/Handle.h>
#include <assets/TypedAssets.h>
#include <ecs/Entity.h>
#include <scene/SceneApply.h>

#include <filesystem>

namespace caustica
{

class App;

// Bevy-style assets.load + spawn. Extract owns GPU upload / AS rebuild.
[[nodiscard]] Handle<ScenePrefabAsset> load(App& app, const std::filesystem::path& path);
[[nodiscard]] ecs::Entity spawn(
    App& app,
    const Handle<ScenePrefabAsset>& prefab,
    const SceneApplyCallbacks& callbacks = {});
[[nodiscard]] ecs::Entity spawnFromFile(
    App& app,
    const std::filesystem::path& path,
    const SceneApplyCallbacks& callbacks = {});
[[nodiscard]] bool despawn(App& app, ecs::Entity entity);

} // namespace caustica
''',
    'SceneLifecycle.h': r'''#pragma once

#include <string>

namespace caustica
{

class App;
class GpuRenderSubsystem;

void attachGpuRenderSubsystem(App& app, GpuRenderSubsystem& gpuRenderSubsystem);
void initStreamlineAndWindow(App& app);
void initializeScene(App& app, const std::string& preferredScene);
void setCurrentScene(App& app, const std::string& sceneName, bool forceReload = false);

void onSceneLoaded(App& app);
void onSceneUnloading(App& app);

void collectUncompressedTextures(App& app);
[[nodiscard]] bool hasAsyncLoadingInProgress(const App& app);

} // namespace caustica
''',
    'RenderSessionApi.h': r'''#pragma once

#include <math/math.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rhi/rhi.h>

struct DebugFeedbackStruct;
struct DeltaTreeVizPathVertex;

namespace caustica
{

class App;
struct MeshInfo;

void debugDrawLine(App& app, math::float3 start, math::float3 stop, math::float4 col1, math::float4 col2);
void runGpuWorkOnRenderThread(App& app, const std::function<void()>& work);

void setEnvMapOverrideSource(App& app, const std::string& envMapOverride);
[[nodiscard]] const std::string& envMapLocalPath(const App& app);
[[nodiscard]] const std::string& envMapOverrideSource(const App& app);
[[nodiscard]] const std::vector<std::filesystem::path>& envMapMediaList(App& app);

bool loadGaussianSplatFile(App& app, const std::filesystem::path& fileName, bool convertRdfToRub = true);
[[nodiscard]] uint32_t gaussianSplatCount(const App& app);
[[nodiscard]] uint32_t gaussianSplatObjectCount(const App& app);
[[nodiscard]] const std::string& gaussianSplatFileName(const App& app);

void requestMeshAccelRebuild(App& app, const std::shared_ptr<MeshInfo>& mesh);
void requestMeshAccelRebuild(App& app, const std::shared_ptr<MeshInfo>& mesh, bool resetAccumulation);

[[nodiscard]] caustica::rhi::ITexture* ldrColorTexture(const App& app);
[[nodiscard]] const DebugFeedbackStruct& feedbackData(const App& app);
[[nodiscard]] const DeltaTreeVizPathVertex* debugDeltaPathTree(const App& app);
[[nodiscard]] int accumulationSampleIndex(const App& app);
[[nodiscard]] math::uint2 renderSize(const App& app);
[[nodiscard]] math::uint2 displaySize(const App& app);
[[nodiscard]] bool accumulationCompleted(const App& app);
[[nodiscard]] float avgTimePerFrame(const App& app);
[[nodiscard]] std::string resolutionInfo(const App& app);
[[nodiscard]] std::string fpsInfo(const App& app);

} // namespace caustica
''',
    'RenderFrameApi.h': r'''#pragma once

#include <cstdint>

namespace caustica
{

class App;
class GpuDevice;

// Schedule-facing frame helpers (also registered by scene plugins).
void beginFrameScheduled(App& app);
void animate(App& app, float elapsedTimeSeconds);
void tickSimulationAndFrameTiming(App& app, float elapsedTimeSeconds);
void renderScene(App& app, GpuDevice& gpuDevice);
void afterWorldRenderScheduled(App& app, GpuDevice& gpuDevice);
void backBufferResizing(App& app);

void setSceneTime(App& app, double sceneTime);
[[nodiscard]] double sceneTime(const App& app);
[[nodiscard]] double& sceneTimeRef(App& app);

} // namespace caustica
''',
    'SceneApi.h': r'''#pragma once

// Compatibility umbrella for the former SceneApi god-facade.
// Prefer focused headers for new code:
//   AppResources.h, SceneQuery.h, SceneSpawn.h, CameraApi.h,
//   SceneLifecycle.h, RenderSessionApi.h, RenderFrameApi.h
// Plugin schedule entry points (updateCamera / prepareRenderFrame / ...) live in ScenePlugins.h.

#include <engine/AppResources.h>
#include <engine/SceneQuery.h>
#include <engine/SceneSpawn.h>
#include <engine/CameraApi.h>
#include <engine/SceneLifecycle.h>
#include <engine/RenderSessionApi.h>
#include <engine/RenderFrameApi.h>
#include <engine/ScenePlugins.h>
''',
    'SceneApiInternal.h': r'''#pragma once

#include <string>

namespace caustica
{

class App;
class CameraController;

namespace detail
{

[[nodiscard]] CameraController* sessionCamera(App& app);
void applySceneSwitch(App& app, const std::string& sceneName, bool forceReload);

} // namespace detail
} // namespace caustica
''',
}

for name, content in headers.items():
    (inc / name).write_text(content, encoding='utf-8', newline='\n')
    print('wrote', name)

COMMON = '''#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/AppResources.h>
#include <engine/SceneViewState.h>
#include <cassert>
'''

cpp_includes = {
    'AppResources': COMMON + '''#include <backend/GpuDevice.h>
#include <scene/SceneManager.h>
#include <render/worldRenderer/WorldRenderer.h>
''',
    'SceneQuery': COMMON + '''#include <engine/SceneQuery.h>
#include <engine/SceneAccess.h>
#include <scene/Scene.h>
#include <scene/SceneManager.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/core/PathTracerSettings.h>
#include <render/worldRenderer/WorldRenderer.h>
''',
    'CameraApi': COMMON + '''#include <engine/CameraApi.h>
#include <engine/SceneQuery.h>
#include <engine/SceneApiInternal.h>
#include <render/core/CameraController.h>
''',
    'RenderSessionApi': COMMON + '''#include <engine/RenderSessionApi.h>
#include <shaders/PathTracer/PathTracerDebug.hlsli>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneRayTracingResources.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <render/core/RenderTargets.h>
#include <math/math.h>
''',
    'SceneSpawn': COMMON + '''#include <engine/SceneSpawn.h>
#include <engine/SceneQuery.h>
#include <engine/RenderSessionApi.h>
#include <engine/ScenePlugins.h>
#include <assets/AssetSystem.h>
#include <assets/RuntimeMeshLoadTypes.h>
#include <scene/loader/RuntimeMeshLoader.h>
#include <scene/SceneApply.h>
#include <scene/SceneManager.h>
#include <scene/Scene.h>
#include <render/core/SceneGpuUpdater.h>
#include <render/core/RenderSceneTypeFactory.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <render/SceneRayTracingResources.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <assets/loader/TextureLoader.h>
#include <assets/loader/ShaderMacro.h>
#include <backend/GpuDevice.h>
''',
    'SceneLifecycle': COMMON + '''#include <engine/SceneLifecycle.h>
#include <engine/SceneQuery.h>
#include <engine/CameraApi.h>
#include <engine/RenderSessionApi.h>
#include <engine/SceneApiInternal.h>
#include <engine/RenderThread.h>
#include <engine/SceneAccess.h>
#include <backend/GpuDevice.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <scene/SceneManager.h>
#include <scene/scene_utils.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneEcs.h>
#include <scene/SceneRenderExtract.h>
#include <render/core/PathTracerSettings.h>
#include <render/core/CameraController.h>
#include <render/SceneLightingPasses.h>
#include <assets/loader/TextureLoader.h>
#include <algorithm>
#include <cctype>
''',
    'RenderFrameApi': COMMON + '''#include <engine/RenderFrameApi.h>
#include <engine/SceneQuery.h>
#include <engine/SceneLifecycle.h>
#include <engine/RenderSessionApi.h>
#include <engine/SceneApiInternal.h>
#include <engine/RenderThread.h>
#include <assets/AssetSystem.h>
#include <scene/SceneManager.h>
#include <scene/SceneAnimationAccess.h>
#include <scene/SceneEcs.h>
#include <scene/Scene.h>
#include <render/core/SceneMeshEditing.h>
#include <render/core/PathTracerSettings.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <render/RenderRuntimeState.h>
#include <backend/GpuDevice.h>
#include <core/format.h>
#include <core/Timer.h>
#include <cmath>
#include <algorithm>
#include <optional>
''',
}


def bodies_for(names):
    chunks = []
    for n in names:
        for _a, _b, body in by_name[n]:
            body = body.replace('localSceneManager(app)', 'sceneManager(app)')
            body = body.replace('sessionCamera(app)', 'detail::sessionCamera(app)')
            body = body.replace('applySceneSwitch(app,', 'detail::applySceneSwitch(app,')
            body = body.replace('detail::detail::', 'detail::')
            chunks.append(body if body.endswith('\n') else body + '\n')
            chunks.append('\n')
    return ''.join(chunks)


internal_cpp = r'''#include <engine/SceneApiInternal.h>
#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/SceneViewState.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <core/log.h>
#include <render/core/CameraController.h>
#include <render/core/PathTracerSettings.h>
#include <scene/SceneManager.h>

namespace caustica::detail
{

CameraController* sessionCamera(App& app)
{
    if (GpuRenderSubsystem* gpu = gpuRender(app))
        return &gpu->camera();
    return nullptr;
}

void applySceneSwitch(App& app, const std::string& sceneName, bool forceReload)
{
    ::SceneManager* manager = sceneManager(app);
    PathTracerSettings* cfg = settings(app);
    SceneViewState* vs = viewState(app);
    if (!manager || !cfg || !vs)
        return;

    if (!manager->beginSceneSwitch(sceneName, getLocalPath(c_AssetsFolder), forceReload))
        return;

    cfg->ResetAccumulation = true;
    cfg->ResetRealtimeCaches = true;
    manager->setAsyncLoadingEnabled(false);

    vs->progressLoading.stop();
    vs->progressLoading.start("Loading scene...");
    manager->beginLoadingScene(
        std::make_shared<caustica::NativeFileSystem>(),
        manager->getCurrentScenePath());
    if (manager->getScene() == nullptr)
    {
        caustica::error("Unable to load scene '%s'", sceneName.c_str());
        manager->clearScene();
        vs->progressLoading.stop();
    }
}

} // namespace caustica::detail
'''
(src / 'SceneApiInternal.cpp').write_text(internal_cpp, encoding='utf-8', newline='\n')

lifecycle_helpers = r'''
namespace
{
    bool LooksLikeInlineSceneJson(const std::string& scene)
    {
        auto it = std::find_if_not(scene.begin(), scene.end(), [](unsigned char ch) {
            return std::isspace(ch);
        });
        return it != scene.end() && *it == '{';
    }

    void initViewState(SceneViewState& viewState)
    {
        if (!viewState.progressLoading.Active())
        {
            viewState.progressLoading.start("Initializing...");
            viewState.progressLoading.Set(50);
        }
    }

    void syncCameraFromScene(App& app)
    {
        auto scenePtr = caustica::activeScene(app);
        CameraController* cam = detail::sessionCamera(app);
        if (!scenePtr || !cam)
            return;

        const auto& cameraEntities = scenePtr->getCameraEntities();
        const auto* ew = scenePtr->getEntityWorld();
        bool syncedCamera = false;
        if (!cameraEntities.empty() && ew)
        {
            const uint32_t selectedIndex = cam->selectedCameraIndex();
            const uint32_t camIdx = (selectedIndex > 0) ? (selectedIndex - 1)
                : static_cast<uint32_t>(cameraEntities.size() - 1);
            if (camIdx < cameraEntities.size())
            {
                ecs::Entity camEntity = cameraEntities[camIdx];
                const auto* camComp = scene::tryGetCamera(ew->world(), camEntity);
                const auto* globalComp = ew->world().get<scene::GlobalTransformComponent>(camEntity);
                if (camComp && globalComp)
                {
                    const scene::CameraRenderProxy proxy =
                        scene::makeCameraRenderProxy(camEntity, *camComp, *globalComp);
                    PathTracerSettings* settingsPtr = caustica::settings(app);
                    scene::applyCameraRenderProxyToController(proxy, *cam, settingsPtr);
                    if (settingsPtr)
                        settingsPtr->ResetAccumulation = true;
                    if (auto* wr = caustica::worldRenderer(app))
                        wr->setGaussianSplatTemporalReset(true);
                    syncedCamera = proxy.projection == scene::CameraProjectionKind::Perspective;
                }
            }
        }
        if (!syncedCamera)
            cam->setupDefaultCamera();
    }
}
'''

frame_helpers = r'''
using namespace caustica::math;

const char* g_windowTitle = "caustica";
FPSLimiter g_FPSLimiter;

namespace
{
    void updateFpsInfo(App& app, double frameTimeSeconds)
    {
        SceneViewState* vs = caustica::viewState(app);
        PathTracerSettings* cfg = caustica::settings(app);
        if (!vs || !cfg || frameTimeSeconds <= 0.0)
            return;

#if CAUSTICA_WITH_STREAMLINE
        if (cfg->actualDLSSFGMode() != SI::DLSSGMode::eOff)
        {
            uint32_t presentedFrames = cfg->DLSSFGMultiplier;
            if (presentedFrames == 0)
                presentedFrames = 1u + cfg->DLSSFGNumFramesToGenerate;

            vs->fpsInfo = stringFormat("%.3f ms/%d-frames* (%.1f FPS*) *DLSS-G",
                frameTimeSeconds * 1e3, presentedFrames, presentedFrames / frameTimeSeconds);
            return;
        }
#endif

        vs->fpsInfo = stringFormat("%.3f ms/frame (%.1f FPS)", frameTimeSeconds * 1e3, 1.0 / frameTimeSeconds);
    }

    void recordFrameTiming(App& app, const GpuDevice& gpuDevice)
    {
        SceneViewState* vs = caustica::viewState(app);
        double frameTime = gpuDevice.getAverageFrameTimeSeconds();
        if (frameTime <= 0.0 && vs && vs->lastDeltaTime > 0.0f)
            frameTime = static_cast<double>(vs->lastDeltaTime);
        updateFpsInfo(app, frameTime);
    }

    bool processPendingSceneSwitch(App& app)
    {
        SceneViewState* vs = caustica::viewState(app);
        if (!vs)
            return false;

        std::optional<SceneViewState::PendingSceneSwitch> pending;
        {
            std::lock_guard lock(vs->pendingSceneSwitchMutex);
            pending.swap(vs->pendingSceneSwitch);
        }

        if (!pending)
            return false;

        detail::applySceneSwitch(app, pending->sceneName, pending->forceReload);
        return true;
    }

    bool processHotReloadChanges(App& app)
    {
        AssetSystem* assets = app.tryResource<AssetSystem>();
        ::SceneManager* manager = sceneManager(app);
        if (!assets || !manager || manager->isSceneLoading())
            return false;

        const std::vector<HotReloadChange> changes = assets->pollHotReloadChanges();
        if (changes.empty())
            return false;

        const std::string sceneName = manager->getCurrentSceneName();
        const std::filesystem::path scenePath = manager->getCurrentScenePath();
        if (sceneName.empty() || scenePath == std::filesystem::path(SceneManager::inlineSceneSentinel()))
            return false;

        caustica::info("Hot reload: detected %zu asset source change(s), reloading scene '%s'",
            changes.size(), sceneName.c_str());
        detail::applySceneSwitch(app, sceneName, true);
        return true;
    }

    void tickSceneSwitchTest(App& app)
    {
        const CommandLineOptions* cmd = caustica::cmdLine(app);
        SceneViewState* vs = caustica::viewState(app);
        if (!cmd || !vs || cmd->sceneSwitchTestInterval <= 0)
            return;

        ::SceneManager* manager = sceneManager(app);
        if (!manager)
            return;

        if (--vs->sceneSwitchTestFramesUntilSwitch > 0)
            return;

        vs->sceneSwitchTestFramesUntilSwitch = cmd->sceneSwitchTestInterval;

        const std::vector<std::string>& scenes = caustica::availableScenes(app);
        if (scenes.size() < 2)
            return;

        if (vs->sceneSwitchTestSceneIndex >= scenes.size())
            vs->sceneSwitchTestSceneIndex = 0;

        const std::string& nextScene = scenes[vs->sceneSwitchTestSceneIndex++];
        caustica::info("SceneSwitchTest: requesting '%s' from render thread", nextScene.c_str());
        caustica::setCurrentScene(app, nextScene);

        ++vs->sceneSwitchTestSwitchesDone;
        if (cmd->sceneSwitchTestCount > 0
            && vs->sceneSwitchTestSwitchesDone >= cmd->sceneSwitchTestCount)
        {
            app.requestExit();
        }
    }

    void beginFrame(App& app)
    {
        if (!processPendingSceneSwitch(app))
            processHotReloadChanges(app);
        tickSceneSwitchTest(app);
    }

    void afterWorldRenderDefault(App& app, GpuDevice& /*gpuDevice*/)
    {
        RenderRuntimeState* runtime = caustica::runtimeState(app);
        if (!runtime)
            return;

        const auto* wr = caustica::worldRenderer(app);
        const caustica::render::RenderPickState renderedPick = wr
            ? wr->getLastRenderedPicking()
            : caustica::render::RenderPickState{};
        if (renderedPick.MaterialRequested)
            runtime->Picking.MaterialRequested = false;
        if (renderedPick.InstanceRequested)
            runtime->Picking.InstanceRequested = false;
    }

    void afterWorldRender(App& app, GpuDevice& gpuDevice)
    {
        afterWorldRenderDefault(app, gpuDevice);
    }
}
'''

file_map = {
    'AppResources.cpp': ('AppResources', None),
    'SceneQuery.cpp': ('SceneQuery', None),
    'CameraApi.cpp': ('CameraApi', None),
    'RenderSessionApi.cpp': ('RenderSessionApi', None),
    'SceneSpawn.cpp': ('SceneSpawn', None),
    'SceneLifecycle.cpp': ('SceneLifecycle', lifecycle_helpers),
    'RenderFrameApi.cpp': ('RenderFrameApi', frame_helpers),
}

for fname, (mod, helpers) in file_map.items():
    out = [cpp_includes[mod], '\n']
    if helpers:
        out.append(helpers)
        out.append('\n')
    out.append('namespace caustica\n{\n\n')
    out.append(bodies_for(modules[mod]))
    out.append('} // namespace caustica\n')
    (src / fname).write_text(''.join(out), encoding='utf-8', newline='\n')
    print('wrote', fname)

(src / 'SceneApi.cpp').unlink()
print('deleted SceneApi.cpp')

plugins_path = inc / 'ScenePlugins.h'
plugins = plugins_path.read_text(encoding='utf-8')
if 'updateCamera' not in plugins:
    plugins = plugins.replace(
        'void registerWindowTitlePlugin(App& app);\n',
        '''void registerWindowTitlePlugin(App& app);

// Schedule entry points implemented by the plugins above / RenderFrameApi.
void updateCamera(App& app, float elapsedTimeSeconds);
void updateWindowTitle(App& app);
void prepareRenderFrame(App& app);
void refreshEntityWorld(App& app, uint32_t frameIndex);

''',
    )
    plugins_path.write_text(plugins, encoding='utf-8', newline='\n')
    print('updated ScenePlugins.h')

print('DONE')
