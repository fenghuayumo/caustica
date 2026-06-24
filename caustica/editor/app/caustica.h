#pragma once

#include <shaders/PathTracer/Config.h>
#include "SampleCommon/SampleCommon.h"

#include "SampleCommon/CommandLine.h"
#include "SampleUI.h"

#include <render/Core/SceneRender.h>
#include <platform/Input.h>
#include <core/vfs/VFS.h>
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
#include "SampleCommon/ExtendedScene.h"

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

class Sample : public caustica::SceneRender, public caustica::IInputHandler
{
    friend class Renderer;

    // static constexpr uint32_t c_PathTracerVariants   = 6; // see shaders.cfg and CreatePTPipeline for details on variants

public:
    using SceneRender::SceneRender;

    Sample(caustica::GpuDevice& deviceManager,
        const CommandLineOptions& cmdLine);
    virtual ~Sample();

    //std::shared_ptr<caustica::IFileSystem> GetRootFs() const                      { return m_RootFS; }
    std::shared_ptr<caustica::ShaderFactory> GetShaderFactory() const          { return m_shaderFactory; }
    std::shared_ptr<caustica::CommonRenderPasses> GetCommonPasses() const      { return m_CommonPasses; }
    nvrhi::ITexture*                       GetLdrColorTexture() const               { return m_renderTargets ? m_renderTargets->LdrColor.Get() : nullptr; }
    std::shared_ptr<caustica::Scene>   GetScene() const                        { return m_scene; }
    std::vector<std::string> const &        GetAvailableScenes() const              { return m_sceneFilesAvailable; }
    std::string                             GetCurrentSceneName() const             { return m_currentSceneName; }
    const DebugFeedbackStruct &             GetFeedbackData() const                 { return m_feedbackData; }
    const DeltaTreeVizPathVertex *          GetDebugDeltaPathTree() const           { return m_debugDeltaPathTree; }
    uint                                    GetSceneCameraCount() const             { return (uint)m_scene->GetSceneGraph()->GetCameras().size() + 1; }
    uint &                                  SelectedCameraIndex()                   { return m_selectedCameraIndex; }   // 0 is default fps free flight, above (if any) will just use current scene camera
    const std::unique_ptr<class GameScene> &     GetGame() const                   { return m_sampleGame; }

    void                                    UpdateSubInstanceContents();
    void                                    UploadSubInstanceData(nvrhi::ICommandList* commandList);
    
    void                                    SetUIPick()                             { m_pick = true; }

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

    float                                   GetCameraVerticalFOV() const            { return m_cameraVerticalFOV; }
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
    // Legacy input methods (called by IInputHandler overrides below)
    virtual bool                            KeyboardUpdate(int key, int scancode, int action, int mods);
    virtual bool                            MousePosUpdate(double xpos, double ypos);
    virtual bool                            MouseButtonUpdate(int button, int action, int mods);
    virtual bool                            MouseScrollUpdate(double xoffset, double yoffset);

    // IInputHandler overrides (registered with Input layer)
    bool onKeyEvent(int key, int scancode, int action, int mods) override
        { return KeyboardUpdate(key, scancode, action, mods); }
    bool onMouseMoveEvent(double xpos, double ypos) override
        { return MousePosUpdate(xpos, ypos); }
    bool onMouseButtonEvent(int button, int action, int mods) override
        { return MouseButtonUpdate(button, action, mods); }
    bool onMouseScrollEvent(double xoffset, double yoffset) override
        { return MouseScrollUpdate(xoffset, yoffset); }

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
    void                                    RebuildDirtyMeshAccelStructs(nvrhi::ICommandList* commandList);
    void                                    UpdateSkinnedBLASs(nvrhi::ICommandList* commandList, uint32_t frameIndex) const;
    void                                    BuildTLAS(nvrhi::ICommandList* commandList) const;
    void                                    TransitionMeshBuffersToReadOnly(nvrhi::ICommandList* commandList);
    void                                    BackBufferResizing() override;
    void                                    CreateRenderPasses(bool& exposureResetRequired, nvrhi::CommandListHandle initializeCommandList);
    void                                    PreUpdatePathTracing(bool resetAccum, nvrhi::CommandListHandle commandList);
    void                                    PostUpdatePathTracing();
    void                                    UpdatePathTracerConstants( PathTracerConstants & constants, const PathTracerCameraData & cameraData );
    void                                    RtxdiSetupFrame(nvrhi::IFramebuffer* framebuffer, PathTracerCameraData cameraData, uint2 renderDims);

