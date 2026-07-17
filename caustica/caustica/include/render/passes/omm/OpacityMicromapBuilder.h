#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include <render/core/BindingCache.h>
#include <render/core/DescriptorTableManager.h>
#include <rhi/nvrhi.h>
#include <math/math.h>
#include <scene/SceneTypes.h>

#include <render/core/ComputePass.h>

#include <shaders/Misc/OmmGeometryDebugData.hlsli>

namespace caustica
{
    class FramebufferFactory;
    class TextureLoader;
    class TextureHandle;
    class ShaderFactory;
    template<typename T>
    class ResourceTracker;
    namespace render { class RenderDevice; }
    struct ImageAsset;
}

// =============================================================================
// MeshGeometryEx / MeshInfoEx �?Now merged into base types in SceneTypes.h.
//
// MeshGeometry now has DebugData (MeshGeometryDebugData).
// MeshInfo now has AccelStructOMM, OpacityMicroMaps,
//   DeformationSourcePositionIndices, DebugData, DebugDataDirty.
//
// These aliases are kept for backward compatibility.
// =============================================================================

using MeshGeometryDebugData = caustica::MeshGeometryDebugData;
using MeshDebugData         = caustica::MeshDebugData;
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
        nvrhi::rt::OpacityMicromapBuildFlags Flag = nvrhi::rt::OpacityMicromapBuildFlags::FastTrace;
        nvrhi::rt::OpacityMicromapFormat Format = nvrhi::rt::OpacityMicromapFormat::OC1_4_State;
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
    OpacityMicromapBuilder(nvrhi::DeviceHandle device,
        std::shared_ptr<caustica::DescriptorTableManager> descriptorTableManager,
        std::shared_ptr<caustica::TextureLoader> textureCache,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory);
    ~OpacityMicromapBuilder();

    void                            createRenderPasses(nvrhi::BindingLayoutHandle bindlessLayout, caustica::render::RenderDevice& renderDevice);

    bool                            update(nvrhi::ICommandList& commandList,
                                           const caustica::ResourceTracker<caustica::MeshInfo>& meshes,
                                           size_t geometryCount);

    OpacityMicroMapUIData &         uiData()    { return m_uiData; }
    bool                            debugGUI(float indent, const caustica::ResourceTracker<caustica::MeshInfo>& meshes);

    void                            sceneLoaded(size_t geometryCount);
    void                            sceneUnloading();

    void                            createOpacityMicromaps(const caustica::ResourceTracker<caustica::MeshInfo>& meshes,
                                                           size_t geometryCount);
    void                            destroyOpacityMicromaps(nvrhi::ICommandList& commandList,
                                                            const caustica::ResourceTracker<caustica::MeshInfo>& meshes);
    void                            buildOpacityMicromaps(nvrhi::ICommandList& commandList,
                                                          const caustica::ResourceTracker<caustica::MeshInfo>& meshes,
                                                          size_t geometryCount);
    void                            writeGeometryDebugBuffer(nvrhi::ICommandList& commandList);
    void                            updateDebugGeometry(const caustica::MeshInfo& mesh);

    [[nodiscard]] nvrhi::IBuffer*   getGeometryDebugBuffer() const { return m_geometryDebugBuffer; }
    [[nodiscard]] bool              shouldUseRayTracingOpacityMicromaps() const { return m_uiData.Enable; }

    void                            setGlobalShaderMacros(std::vector<caustica::ShaderMacro> & macros);


private:
    void                            ensureGeometryDebugCapacity(size_t geometryCount);

    nvrhi::DeviceHandle             m_device;
    std::shared_ptr<caustica::TextureLoader> m_textureCache;
    std::shared_ptr<caustica::FramebufferFactory> m_framebufferFactory;
    std::shared_ptr<caustica::DescriptorTableManager> m_descriptorTableManager;
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;

    nvrhi::BindingLayoutHandle      m_commonBindingLayout;
    nvrhi::BindingLayoutHandle      m_bindlessLayout;
    caustica::BindingCache     m_bindingCache;

    ComputePass                     m_examplePass;

    std::unique_ptr<class OmmBuildQueue>  m_ommBuildQueue;

    OpacityMicroMapUIData           m_uiData;

    std::vector<class GeometryDebugData> m_geometryDebugDataPtr;
    nvrhi::BufferHandle             m_geometryDebugBuffer;
};
