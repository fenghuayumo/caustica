#pragma once

#include <cassert>

#include <backend/GpuDevice.h>
#include <core/command_line.h>
#include <core/progress.h>
#include <math/math.h>
#include <render/Core/CameraController.h>
#include <render/Core/PathTracerSettings.h>
#include <render/Core/RenderCore.h>
#include <render/Core/TextureUtils.h>
#include <scene/Scene.h>
#include <scene/SceneManager.h>
#include <scene/camera/Camera.h>

#include <render/RenderRuntimeState.h>
#include <render/SessionDiagnostics.h>
#include "ui/EditorUIData.h"
#include "EditorInputRouter.h"
#include <render/RenderSessionState.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rhi/nvrhi.h>

namespace caustica
{
class BindingCache;
class CommonRenderPasses;
class DescriptorTableManager;
class FirstPersonCamera;
class Material;
class MeshInfo;
class PlanarView;
class ShaderFactory;
class TextureLoader;
class Event;
class KeyPressedEvent;
class KeyReleasedEvent;
class MouseMovedEvent;
class MouseButtonPressedEvent;
class MouseButtonReleasedEvent;
class MouseScrolledEvent;
class RootFileSystem;
} // namespace caustica

namespace caustica::render
{
class PathTracingWorldRenderer;
class SceneGaussianSplatPasses;
class SceneLightingPasses;
class SceneRayTracingResources;
class SessionDiagnostics;
} // namespace caustica::render

class RenderTargets;
class ZoomTool;
struct DebugFeedbackStruct;
struct DeltaTreeVizPathVertex;

#if CAUSTICA_WITH_PYTHON
class PythonScripting;
#endif

class GameScene;

namespace caustica::editor
{

class CaptureScriptManager;

using namespace caustica::math;

// Scene editor shell (mesh edit, Inspector, Capture). Path tracer is owned by EngineRenderer.
class SceneEditor
{
public:
    SceneEditor(const CommandLineOptions& cmdLine,
        caustica::render::RenderSessionState& sessionState,
        EditorUIState& editorState,
        caustica::render::SessionDiagnostics& diagnostics);
    SceneEditor(const CommandLineOptions& cmdLine, EditorUIData& ui, caustica::render::SessionDiagnostics& diagnostics);

    ~SceneEditor();

    void setGpuDevice(caustica::GpuDevice& dm) { m_gpuDevice = &dm; }

    void SetLatewarpOptions() { }
    bool ShouldAnimateUnfocused() { return false; }
    bool SupportsDepthBuffer() { return true; }
    void BackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) { (void)width; (void)height; (void)sampleCount; }
    void DisplayScaleChanged(float scaleX, float scaleY) { (void)scaleX; (void)scaleY; }

    std::shared_ptr<caustica::ShaderFactory> GetShaderFactory() const          { return m_shaderFactory; }
    std::shared_ptr<caustica::CommonRenderPasses> GetCommonPasses() const;
    std::shared_ptr<caustica::TextureLoader> GetTextureLoader() const { return m_TextureLoader; }
    std::shared_ptr<caustica::DescriptorTableManager> GetDescriptorTable() const;
    SceneManager* GetSceneManager() const { return m_sceneManager; }
    caustica::RenderCore* GetRenderCore() const { return m_renderCore; }
    nvrhi::ITexture*                       GetLdrColorTexture() const;
    std::shared_ptr<caustica::Scene>   GetScene() const;
    std::vector<std::string> const &        GetAvailableScenes() const;
    std::string                             GetCurrentSceneName() const;
    const DebugFeedbackStruct &             GetFeedbackData() const;
    const DeltaTreeVizPathVertex *          GetDebugDeltaPathTree() const;
    uint                                    GetSceneCameraCount() const;
    uint &                                  SelectedCameraIndex();
    const std::unique_ptr<::GameScene> &     GetGame() const                   { return m_sampleGame; }

    EditorUIData& GetUIData() { assert(m_editorUi); return *m_editorUi; }
    const EditorUIData& GetUIData() const { assert(m_editorUi); return *m_editorUi; }
    caustica::render::RenderSessionState& GetRenderSessionState() { return m_sessionState; }
    const caustica::render::RenderSessionState& GetRenderSessionState() const { return m_sessionState; }
    PathTracerSettings& GetPathTracerSettings() { return m_settings; }
    const PathTracerSettings& GetPathTracerSettings() const { return m_settings; }
    EditorUIState& GetEditorUIState() { return m_editor; }
    const EditorUIState& GetEditorUIState() const { return m_editor; }
    caustica::render::RenderRuntimeState& GetRenderRuntimeState() { return m_renderState; }
    const caustica::render::RenderRuntimeState& GetRenderRuntimeState() const { return m_renderState; }

    std::shared_ptr<caustica::Material> FindMaterial( int materialID ) const;
    std::shared_ptr<caustica::SceneGraphNode> FindNodeByInstanceIndex(int instanceIndex) const;

