#pragma once

#include <shaders/PathTracer/Config.h>
#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <events/event.h>
#include <events/key_event.h>
#include <events/mouse_event.h>
#include <render/Core/ScopedPerfMarker.h>
#include <render/Core/TextureUtils.h>
#include <render/WorldRenderer/WorldRendererServices.h>

#include <core/command_line.h>
#include "SampleUI.h"

#include <backend/GpuDevice.h>

namespace caustica::render { class PathTracingWorldRenderer; }

#include <functional>
#include <chrono>
#include <core/vfs/VFS.h>
#include <render/Core/RenderCore.h>
#include <render/Core/CameraController.h>
#include <render/Passes/Geometry/BloomPass.h>
#include <scene/camera/Camera.h>
#include <engine/SceneManager.h>
#include <assets/cache/TextureCache.h>
#include <render/Core/CommonRenderPasses.h>
#if CAUSTICA_WITH_NATIVE_DLSS
#include <render/Passes/Geometry/DLSS.h>
#endif

#include <render/Passes/RTXDI/RtxdiPass.h>
#include <render/Passes/Denoisers/NrdIntegration.h>
//#include "PathTracer/StablePlanes.hlsli"
#if CAUSTICA_WITH_STREAMLINE
#include <engine/StreamlineInterface.h>
#endif

#include "render/Core/RenderTargets.h"
#include <render/Passes/PostProcess/PostProcess.h>
#include <shaders/SampleConstantBuffer.h>
#include <render/Passes/PostProcess/AccumulationPass.h>
#include <render/Passes/Gaussian/GaussianSplatEmissionProxy.h>
#include <scene/Scene.h>

#include <render/Passes/Lighting/Distant/EnvMapBaker.h>
#include <render/Passes/Lighting/LightsBaker.h>

#include <render/Passes/Debug/ShaderDebug.h>

#include <map>

class DenoisingGuidesBaker;
class CaptureScriptManager;
class ComputePipelineBaker;
class ComputeShaderVariant;
class OidnDenoiser;
#if CAUSTICA_WITH_PYTHON
class PythonScripting;
#endif
class GaussianSplatPass;

// Scene editor shell (mesh edit, Inspector, Capture). GPU path tracing is owned by EditorApplication.
class PathTracerApp : public caustica::render::IWorldRendererPipelineHooks
{
    struct GaussianSplatSceneObject;

public:
    PathTracerApp(const CommandLineOptions& cmdLine,
        SampleUIData& ui);
    ~PathTracerApp() override;

    void setGpuDevice(caustica::GpuDevice& dm) { m_gpuDevice = &dm; }

