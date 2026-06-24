#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include <render/Core/BindingCache.h>
#include <rhi/nvrhi.h>
#include <math/math.h>
#include <scene/SceneTypes.h>

#include <render/Core/ComputePass.h>

#include <shaders/Misc/OmmGeometryDebugData.hlsli>

namespace caustica
{
    class Scene;
    class FramebufferFactory;
    class TextureCache;
    class TextureHandle;
    class ShaderFactory;
    class CommonRenderPasses;
    struct TextureData;
    struct LoadedTexture;
}

struct MeshGeometryDebugData
{
    uint32_t ommArrayDataOffset = 0xFFFFFFFF; // for use by applications
    uint32_t ommDescBufferOffset = 0xFFFFFFFF; // for use by applications
    uint32_t ommIndexBufferOffset = 0xFFFFFFFF; // for use by applications
    nvrhi::Format ommIndexBufferFormat = nvrhi::Format::R32_UINT; // for use by applications
    uint64_t ommStatsTotalKnown = 0;
    uint64_t ommStatsTotalUnknown = 0;
};

struct MeshDebugData
{
    std::shared_ptr<caustica::DescriptorHandle> ommArrayDataBufferDescriptor;
    std::shared_ptr<caustica::DescriptorHandle> ommDescBufferDescriptor;
    std::shared_ptr<caustica::DescriptorHandle> ommIndexBufferDescriptor;
    nvrhi::BufferHandle ommArrayDataBuffer; // for use by applications
    nvrhi::BufferHandle ommDescBuffer; // for use by applications
    nvrhi::BufferHandle ommIndexBuffer; // for use by applications
};

struct MeshGeometryEx : public caustica::MeshGeometry
{
    // (Debug) OMM buffers.
    MeshGeometryDebugData DebugData;

    virtual ~MeshGeometryEx() = default;
};

struct MeshInfoEx : public caustica::MeshInfo
{
    nvrhi::rt::AccelStructHandle AccelStructOMM; // for use by application
    std::vector<nvrhi::rt::OpacityMicromapHandle> OpacityMicroMaps; // for use by application
    std::vector<uint32_t> DeformationSourcePositionIndices; // per render vertex; preserves OBJ v-order for deformation APIs

    std::unique_ptr<MeshDebugData> DebugData;
    bool DebugDataDirty = true; // set this to true to make Scene update the debug data

    virtual ~MeshInfoEx() = default;
};

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

    bool                                Enable = true;
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

class OmmBaker
{
public:
    OmmBaker(nvrhi::DeviceHandle device,
        std::shared_ptr<caustica::DescriptorTableManager> descriptorTableManager,
        std::shared_ptr<caustica::TextureCache> textureCache,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory);
    ~OmmBaker();

    void                            CreateRenderPasses(nvrhi::BindingLayoutHandle bindlessLayout, std::shared_ptr<caustica::CommonRenderPasses> commonPasses);

    bool                            Update(nvrhi::ICommandList& commandList, const caustica::Scene& scene);

    OpacityMicroMapUIData &         UIData()    { return m_uiData; }
    bool                            DebugGUI(float indent, const caustica::Scene& scene);

    void                            SceneLoaded(const caustica::Scene& scene);
    void                            SceneUnloading();

    void                            CreateOpacityMicromaps(const caustica::Scene& scene);
    void                            DestroyOpacityMicromaps(nvrhi::ICommandList& commandList, const caustica::Scene& scene);
    void                            BuildOpacityMicromaps(nvrhi::ICommandList& commandList, const caustica::Scene& scene);
    void                            WriteGeometryDebugBuffer(nvrhi::ICommandList& commandList);
    void                            UpdateDebugGeometry(const caustica::MeshInfo& mesh);

    [[nodiscard]] nvrhi::IBuffer*   GetGeometryDebugBuffer() const { return m_geometryDebugBuffer; }

    void                            SetGlobalShaderMacros(std::vector<caustica::ShaderMacro> & macros);


private:
    nvrhi::DeviceHandle             m_device;
    std::shared_ptr<caustica::TextureCache> m_textureCache;
    std::shared_ptr<caustica::CommonRenderPasses> m_commonPasses;
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