    // Extendable sample interface
    virtual bool                            NeedsRasterPrecompute() { return false; } // TODO: do this in a nicer way, no time now
    virtual void                            SampleRenderCode(nvrhi::IFramebuffer* framebuffer, nvrhi::CommandListHandle commandList, const SampleConstants& constants) = 0; // TODO: Rename this
    virtual void                            CreateRTPipelines() = 0;
    virtual void                            DestroyRTPipelines() = 0;
    virtual std::string                     GetMaterialSpecializationShader() const = 0;

    void                                    Denoise(nvrhi::IFramebuffer* framebuffer);
    void                                    PathTrace(nvrhi::IFramebuffer* framebuffer, const SampleConstants & constants);
    void                                    PreRender();
    void                                    StreamlinePreRender();
#if CAUSTICA_WITH_NATIVE_DLSS
    void                                    NativeDLSSPreRender();
#endif
    void                                    Render(nvrhi::IFramebuffer* framebuffer) override;
    void                                    PostProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset);

    void                                    PreUpdateLighting(nvrhi::CommandListHandle commandList, bool& needNewBindings);     // this can (re)allocate buffers depending on scene changes
    void                                    UpdateLighting(nvrhi::CommandListHandle commandList);                               // this will process and fill up all lighting buffers

    dm::float2                     ComputeCameraJitter( uint frameIndex );

    std::string                             GetResolutionInfo() const;
    std::string                             GetFPSInfo() const              { return m_fpsInfo; }

    void                                    DebugDrawLine( float3 start, float3 stop, float4 col1, float4 col2 );
    const caustica::FirstPersonCamera &   GetCurrentCamera( ) const { return m_camera; }
    const auto &                            GetCurrentView( ) const { return m_view; }

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

    int                                     GetAccumulationSampleIndex() const { return m_accumulationSampleIndex; }

    uint2                                   GetRenderSize() const               { return m_renderSize;  } // native render resolution
    uint2                                   GetDisplaySize() const              { return m_displaySize; } // final output resolution

    const std::unique_ptr<CaptureScriptManager> & GetCaptureScriptManager() const { return m_captureScriptManager; }

#if CAUSTICA_WITH_PYTHON
    const std::unique_ptr<PythonScripting> & GetPythonScripting() const { return m_pythonScripting; }
#endif

    bool                                    HasAsyncLoadingInProgress() const   { return m_asyncLoadingInProgress || m_ui.ShaderAndACRefreshDelayedRequest > 0; }

    bool                                    AccumulationCompleted() const       { return m_accumulationCompleted; }