    void SetLatewarpOptions() { }
    bool ShouldAnimateUnfocused() { return false; }
    bool SupportsDepthBuffer() { return true; }
    void BackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) { (void)width; (void)height; (void)sampleCount; }
    void DisplayScaleChanged(float scaleX, float scaleY) { (void)scaleX; (void)scaleY; }

    //std::shared_ptr<caustica::IFileSystem> GetRootFs() const                      { return m_RootFS; }
    std::shared_ptr<caustica::ShaderFactory> GetShaderFactory() const          { return m_shaderFactory; }
    std::shared_ptr<caustica::CommonRenderPasses> GetCommonPasses() const { return m_CommonPasses; }
    std::shared_ptr<caustica::TextureCache> GetTextureCache() const { return m_TextureCache; }
    std::shared_ptr<caustica::DescriptorTableManager> GetDescriptorTable() const { return m_DescriptorTable; }
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
    const std::unique_ptr<class GameScene> &     GetGame() const                   { return m_sampleGame; }

    SampleUIData& GetUIData() { return m_ui; }
    const SampleUIData& GetUIData() const { return m_ui; }
    PathTracerSettings& GetPathTracerSettings() { return m_settings; }
    const PathTracerSettings& GetPathTracerSettings() const { return m_settings; }
    EditorUIState& GetEditorUIState() { return m_editor; }
    const EditorUIState& GetEditorUIState() const { return m_editor; }

    void                                    UpdateSubInstanceContents();
    void                                    UploadSubInstanceData(nvrhi::ICommandList* commandList);
    
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
        const std::shared_ptr<caustica::TextureCache>& textureCache);
    void AttachSceneServices(SceneManager& sceneManager, caustica::RenderCore& renderCore);
    void Init(const std::string& preferredScene,
        const std::shared_ptr<caustica::ShaderFactory>& shaderFactory);

    void AttachWorldRenderer(caustica::render::PathTracingWorldRenderer* renderer);
    [[nodiscard]] caustica::render::PathTracingWorldRenderer* GetWorldRenderer() const { return m_worldRenderer; }

    void initStreamlineAndWindow();  // call after setGpuDevice()

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

    void                                    FillPTPipelineGlobalMacros(std::vector<caustica::ShaderMacro> & macros);
    bool                                    CreatePTPipeline(caustica::ShaderFactory& shaderFactory);
    void                                    DestroyOpacityMicromaps(nvrhi::ICommandList* commandList);
    void                                    CreateOpacityMicromaps();
    void                                    CreateBlases(nvrhi::ICommandList* commandList);
    void                                    CreateTlas(nvrhi::ICommandList* commandList);
    void                                    CreateAccelStructs(nvrhi::ICommandList* commandList);
    void                                    RecreateAccelStructs(nvrhi::ICommandList* commandList);
    void                                    RequestMeshAccelRebuild(const std::shared_ptr<caustica::MeshInfo>& mesh);

    dm::float2                              ComputeCameraJitter(uint frameIndex);

    // --- IWorldRendererPipelineHooks ---
    bool needsRasterPrecompute() override { return false; }
    std::string getMaterialSpecializationShader() const override;
    void fillPTPipelineGlobalMacros(std::vector<caustica::ShaderMacro>& macros) override;
    void sampleRenderCode(nvrhi::IFramebuffer* framebuffer, nvrhi::CommandListHandle commandList, const SampleConstants& constants) override;
    void addCustomBindings(nvrhi::BindingSetDesc& bindingSetDesc) override;
    void createRTPipelines() override;
    void onRenderTargetsRecreated() override;
    void prepareGaussianSplatPasses() override;
    void buildGaussianSplatEmissionProxyList() override;
    bool isGaussianSplatEmissionEnabled() const override;
    bool gaussianSplatObjectsEmpty() const override;
    caustica::render::WorldRendererGaussianSplatBinding getPrimaryGaussianSplatBinding() const override;
    void renderSceneGaussianSplats(nvrhi::ICommandList* commandList,
                                   const caustica::PlanarView& splatView,
                                   RenderTargets& renderTargets,
                                   const GaussianSplatRenderSettings& settings,
                                   bool& renderedAny) override;
    void updateViews(nvrhi::IFramebuffer* framebuffer) override;
    void recreateAccelStructs(nvrhi::ICommandList* commandList) override;
    void uploadSubInstanceData(nvrhi::ICommandList* commandList) override;
    void collectUncompressedTextures() override;
    dm::float2 computeCameraJitter(uint frameIndex) override;
    bool consumeShaderReloadRequest() override;
    bool& accelerationStructRebuildRequested() override;
    bool hasActivePickRequest() const override;
    bool showDeltaTree() const override;
    bool pickMaterialRequested() const override;
    bool pickInstanceRequested() const override;
    void clearPickRequests() override;
    void resolvePickFeedback(const DebugFeedbackStruct& feedback) override;
    bool consumeExperimentalPhotoScreenshot() override;
    void captureScriptPreRender() override;
    void captureScriptPostRender(std::function<bool(const char* fileName)> saveTexture) override;
    class ZoomTool* getOrCreateZoomTool() override;

    // --- Pipeline variant accessors ---
    std::shared_ptr<class PTPipelineVariant>& PtPipelineReference();
    std::shared_ptr<class PTPipelineVariant>& PtPipelineBuildStablePlanes();
    std::shared_ptr<class PTPipelineVariant>& PtPipelineFillStablePlanes();
    std::shared_ptr<class PTPipelineVariant>& PtPipelineTestRaygenPPHDR();
    std::shared_ptr<class PTPipelineVariant>& PtPipelineEdgeDetection();

    // Render entry points (formerly on AdvancedPathTracer)
    void Render(nvrhi::IFramebuffer* framebuffer);
    void BackBufferResizing();
    void PathTrace(nvrhi::IFramebuffer* framebuffer, const SampleConstants& constants);
    void Denoise(nvrhi::IFramebuffer* framebuffer);
    void PostProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset);
    std::string GetMaterialSpecializationShader() const;
    bool NeedsRasterPrecompute() { return false; }

    // --- Input event handling (replaces PathTracerInputController) ---
    void onEvent(caustica::Event& event);

    std::string                             GetResolutionInfo() const;
    std::string                             GetFPSInfo() const              { return m_fpsInfo; }

    void                                    DebugDrawLine( float3 start, float3 stop, float4 col1, float4 col2 );
    const caustica::FirstPersonCamera &   GetCurrentCamera( ) const;
    const std::shared_ptr<caustica::PlanarView>& GetCurrentView() const;

    void                                    SetSceneTime( double sceneTime );
    double                                  GetSceneTime( );

    bool                                    IsEnvMapLoaded() const      { return true; } // with the new EnvMapBaker it's always present (just black)
    const std::string &                     GetEnvMapLocalPath() const { return m_envMapLocalPath; }
    const std::string &                     GetEnvMapOverrideSource() const { return m_envMapOverride; }
    void                                    SetEnvMapOverrideSource(const std::string & envMapOverride);
    const std::vector<std::filesystem::path> & GetEnvMapMediaList()     { return m_envMapMediaList; }

    std::shared_ptr<EnvMapBaker>&           GetEnvMapBaker() { return m_envMapBaker; }
    const std::shared_ptr<EnvMapBaker>&   GetEnvMapBaker() const { return m_envMapBaker; }
    std::shared_ptr<LightsBaker>&         GetLightsBaker() { return m_lightsBaker; }
    const std::shared_ptr<LightsBaker>&     GetLightsBaker() const { return m_lightsBaker; }
    std::shared_ptr<MaterialsBaker>&      GetMaterialsBaker() { return m_materialsBaker; }
    const std::shared_ptr<MaterialsBaker>&  GetMaterialsBaker() const { return m_materialsBaker; }
    std::shared_ptr<class OmmBaker>&        GetOMMBaker() { return m_ommBaker; }
    const std::shared_ptr<class OmmBaker>&    GetOMMBaker() const { return m_ommBaker; }
    std::shared_ptr<ComputePipelineBaker>&  GetComputePipelineBaker() { return m_computePipelineBaker; }
    const std::shared_ptr<ComputePipelineBaker>& GetComputePipelineBaker() const { return m_computePipelineBaker; }
    std::vector<std::shared_ptr<caustica::Light>>& GetLights() { return m_lights; }
    EnvMapSceneParams&                      GetEnvMapSceneParams() { return m_envMapSceneParams; }
    const EnvMapSceneParams&                GetEnvMapSceneParams() const { return m_envMapSceneParams; }
    std::string&                            GetEnvMapLocalPath() { return m_envMapLocalPath; }
    std::string&                            GetEnvMapOverrideSource() { return m_envMapOverride; }
    double&                                 GetSceneTimeRef() { return m_sceneTime; }
    std::vector<GaussianSplatEmissionProxy>& GetGaussianSplatEmissionProxies() { return m_gaussianSplatEmissionProxies; }
    ProgressBar&                            GetProgressInitializingRenderer() { return m_progressInitializingRenderer; }
    bool&                                   GetAsyncLoadingInProgressRef() { return m_asyncLoadingInProgress; }
    std::chrono::high_resolution_clock::time_point& GetBenchStart() { return m_benchStart; }
    std::chrono::high_resolution_clock::time_point& GetBenchLast() { return m_benchLast; }
    int&                                    GetBenchFrames() { return m_benchFrames; }
    const std::unique_ptr<class ZoomTool> & GetZoomTool() const { return m_zoomTool; }
    caustica::BindingCache &           GetBindingCache() { return *m_bindingCache; }

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

    bool                                    HasAsyncLoadingInProgress() const   { return m_asyncLoadingInProgress || m_editor.ShaderAndACRefreshDelayedRequest > 0; }

    bool                                    AccumulationCompleted() const;

