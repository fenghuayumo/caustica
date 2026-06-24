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
#include <render/Core/ScopedPerfMarker.h>
#include <render/Core/TextureUtils.h>

#include <core/command_line.h>
#include "SampleUI.h"

#include <render/Core/SceneRender.h>
#include <core/vfs/VFS.h>
#include <render/Core/RenderCore.h>
#include <render/Passes/Geometry/BloomPass.h>
#include <scene/camera/Camera.h>
#include <engine/SceneManager.h>
#include "SampleCommon/Renderer.h"
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
class Renderer;
class PathTracingRenderer;

// Scene editor + main 3D render pass (IRenderPass). Owns scene editing, UI state,
// and delegates GPU path tracing to PathTracingRenderer.
class PathTracerApp : public caustica::SceneRender
{
    friend class Renderer;
    friend class PathTracerInputController;
    friend class PathTracingRenderer;

    // static constexpr uint32_t c_PathTracerVariants   = 6; // see shaders.cfg and CreatePTPipeline for details on variants

public:
    using SceneRender::SceneRender;

    PathTracerApp(caustica::GpuDevice& deviceManager,
        const CommandLineOptions& cmdLine,
        SampleUIData& ui);
    virtual ~PathTracerApp();

    //std::shared_ptr<caustica::IFileSystem> GetRootFs() const                      { return m_RootFS; }
    std::shared_ptr<caustica::ShaderFactory> GetShaderFactory() const          { return m_shaderFactory; }
    std::shared_ptr<caustica::CommonRenderPasses> GetCommonPasses() const      { return m_CommonPasses; }
    nvrhi::ITexture*                       GetLdrColorTexture() const;
    std::shared_ptr<caustica::Scene>   GetScene() const                        { return m_sceneManager->getScene(); }
    std::vector<std::string> const &        GetAvailableScenes() const              { return m_sceneManager->getAvailableScenes(); }
    std::string                             GetCurrentSceneName() const             { return m_sceneManager->getCurrentSceneName(); }
    const DebugFeedbackStruct &             GetFeedbackData() const;
    const DeltaTreeVizPathVertex *          GetDebugDeltaPathTree() const;
    uint                                    GetSceneCameraCount() const             { return (uint)GetScene()->GetSceneGraph()->GetCameras().size() + 1; }
    uint &                                  SelectedCameraIndex()                   { return m_renderCore.camera().selectedCameraIndex(); }   // 0 is default fps free flight, above (if any) will just use current scene camera
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

    float                                   GetCameraVerticalFOV() const            { return m_renderCore.camera().verticalFOV(); }
    void                                    SetCameraVerticalFOV(float cameraFOV);
    void                                    SetCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height);
    void                                    ClearCameraIntrinsics();

    float                                   GetAvgTimePerFrame() const;

    void                                    Init(const std::string& preferredScene, const std::shared_ptr<caustica::ShaderFactory>& shaderFactory);
    void                                    SetCurrentScene(const std::string& sceneName, bool forceReload = false);
    bool                                    LoadGaussianSplatFile(const std::filesystem::path& fileName, bool convertRdfToRub = true);
    uint32_t                                GetGaussianSplatCount() const;
    uint32_t                                GetGaussianSplatObjectCount() const;
    const std::string&                      GetGaussianSplatFileName() const;

    virtual void                            SceneUnloading() override;
    virtual bool                            LoadScene(std::shared_ptr<caustica::IFileSystem> fs, const std::filesystem::path& sceneFileName) override;
    virtual void                            SceneLoaded() override;
    virtual bool                            ShouldRenderUnfocused() override;
    virtual void                            Animate(float fElapsedTimeSeconds) override;

    void                                    FillPTPipelineGlobalMacros(std::vector<caustica::ShaderMacro> & macros);
    bool                                    CreatePTPipeline(caustica::ShaderFactory& shaderFactory);
    void                                    DestroyOpacityMicromaps(nvrhi::ICommandList* commandList);
    void                                    CreateOpacityMicromaps();
    void                                    CreateBlases(nvrhi::ICommandList* commandList);
    void                                    CreateTlas(nvrhi::ICommandList* commandList);
    void                                    CreateAccelStructs(nvrhi::ICommandList* commandList);
    void                                    RecreateAccelStructs(nvrhi::ICommandList* commandList);
    void                                    RequestMeshAccelRebuild(const std::shared_ptr<caustica::MeshInfo>& mesh);
    void                                    BackBufferResizing() override;
    void                                    Render(nvrhi::IFramebuffer* framebuffer) override;
    virtual bool                            NeedsRasterPrecompute() { return false; } // TODO: do this in a nicer way, no time now
    virtual void                            SampleRenderCode(nvrhi::IFramebuffer* framebuffer, nvrhi::CommandListHandle commandList, const SampleConstants& constants) = 0; // TODO: Rename this
    virtual void                            CreateRTPipelines() = 0;
    virtual void                            DestroyRTPipelines() = 0;
    virtual std::string                     GetMaterialSpecializationShader() const = 0;

    void                                    Denoise(nvrhi::IFramebuffer* framebuffer);
    void                                    PathTrace(nvrhi::IFramebuffer* framebuffer, const SampleConstants & constants);
    void                                    PostProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset);

    dm::float2                              ComputeCameraJitter(uint frameIndex);

    std::string                             GetResolutionInfo() const;
    std::string                             GetFPSInfo() const              { return m_fpsInfo; }

    void                                    DebugDrawLine( float3 start, float3 stop, float4 col1, float4 col2 );
    const caustica::FirstPersonCamera &   GetCurrentCamera( ) const { return m_renderCore.camera().camera(); }
    const auto &                            GetCurrentView( ) const { return m_renderCore.camera().view(); }

    void                                    SetSceneTime( double sceneTime );
    double                                  GetSceneTime( );

    bool                                    IsEnvMapLoaded() const      { return true; } // with the new EnvMapBaker it's always present (just black)
    const std::string &                     GetEnvMapLocalPath()        { return m_envMapLocalPath; }
    const std::string &                     GetEnvMapOverrideSource()   { return m_envMapOverride; }
    void                                    SetEnvMapOverrideSource(const std::string & envMapOverride);
    const std::vector<std::filesystem::path> & GetEnvMapMediaList()     { return m_envMapMediaList; }

    const std::shared_ptr<EnvMapBaker> &    GetEnvMapBaker() const { return m_envMapBaker; }
    const std::shared_ptr<LightsBaker> &    GetLightsBaker() const { return m_lightsBaker; }
    const std::shared_ptr<MaterialsBaker> & GetMaterialsBaker() const { return m_materialsBaker; }
    const std::shared_ptr<class OmmBaker> & GetOMMBaker() const { return m_ommBaker; }
    const std::unique_ptr<class ZoomTool> & GetZoomTool() const { return m_zoomTool; }
    caustica::BindingCache &           GetBindingCache() { return *m_bindingCache; }

    GLFWwindow *                            GetGLFWWindow() const { return GetGpuDevice()->GetWindow(); }

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
    
    // Path tracing settings (render) and editor UI state are distinct slices of SampleUIData.
    PathTracerSettings& m_settings;
    EditorUIState& m_editor;
    SampleUIData& m_ui;

    std::unique_ptr<CaptureScriptManager>       m_captureScriptManager;
