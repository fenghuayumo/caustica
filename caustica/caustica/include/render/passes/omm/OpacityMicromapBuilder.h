#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <render/core/BindingCache.h>
#include <render/core/DescriptorTableManager.h>
#include <rhi/rhi.h>
#include <math/math.h>
#include <scene/SceneRenderData.h>

#include <render/core/ComputePass.h>

#include <shaders/Misc/OmmGeometryDebugData.hlsli>

class MaterialGpuCache;

namespace caustica
{
    class FramebufferFactory;
    class TextureLoader;
    class TextureHandle;
    class ShaderFactory;
    namespace render { class RenderDevice; struct SceneGpuResources; struct MeshGpuRecord; }
    struct ImageAsset;
}

// =============================================================================
// Legacy extension type names now map to CPU authoring types; all OMM GPU state
// is held by render::MeshGpuRecord.
using MeshGeometryEx        = caustica::MeshGeometry;
using MeshInfoEx            = caustica::MeshInfo;

enum class OpacityMicroMapDebugView : uint32_t
{
    Disabled,
    InWorld,
    Overlay,
};

struct OpacityMicroMapUIData
{
    struct BuildState
    {
        // ~~ Application is expected to tweak these settings ~~ 
        int MaxSubdivisionLevel = 12;
        bool EnableDynamicSubdivision = true;
        float DynamicSubdivisionScale = 1.f;
        caustica::rhi::rt::OpacityMicromapBuildFlags Flag = caustica::rhi::rt::OpacityMicromapBuildFlags::FastTrace;
        caustica::rhi::rt::OpacityMicromapFormat Format = caustica::rhi::rt::OpacityMicromapFormat::OC1_4_State;
        int AlphaCutoffGT = 1 /*Opaque*/;
        int AlphaCutoffLE = 0 /*Transparent*/;

        // ~~ Debug settings, application is expected to leave to default ~~ 
        bool ComputeOnly = true;
        bool LevelLineIntersection = true;
        bool EnableTexCoordDeduplication = true;
        bool Force32BitIndices = false;
        bool EnableNsightDebugMode = false;
        bool EnableSpecialIndices = true;
        int MaxOmmArrayDataSizeInMB = 100;

        bool operator == (const BuildState& other) const = default;
    };

    bool                                Enable = false;
    bool                                Force2State = false;
    bool                                OnlyOMMs = false;

    // Amortize the builds over multiple frames
    std::optional<BuildState>           ActiveState;
    BuildState                          DesiredState;
    bool                                TriggerRebuild = true;

    // --- Stats --- 
    // build progress of active tasks
    uint32_t                            BuildsLeftInQueue = 0;
    uint32_t                            BuildsQueued = 0;

    OpacityMicroMapDebugView            DebugView = OpacityMicroMapDebugView::Disabled;
};

class OpacityMicromapBuilder
{
public:
    OpacityMicromapBuilder(caustica::rhi::DeviceHandle device,
        std::shared_ptr<caustica::DescriptorTableManager> descriptorTableManager,
        std::shared_ptr<caustica::TextureLoader> textureCache,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory);
    ~OpacityMicromapBuilder();

    void                            createRenderPasses(caustica::rhi::BindingLayoutHandle bindlessLayout, caustica::render::RenderDevice& renderDevice);
    void                            setMaterialGpuCache(MaterialGpuCache* materials);

    bool                            update(caustica::rhi::ICommandList& commandList,
                                           const caustica::scene::SceneRenderData& renderData);

    OpacityMicroMapUIData &         uiData()    { return m_uiData; }
    bool                            debugGUI(float indent, const caustica::scene::SceneRenderData& renderData);

    void                            sceneLoaded(size_t geometryCount);
    void                            sceneUnloading();

    void                            createOpacityMicromaps(const caustica::scene::SceneRenderData& renderData);
    void                            destroyOpacityMicromaps(caustica::rhi::ICommandList& commandList,
                                                            const caustica::scene::SceneRenderData& renderData);
    void                            buildOpacityMicromaps(caustica::rhi::ICommandList& commandList,
                                                          const caustica::scene::SceneRenderData& renderData);
    void                            writeGeometryDebugBuffer(caustica::rhi::ICommandList& commandList);
    void                            updateDebugGeometry(
                                        const caustica::scene::MeshRenderResourceSnapshot& mesh,
                                        const caustica::render::MeshGpuRecord& meshGpu);

    [[nodiscard]] caustica::rhi::IBuffer*   getGeometryDebugBuffer() const { return m_geometryDebugBuffer; }
    [[nodiscard]] bool              shouldUseRayTracingOpacityMicromaps() const { return m_uiData.Enable; }

    void                            setGlobalShaderMacros(std::vector<caustica::ShaderMacro> & macros);


private:
    void                            ensureGeometryDebugCapacity(size_t geometryCount);

    caustica::rhi::DeviceHandle             m_device;
    caustica::render::SceneGpuResources* m_sceneGpuResources = nullptr;
    MaterialGpuCache*               m_materialGpuCache = nullptr;
    uint64_t                        m_materialStateRevision = 0;
    bool                            m_waitingForMaterialTextures = false;
    std::shared_ptr<caustica::TextureLoader> m_textureCache;
    std::shared_ptr<caustica::FramebufferFactory> m_framebufferFactory;
    std::shared_ptr<caustica::DescriptorTableManager> m_descriptorTableManager;
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;

    caustica::rhi::BindingLayoutHandle      m_commonBindingLayout;
    caustica::rhi::BindingLayoutHandle      m_bindlessLayout;
    caustica::BindingCache     m_bindingCache;

    ComputePass                     m_examplePass;

    std::unique_ptr<class OmmBuildQueue>  m_ommBuildQueue;

    OpacityMicroMapUIData           m_uiData;

    std::vector<class GeometryDebugData> m_geometryDebugDataPtr;
    caustica::rhi::BufferHandle             m_geometryDebugBuffer;
};