protected:
    // Called when render targets have been recreated (e.g. after window resize)
    virtual void OnRenderTargetsRecreated() { }

    // Called during binding set creation to allow derived classes to add custom bindings
    // The reflection texture slots (t80-t83, b3) have null placeholders by default
    virtual void AddCustomBindings(nvrhi::BindingSetDesc& bindingSetDesc) { }
    
    // Invalidates the binding set, forcing recreation on next frame
    // Call this from derived classes when custom bindings need to be updated
    void InvalidateBindingSet();
    void RecreateBindingSet();

    void UpdateViews(nvrhi::IFramebuffer* framebuffer);
    void PrepareGaussianSplatPass(GaussianSplatPass& pass);
    void BuildGaussianSplatEmissionProxyList();
    [[nodiscard]] caustica::CameraUpdateParams makeCameraUpdateParams() const;
    GaussianSplatSceneObject* GetPrimaryGaussianSplatObject();
    const GaussianSplatSceneObject* GetPrimaryGaussianSplatObject() const;
    dm::float4x4 GetGaussianSplatObjectToWorld(const GaussianSplatSceneObject& object) const;
    class ZoomTool* GetOrCreateZoomTool();

    // Path tracing settings (render) and editor UI state are distinct slices of SampleUIData.
    PathTracerSettings& m_settings;
    EditorUIState& m_editor;
    SampleUIData& m_ui;

    std::unique_ptr<CaptureScriptManager>       m_captureScriptManager;