    void                                    HandleDroppedFiles();
    bool                                    LoadMeshFile(const std::filesystem::path& filePath);
    bool                                    LoadGltfMeshFile(const std::filesystem::path& filePath);
    bool                                    LoadObjMeshFile(const std::filesystem::path& filePath);
    void                                    FinalizeRuntimeSceneMutation(const std::shared_ptr<caustica::SceneGraphNode>& importedRoot);
    bool                                    DeleteSceneNode(const std::shared_ptr<caustica::SceneGraphNode>& node);
    void                                    RequestFullRebuild();
    std::vector<dm::float3>        GetMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh) const;
    void                                    SetMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh,
                                                            const std::vector<dm::float3>& vertices,
                                                            bool recomputeNormals = true,
                                                            bool rebuildAccelerationStructure = true);
    std::vector<dm::float3>        GetMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh);
    std::vector<dm::float3>        GetMeshVerticesWorld(const std::shared_ptr<caustica::SceneGraphNode>& node);
    void                                    SetMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh,
                                                            const std::vector<dm::float3>& vertices,
                                                            bool recomputeNormals = true,
                                                            bool rebuildAccelerationStructure = true);
    void                                    SetMeshVerticesWorld(const std::shared_ptr<caustica::SceneGraphNode>& node,
                                                            const std::vector<dm::float3>& vertices,
                                                            bool recomputeNormals = true,
                                                            bool rebuildAccelerationStructure = true);
    
    void                                    CollectUncompressedTextures();
    auto &                                  GetUncompressedTextures()               { return m_uncompressedTextures; }
    void                                    SaveCurrentCamera() const;
    void                                    LoadCurrentCamera();
    std::string                             GetCurrentCameraPosDirUp() const;
    bool                                    SetCurrentCameraPosDirUp(const std::string & val);

    float                                   GetCameraVerticalFOV() const;
    void                                    SetCameraVerticalFOV(float cameraFOV);
    void                                    SetCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height);
    void                                    ClearCameraIntrinsics();

    float                                   GetAvgTimePerFrame() const;

    void AttachRenderResources(const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
        const std::shared_ptr<caustica::CommonRenderPasses>& commonPasses,
        caustica::BindingCache* bindingCache,
        const std::shared_ptr<caustica::DescriptorTableManager>& descriptorTable,
        const std::shared_ptr<caustica::TextureLoader>& textureCache);
    void AttachSceneServices(SceneManager& sceneManager, caustica::RenderCore& renderCore);
    void AttachLightingPasses(caustica::render::SceneLightingPasses& lightingPasses);
    caustica::render::SceneLightingPasses& GetLightingPasses();
    const caustica::render::SceneLightingPasses& GetLightingPasses() const;
    void AttachRayTracingResources(caustica::render::SceneRayTracingResources& rayTracingResources);
    caustica::render::SceneRayTracingResources& GetRayTracingResources();
    const caustica::render::SceneRayTracingResources& GetRayTracingResources() const;
    void AttachGaussianSplatPasses(caustica::render::SceneGaussianSplatPasses& gaussianSplatPasses);
    caustica::render::SceneGaussianSplatPasses& GetGaussianSplatPasses();
    const caustica::render::SceneGaussianSplatPasses& GetGaussianSplatPasses() const;
    void Init(const std::string& preferredScene,
        const std::shared_ptr<caustica::ShaderFactory>& shaderFactory);

    void AttachWorldRenderer(caustica::render::PathTracingWorldRenderer* renderer);
    [[nodiscard]] caustica::render::PathTracingWorldRenderer* GetWorldRenderer() const { return m_worldRenderer; }

    void initStreamlineAndWindow();

    void PrepareEditorFrame();
    void                                    SetCurrentScene(const std::string& sceneName, bool forceReload = false);
    bool                                    IsSceneLoading() const;
    bool                                    IsSceneLoaded() const;
    bool                                    LoadGaussianSplatFile(const std::filesystem::path& fileName, bool convertRdfToRub = true);
    uint32_t                                GetGaussianSplatCount() const;
    uint32_t                                GetGaussianSplatObjectCount() const;
    const std::string&                      GetGaussianSplatFileName() const;

    virtual void DestroyRTPipelines() {}

    virtual void                            SceneUnloading();
    void                                    SceneLoaded();
    virtual bool                            ShouldRenderUnfocused() const;
    virtual void                            Animate(float fElapsedTimeSeconds);

    void                                    RequestMeshAccelRebuild(const std::shared_ptr<caustica::MeshInfo>& mesh);

    bool                                    ShowDeltaTree() const;
    void                                    ResolvePickFeedback(const DebugFeedbackStruct& feedback);
    bool                                    ConsumeExperimentalPhotoScreenshot();
    void                                    CaptureScriptPreRender();
    void                                    CaptureScriptPostRender(std::function<bool(const char* fileName)> saveTexture);
    ::ZoomTool*                             GetOrCreateZoomTool();

    // Render entry points
    void Render(nvrhi::IFramebuffer* framebuffer);
    void BackBufferResizing();

    void OnRenderTargetsRecreated() { }
    void AddCustomBindings(nvrhi::BindingSetDesc& bindingSetDesc) { }

    void onEvent(caustica::Event& event);

    std::string                             GetResolutionInfo() const;
    std::string                             GetFPSInfo() const              { return m_fpsInfo; }

    void                                    DebugDrawLine( float3 start, float3 stop, float4 col1, float4 col2 );
    const caustica::FirstPersonCamera &   GetCurrentCamera( ) const;
    const std::shared_ptr<caustica::PlanarView>& GetCurrentView() const;

    void                                    SetSceneTime( double sceneTime );
    double                                  GetSceneTime( );

    bool                                    IsEnvMapLoaded() const      { return true; }
    const std::string &                     GetEnvMapLocalPath() const;
    const std::string &                     GetEnvMapOverrideSource() const;
    void                                    SetEnvMapOverrideSource(const std::string & envMapOverride);
    const std::vector<std::filesystem::path> & GetEnvMapMediaList();

    double&                                 GetSceneTimeRef() { return m_sceneTime; }
    const std::unique_ptr<::ZoomTool> & GetZoomTool() const { return m_zoomTool; }
    caustica::BindingCache &           GetBindingCache();

    [[nodiscard]] caustica::GpuDevice& GetGpuDevice() const { return *m_gpuDevice; }
    [[nodiscard]] nvrhi::IDevice* GetDevice() const { return m_gpuDevice->GetDevice(); }
    [[nodiscard]] uint32_t GetFrameIndex() const { return m_gpuDevice->GetFrameIndex(); }

    GLFWwindow *                            GetGLFWWindow() const { return m_gpuDevice->GetWindow(); }

    int                                     GetAccumulationSampleIndex() const;

    uint2                                   GetRenderSize() const;
    uint2                                   GetDisplaySize() const;

    const std::unique_ptr<CaptureScriptManager> & GetCaptureScriptManager() const { return m_captureScriptManager; }