protected:
    // Called when render targets have been recreated (e.g. after window resize)
    virtual void OnRenderTargetsRecreated() { }

    // Called during binding set creation to allow derived classes to add custom bindings
    // The reflection texture slots (t80-t83, b3) have null placeholders by default
    virtual void AddCustomBindings(nvrhi::BindingSetDesc& bindingSetDesc) { }
    
    // Invalidates the binding set, forcing recreation on next frame
    // Call this from derived classes when custom bindings need to be updated
    void InvalidateBindingSet() { m_bindingSet = nullptr; }
    void RecreateBindingSet();
    
    // all UI-tweakable settings are here
    SampleUIData& m_ui;
    std::unique_ptr<RtxdiPass>                  m_rtxdiPass;
    std::unique_ptr<RenderTargets>              m_renderTargets;
    nvrhi::BindingLayoutHandle                  m_bindingLayout;
    nvrhi::BindingLayoutHandle                  m_bindlessLayout;
    nvrhi::BindingSetHandle                     m_bindingSet;

    std::shared_ptr<caustica::DescriptorTableManager> m_DescriptorTable;

    // QoL accessors for derived samples
    const caustica::PlanarView& GetView() const { return *m_view; }
    std::shared_ptr<class PTPipelineBaker>  GetRTPipelineBaker() const { return m_ptPipelineBaker; }
    std::shared_ptr<ComputePipelineBaker>   GetComputePipelineBaker() const { return m_computePipelineBaker; }

    // TODO: These are specific to the advanced sample. Should move them there
    std::shared_ptr<class PTPipelineVariant>    m_ptPipelineReference;
    std::shared_ptr<class PTPipelineVariant>    m_ptPipelineBuildStablePlanes;
    std::shared_ptr<class PTPipelineVariant>    m_ptPipelineFillStablePlanes;

    std::shared_ptr<class PTPipelineVariant>    m_ptPipelineTestRaygenPPHDR;
    std::shared_ptr<class PTPipelineVariant>    m_ptPipelineEdgeDetection;

    std::unique_ptr<CaptureScriptManager>       m_captureScriptManager;
#if CAUSTICA_WITH_PYTHON
    std::unique_ptr<PythonScripting>            m_pythonScripting;
#endif

private:
    struct GaussianSplatSceneObject;

    void                                    UpdateCameraFromScene( const std::shared_ptr<caustica::PerspectiveCamera> & sceneCamera );
    void                                    UpdateViews( nvrhi::IFramebuffer* framebuffer );
    void                                    DenoisedScreenshot( nvrhi::ITexture * framebufferTexture ) const;
    void                                    ResetReferenceOIDN();
    void                                    ApplyReferenceOIDN();
    void                                    PostProcessPreToneMapping(nvrhi::ICommandList* commandList, const caustica::ICompositeView& compositeView);
    void                                    PostProcessPostToneMapping(nvrhi::ICommandList* commandList, const caustica::ICompositeView& compositeView);
    void                                    LoadGaussianSplatsFromScene();
    bool                                    AttachGaussianSplatToScene(const std::filesystem::path& fileName, bool convertRdfToRub);
    void                                    PrepareGaussianSplatPass(GaussianSplatPass& pass);
    void                                    UpdateGaussianSplatUIState();
    uint32_t                                GetTotalGaussianSplatCount() const;
    void                                    RenderGaussianSplats(bool renderToOutputColor);
    void                                    AccumulateGaussianSplats(const caustica::IView& splatView);
    void                                    BuildGaussianSplatEmissionProxyList();
    void                                    RefreshEnvironmentMapMediaList();
#if CAUSTICA_WITH_NATIVE_DLSS
    bool                                    EvaluateNativeDLSS(bool reset);
#endif