#if CAUSTICA_WITH_PYTHON
    std::unique_ptr<PythonScripting>            m_pythonScripting;
#endif

    caustica::render::PathTracingWorldRenderer* m_worldRenderer = nullptr;

    // QoL accessors for derived samples (delegate to world renderer)
    const caustica::PlanarView& GetView() const;
    std::shared_ptr<class PTPipelineBaker>  GetRTPipelineBaker() const;

private:
    void                                    UpdateCameraFromScene( const std::shared_ptr<caustica::PerspectiveCamera> & sceneCamera );
    void                                    LoadGaussianSplatsFromScene();
    bool                                    AttachGaussianSplatToScene(const std::filesystem::path& fileName, bool convertRdfToRub);
    void                                    UpdateGaussianSplatUIState();
    uint32_t                                GetTotalGaussianSplatCount() const;
    void                                    RefreshEnvironmentMapMediaList();

    // Input event handlers (formerly in PathTracerInputController)
    bool onKeyPressed(caustica::KeyPressedEvent& e);
    bool onKeyReleased(caustica::KeyReleasedEvent& e);
    bool onMouseMoved(caustica::MouseMovedEvent& e);
    bool onMouseButtonPressed(caustica::MouseButtonPressedEvent& e);
    bool onMouseButtonReleased(caustica::MouseButtonReleasedEvent& e);
    bool onMouseScrolled(caustica::MouseScrolledEvent& e);

    struct GaussianSplatSceneObject
    {
        std::shared_ptr<caustica::GaussianSplat> splat;
        std::weak_ptr<caustica::SceneGraphNode> node;
        std::unique_ptr<GaussianSplatPass> pass;
    };

    std::filesystem::path                       ResolveGaussianSplatPath(const caustica::GaussianSplat& splat) const;

    std::shared_ptr<caustica::RootFileSystem> m_RootFS;

    caustica::GpuDevice*                        m_gpuDevice = nullptr;

    // Scene / render services (owned by EditorApplication)
    SceneManager*                               m_sceneManager = nullptr;
    caustica::RenderCore*                       m_renderCore = nullptr;

    // scene timing
    double                                      m_sceneTime = 0.;           // if m_ui.LoopLongestAnimation then it loops with longest animation
    float                                       m_lastDeltaTime = 0.0f;

    // device setup
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
    std::shared_ptr<caustica::TextureCache> m_TextureCache;
    std::shared_ptr<caustica::CommonRenderPasses> m_CommonPasses;
    caustica::BindingCache* m_bindingCache = nullptr;
    std::shared_ptr<caustica::DescriptorTableManager> m_DescriptorTable;

    // lighting
    std::string                                 m_envMapLocalPath;
    
    std::filesystem::path                       m_envMapMediaFolder;
    std::vector<std::filesystem::path>          m_envMapMediaList;

    std::string                                 m_envMapOverride;

    std::shared_ptr<EnvMapBaker>                m_envMapBaker;
    EnvMapSceneParams                           m_envMapSceneParams;
    std::shared_ptr<LightsBaker>                m_lightsBaker;
    std::shared_ptr<class MaterialsBaker>       m_materialsBaker;
    std::shared_ptr<class OmmBaker>             m_ommBaker;
    std::shared_ptr<ComputePipelineBaker>       m_computePipelineBaker;

    // utility
    std::shared_ptr<class GPUSort>              m_gpuSort;
    std::vector<GaussianSplatSceneObject>       m_gaussianSplatSceneObjects;
    std::vector<GaussianSplatEmissionProxy>     m_gaussianSplatEmissionProxies;
    std::string                                 m_gaussianSplatFileNameSummary;
    bool                                        m_initialGaussianSplatAttached = false;

    std::chrono::high_resolution_clock::time_point m_benchStart = std::chrono::high_resolution_clock::now();
    std::chrono::high_resolution_clock::time_point m_benchLast = std::chrono::high_resolution_clock::now();
    int                                         m_benchFrames = 0;

    // texture compression: used but not compressed textures
    std::map<std::shared_ptr<caustica::LoadedTexture>, TextureCompressionType> m_uncompressedTextures;

    std::string                                 m_fpsInfo;
    bool                                        m_windowIsInFocus = true;

    std::unique_ptr<class GameScene>            m_sampleGame;

    ProgressBar                                 m_progressLoading;
    ProgressBar                                 m_progressInitializingRenderer;

    std::unique_ptr<class ZoomTool>             m_zoomTool;

    bool                                        m_asyncLoadingInProgress = false;

    // The command line settings are here
    const CommandLineOptions&                   m_cmdLine;

    // rendering (scene lights list used by lighting update)
    std::vector<std::shared_ptr<caustica::Light>> m_lights;
};