#if CAUSTICA_WITH_PYTHON
    std::unique_ptr<PythonScripting>            m_pythonScripting;
#endif

    std::unique_ptr<PathTracingRenderer>        m_pathTracingRenderer;

    // QoL accessors for derived samples (delegate to PathTracingRenderer)
    const caustica::PlanarView& GetView() const { return *m_renderCore.camera().view(); }
    std::shared_ptr<class PTPipelineBaker>  GetRTPipelineBaker() const;
    std::shared_ptr<ComputePipelineBaker>   GetComputePipelineBaker() const { return m_computePipelineBaker; }

    std::shared_ptr<class PTPipelineVariant>& PtPipelineReference();
    std::shared_ptr<class PTPipelineVariant>& PtPipelineBuildStablePlanes();
    std::shared_ptr<class PTPipelineVariant>& PtPipelineFillStablePlanes();
    std::shared_ptr<class PTPipelineVariant>& PtPipelineTestRaygenPPHDR();
    std::shared_ptr<class PTPipelineVariant>& PtPipelineEdgeDetection();

private:
    struct GaussianSplatSceneObject;

    void                                    UpdateCameraFromScene( const std::shared_ptr<caustica::PerspectiveCamera> & sceneCamera );
    void                                    UpdateViews( nvrhi::IFramebuffer* framebuffer );
    void                                    LoadGaussianSplatsFromScene();
    bool                                    AttachGaussianSplatToScene(const std::filesystem::path& fileName, bool convertRdfToRub);
    void                                    PrepareGaussianSplatPass(GaussianSplatPass& pass);
    void                                    UpdateGaussianSplatUIState();
    uint32_t                                GetTotalGaussianSplatCount() const;
    void                                    BuildGaussianSplatEmissionProxyList();
    void                                    RefreshEnvironmentMapMediaList();

private:
    struct GaussianSplatSceneObject
    {
        std::shared_ptr<caustica::GaussianSplat> splat;
        std::weak_ptr<caustica::SceneGraphNode> node;
        std::unique_ptr<GaussianSplatPass> pass;
    };

    std::filesystem::path                       ResolveGaussianSplatPath(const caustica::GaussianSplat& splat) const;
    dm::float4x4                       GetGaussianSplatObjectToWorld(const GaussianSplatSceneObject& object) const;
    GaussianSplatSceneObject*                   GetPrimaryGaussianSplatObject();
    const GaussianSplatSceneObject*             GetPrimaryGaussianSplatObject() const;

    std::shared_ptr<caustica::RootFileSystem> m_RootFS;

    // Scene management + render orchestrator (engine)
    std::unique_ptr<SceneManager>               m_sceneManager;
    caustica::RenderCore                           m_renderCore;
    std::unique_ptr<Renderer>                   m_renderer;

    // scene timing
    double                                      m_sceneTime = 0.;           // if m_ui.LoopLongestAnimation then it loops with longest animation
    float                                       m_lastDeltaTime = 0.0f;

    // device setup
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
    std::shared_ptr<caustica::CommonRenderPasses> m_CommonPasses;
    std::unique_ptr<caustica::BindingCache> m_bindingCache;
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

    std::unique_ptr<PathTracerInputController>  m_inputController;

    bool                                        m_asyncLoadingInProgress = false;

    // The command line settings are here
    const CommandLineOptions&                   m_cmdLine;

    // rendering (scene lights list used by lighting update)
    std::vector<std::shared_ptr<caustica::Light>> m_lights;
};