private:
    struct GaussianSplatSceneObject
    {
        std::shared_ptr<GaussianSplat> splat;
        std::weak_ptr<caustica::SceneGraphNode> node;
        std::unique_ptr<GaussianSplatPass> pass;
    };

    std::filesystem::path                       ResolveGaussianSplatPath(const GaussianSplat& splat) const;
    dm::float4x4                       GetGaussianSplatObjectToWorld(const GaussianSplatSceneObject& object) const;
    GaussianSplatSceneObject*                   GetPrimaryGaussianSplatObject();
    const GaussianSplatSceneObject*             GetPrimaryGaussianSplatObject() const;

    std::shared_ptr<caustica::RootFileSystem> m_RootFS;

    // Scene management helper (queries, discovery)
    std::unique_ptr<SceneManager>               m_sceneManager;
    std::unique_ptr<Renderer>                   m_renderer;

    // scene
    std::vector<std::string>                    m_sceneFilesAvailable;
    std::string                                 m_currentSceneName;
    std::filesystem::path                       m_currentScenePath;
    std::string                                 m_inlineSceneJson;
    std::shared_ptr<ExtendedScene>              m_scene;
    double                                      m_sceneTime = 0.;           // if m_ui.LoopLongestAnimation then it loops with longest animation
    float                                       m_lastDeltaTime = 0.0f;
    uint                                        m_selectedCameraIndex = 0;  // 0 is first person camera, the rest (if any) are scene cameras

    // device setup
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
    std::shared_ptr<caustica::CommonRenderPasses> m_CommonPasses;
    std::unique_ptr<caustica::BindingCache> m_bindingCache;
    nvrhi::CommandListHandle                    m_commandList;

    std::unique_ptr<caustica::render::TemporalAntiAliasingPass> m_temporalAntiAliasingPass;

    // rendering
    std::vector <std::shared_ptr<caustica::Light>> m_lights;
    std::unique_ptr<caustica::render::BloomPass>   m_bloomPass;
    std::unique_ptr<ToneMappingPass>            m_toneMappingPass;
    nvrhi::BufferHandle                         m_constantBuffer;

    std::vector<SubInstanceData>                m_subInstanceData;
    nvrhi::BufferHandle                         m_subInstanceBuffer;            // per-instance-geometry data, indexed with (InstanceID()+GeometryIndex())
    uint                                        m_subInstanceCount;

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
    std::shared_ptr<class PTPipelineBaker>      m_ptPipelineBaker;
    std::shared_ptr<ComputePipelineBaker>       m_computePipelineBaker;

    // utility
    std::shared_ptr<class GPUSort>              m_gpuSort;
    std::vector<GaussianSplatSceneObject>       m_gaussianSplatSceneObjects;
    std::vector<GaussianSplatEmissionProxy>     m_gaussianSplatEmissionProxies;
    std::string                                 m_gaussianSplatFileNameSummary;
    bool                                        m_initialGaussianSplatAttached = false;
    nvrhi::TextureHandle                        m_gaussianSplatCurrentColor;
    nvrhi::TextureHandle                        m_gaussianSplatAccumulatedColor;
    std::unique_ptr<AccumulationPass>           m_gaussianSplatAccumulationPass;
    int                                         m_gaussianSplatTemporalSampleIndex = 0;
    bool                                        m_gaussianSplatTemporalReset = true;

    // raytracing basics
    nvrhi::rt::AccelStructHandle                m_topLevelAS;
    std::vector<std::shared_ptr<caustica::MeshInfo>> m_meshesPendingAccelRebuild;

    // camera
    caustica::FirstPersonCamera               m_camera;
    std::shared_ptr<caustica::PlanarView>  m_view;
    std::shared_ptr<caustica::PlanarView>  m_viewPrevious;
    float                                       m_cameraVerticalFOV = 60.0f;
    float                                       m_cameraZNear = 0.001f;
    float                                       m_cameraZFar = 100000.0f;
    bool                                        m_cameraUseCustomIntrinsics = false;
    dm::float4                                  m_cameraIntrinsics = dm::float4(0.f); // fx, fy, cx, cy
    dm::float2                                  m_cameraIntrinsicsViewport = dm::float2(0.f);
    dm::float3                                  m_lastCamPos = { 0,0,0 };
    dm::float3                                  m_lastCamDir = { 0,0,0 };
    dm::float3                                  m_lastCamUp = { 0,0,0 };


    std::chrono::high_resolution_clock::time_point m_benchStart = std::chrono::high_resolution_clock::now();
    std::chrono::high_resolution_clock::time_point m_benchLast = std::chrono::high_resolution_clock::now();
    int                                         m_benchFrames = 0;

    std::shared_ptr<PostProcess>                m_postProcess;

    //Debugging and debug viz
    nvrhi::BufferHandle                         m_feedback_Buffer_Gpu;
    nvrhi::BufferHandle                         m_feedback_Buffer_Cpu;
    nvrhi::BufferHandle                         m_debugLineBufferCapture;
    nvrhi::BufferHandle                         m_debugLineBufferDisplay;
    nvrhi::ShaderHandle                         m_linesVertexShader;
    nvrhi::ShaderHandle                         m_linesPixelShader;

    std::vector<DebugLineStruct>                m_cpuSideDebugLines;

    nvrhi::InputLayoutHandle                    m_linesInputLayout;
    nvrhi::GraphicsPipelineHandle               m_linesPipeline;
    nvrhi::BindingLayoutHandle                  m_linesBindingLayout;
    nvrhi::BindingSetHandle                     m_linesBindingSet;
    uint2                                       m_pickPosition = 0u;
    bool                                        m_pick = false;         // right-click: pixel debug + material and instance picking
    bool                                        m_pickInstance = false;  // instance picking for Inspector
    DebugFeedbackStruct                         m_feedbackData;

    DeltaTreeVizPathVertex                      m_debugDeltaPathTree[cDeltaTreeVizMaxVertices];
    nvrhi::BufferHandle                         m_debugDeltaPathTree_Gpu;
    nvrhi::BufferHandle                         m_debugDeltaPathTree_Cpu;
    nvrhi::BufferHandle                         m_debugDeltaPathTreeSearchStack;

    // The command line settings are here
    const CommandLineOptions&                   m_cmdLine;

    // path tracing
    int                                         m_accumulationSampleIndex = 0;  // accumulated so far in the past, so if 0 this is the first.

    uint64_t                                    m_frameIndex = 0;
    uint                                        m_sampleIndex = 0;            // per-frame sampling index; same as m_accumulationSampleIndex in accumulation mode, otherwise in realtime based on frameIndex%something 
    SampleConstants                             m_currentConstants = {};

    std::unique_ptr<NrdIntegration>             m_nrd[cStablePlaneCount];       // reminder: when switching between ReLAX/ReBLUR, change settings, reset these to 0 and they'll get re-created in CreateRenderPasses!
    std::unique_ptr<AccumulationPass>           m_accumulationPass;
    std::unique_ptr<OidnDenoiser>               m_oidnDenoiser;
    nvrhi::TextureHandle                        m_oidnDenoisedOutput;
    bool                                        m_oidnDenoisedOutputValid = false;
    bool                                        m_oidnDenoiserFailed = false;
    std::shared_ptr<ShaderDebug>                m_shaderDebug;

    std::shared_ptr<DenoisingGuidesBaker>       m_denoisingGuidesBaker;

    nvrhi::ShaderHandle                         m_exportVBufferCS;
    nvrhi::ComputePipelineHandle                m_exportVBufferPSO;

    // texture compression: used but not compressed textures
    std::map<std::shared_ptr<caustica::LoadedTexture>, TextureCompressionType> m_uncompressedTextures;

#if CAUSTICA_WITH_STREAMLINE
    caustica::StreamlineInterface::DLSSSettings   m_recommendedDLSSSettings = {};
    caustica::StreamlineInterface::DLSSRROptions  m_lastDLSSRROptions;
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
    std::unique_ptr<caustica::render::DLSS>     m_nativeDLSS;
#endif
    uint2                                       m_renderSize;   // native render resolution
    uint2                                       m_displaySize;  // final output resolution
    float                                       m_displayAspectRatio = 1.0f;

    std::string                                 m_fpsInfo;
    bool                                        m_windowIsInFocus = true;

    std::unique_ptr<class GameScene>            m_sampleGame;

    ProgressBar                                 m_progressLoading;
    ProgressBar                                 m_progressInitializingRenderer;

    std::unique_ptr<class ZoomTool>             m_zoomTool;

    bool                                        m_asyncLoadingInProgress = false;
    bool                                        m_accumulationCompleted = false;
};