#if CAUSTICA_WITH_PYTHON
    const std::unique_ptr<PythonScripting> & GetPythonScripting() const { return m_pythonScripting; }
#endif

    bool                                    HasAsyncLoadingInProgress() const
    {
        return m_sessionDiagnostics.asyncLoadingInProgress
            || m_renderState.Invalidation.ShaderAndACRefreshDelayedRequest > 0;
    }

    bool                                    AccumulationCompleted() const;

protected:
    caustica::render::RenderSessionState& m_sessionState;
    PathTracerSettings& m_settings;
    caustica::render::RenderRuntimeState& m_renderState;
    EditorUIState& m_editor;
    EditorUIData* m_editorUi = nullptr;
    caustica::render::SessionDiagnostics& m_sessionDiagnostics;

    std::unique_ptr<CaptureScriptManager>       m_captureScriptManager;
#if CAUSTICA_WITH_PYTHON
    std::unique_ptr<PythonScripting>            m_pythonScripting;
#endif

    caustica::render::PathTracingWorldRenderer* m_worldRenderer = nullptr;
    EditorInputRouter m_inputRouter;

    const caustica::PlanarView& GetView() const;

private:
    void                                    UpdateCameraFromScene( const std::shared_ptr<caustica::PerspectiveCamera> & sceneCamera );
    void                                    RefreshEnvironmentMapMediaList();
    void                                    SyncInputRouterContext();

    std::shared_ptr<caustica::RootFileSystem> m_RootFS;

    caustica::GpuDevice*                        m_gpuDevice = nullptr;

    SceneManager*                               m_sceneManager = nullptr;
    caustica::RenderCore*                       m_renderCore = nullptr;

    double                                      m_sceneTime = 0.;
    float                                       m_lastDeltaTime = 0.0f;

    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
    std::shared_ptr<caustica::TextureLoader> m_TextureLoader;
    std::shared_ptr<caustica::CommonRenderPasses> m_CommonPasses;
    caustica::BindingCache* m_bindingCache = nullptr;
    std::shared_ptr<caustica::DescriptorTableManager> m_DescriptorTable;

    caustica::render::SceneLightingPasses*        m_lightingPasses = nullptr;
    caustica::render::SceneRayTracingResources*   m_rayTracingResources = nullptr;
    caustica::render::SceneGaussianSplatPasses*   m_gaussianSplatPasses = nullptr;

    std::map<std::shared_ptr<caustica::LoadedTexture>, TextureCompressionType> m_uncompressedTextures;

    std::string                                 m_fpsInfo;
    bool                                        m_windowIsInFocus = true;

    std::unique_ptr<::GameScene>            m_sampleGame;

    ProgressBar                                 m_progressLoading;

    std::unique_ptr<::ZoomTool>             m_zoomTool;

    const CommandLineOptions&                   m_cmdLine;

};

} // namespace caustica::editor
