#pragma once

#include <rhi/d3d11.h>
#include <rhi/common/resourcebindingmap.h>
#include <rhi/utils.h>
#include "../common/dxgi-format.h"

#include <d3d11_1.h>
#include <map>
#include <vector>

#ifndef CAUSTICA_RHI_D3D11_WITH_NVAPI
#define CAUSTICA_RHI_D3D11_WITH_NVAPI 0
#endif

#if CAUSTICA_RHI_D3D11_WITH_NVAPI
#include <nvapi.h>
#include <nvShaderExtnEnums.h>
#endif

#include <rhi/common/aftermath.h>
#if CAUSTICA_RHI_WITH_AFTERMATH
#include <GFSDK_Aftermath.h>
#endif

namespace caustica::rhi::d3d11
{
    void SetDebugName(ID3D11DeviceChild* pObject, const char* name);

    D3D11_BLEND convertBlendValue(BlendFactor value);
    D3D11_BLEND_OP convertBlendOp(BlendOp value);
    D3D11_STENCIL_OP convertStencilOp(StencilOp value);
    D3D11_COMPARISON_FUNC convertComparisonFunc(ComparisonFunc value);
    D3D_PRIMITIVE_TOPOLOGY convertPrimType(PrimitiveType pt, uint32_t controlPoints);
    D3D11_TEXTURE_ADDRESS_MODE convertSamplerAddressMode(SamplerAddressMode mode);
    UINT convertSamplerReductionType(SamplerReductionType reductionType);

    struct Context
    {
        RefCountPtr<ID3D11Device> device;
        RefCountPtr<ID3D11DeviceContext> immediateContext;
        RefCountPtr<ID3D11DeviceContext1> immediateContext1;
        RefCountPtr<ID3D11Buffer> pushConstantBuffer;
        MessageCallback* messageCallback = nullptr;
        bool nvapiAvailable = false;
#if CAUSTICA_RHI_WITH_AFTERMATH
        GFSDK_Aftermath_ContextHandle aftermathContext = nullptr;
#endif

        void error(const std::string& message) const;
    };

    class Texture : public RefCounter<rhi::Texture>
    {
    public:
        TextureDesc desc;
        RefCountPtr<ID3D11Resource> resource;
        HANDLE sharedHandle = nullptr;

        Texture(const Context& context) : m_Context(context) { }
        const TextureDesc& getDesc() const override { return desc; }
        Object getNativeObject(ObjectType objectType) override;
        Object getNativeView(ObjectType objectType, Format format, TextureSubresourceSet subresources, TextureDimension dimension, bool isReadOnlyDSV = false) override;

        ID3D11ShaderResourceView* getSRV(Format format, TextureSubresourceSet subresources, TextureDimension dimension);
        ID3D11RenderTargetView* getRTV(Format format, TextureSubresourceSet subresources);
        ID3D11DepthStencilView* getDSV(TextureSubresourceSet subresources, bool isReadOnly = false);
        ID3D11UnorderedAccessView* getUAV(Format format, TextureSubresourceSet subresources, TextureDimension dimension);

    private:
        const Context& m_Context;
        TextureBindingKey_HashMap<RefCountPtr<ID3D11ShaderResourceView>> m_ShaderResourceViews;
        TextureBindingKey_HashMap<RefCountPtr<ID3D11RenderTargetView>> m_RenderTargetViews;
        TextureBindingKey_HashMap<RefCountPtr<ID3D11DepthStencilView>> m_DepthStencilViews;
        TextureBindingKey_HashMap<RefCountPtr<ID3D11UnorderedAccessView>> m_UnorderedAccessViews;
    };

    class StagingTexture : public RefCounter<rhi::StagingTexture>
    {
    public:
        RefCountPtr<Texture> texture;
        CpuAccessMode cpuAccess = CpuAccessMode::None;
        UINT mappedSubresource = UINT(-1);
        
        const TextureDesc& getDesc() const override { return texture->getDesc(); }
    };

    class Buffer : public RefCounter<rhi::Buffer>
    {
    public:
        BufferDesc desc;
        RefCountPtr<ID3D11Buffer> resource;
        HANDLE sharedHandle = nullptr;
        
        Buffer(const Context& context) : m_Context(context) { }
        const BufferDesc& getDesc() const override { return desc; }
        GpuVirtualAddress getGpuVirtualAddress() const override { caustica::rhi::utils::NotImplemented(); return 0; }
        Object getNativeObject(ObjectType objectType) override;

        ID3D11ShaderResourceView* getSRV(Format format, BufferRange range, ResourceType type);
        ID3D11UnorderedAccessView* getUAV(Format format, BufferRange range, ResourceType type);
        
    private:
        const Context& m_Context;
        std::unordered_map<BufferBindingKey, RefCountPtr<ID3D11ShaderResourceView>> m_ShaderResourceViews;
        std::unordered_map<BufferBindingKey, RefCountPtr<ID3D11UnorderedAccessView>> m_UnorderedAccessViews;
    };

    class Shader : public RefCounter<rhi::Shader>
    {
    public:
        ShaderDesc desc;
        RefCountPtr<ID3D11VertexShader> VS;
        RefCountPtr<ID3D11HullShader> HS;
        RefCountPtr<ID3D11DomainShader> DS;
        RefCountPtr<ID3D11GeometryShader> GS;
        RefCountPtr<ID3D11PixelShader> PS;
        RefCountPtr<ID3D11ComputeShader> CS;
        std::vector<char> bytecode;
        
        const ShaderDesc& getDesc() const override { return desc; }

        void getBytecode(const void** ppBytecode, size_t* pSize) const override;
    };

    class Sampler : public RefCounter<rhi::Sampler>
    {
    public:
        SamplerDesc desc;
        RefCountPtr<ID3D11SamplerState> sampler;
        
        const SamplerDesc& getDesc() const override { return desc; }
    };

    class EventQuery : public RefCounter<rhi::EventQuery>
    {
    public:
        RefCountPtr<ID3D11Query> query;
        bool resolved = false;
    };

    class TimerQuery : public RefCounter<rhi::TimerQuery>
    {
    public:
        RefCountPtr<ID3D11Query> start;
        RefCountPtr<ID3D11Query> end;
        RefCountPtr<ID3D11Query> disjoint;

        bool resolved = false;
        float time = 0.f;
    };
    
    class InputLayout : public RefCounter<rhi::InputLayout>
    {
    public:
        RefCountPtr<ID3D11InputLayout> layout;
        std::vector<VertexAttributeDesc> attributes;
        // maps a binding slot number to a stride
        std::unordered_map<uint32_t, uint32_t> elementStrides;

        uint32_t getNumAttributes() const override { return uint32_t(attributes.size()); }
        const VertexAttributeDesc* getAttributeDesc(uint32_t index) const override;
    };


    class Framebuffer : public RefCounter<rhi::Framebuffer>
    {
    public:
        FramebufferDesc desc;
        FramebufferInfoEx framebufferInfo;
        static_vector<RefCountPtr<ID3D11RenderTargetView>, c_MaxRenderTargets> RTVs;
        RefCountPtr<ID3D11DepthStencilView> DSV;
        
        const FramebufferDesc& getDesc() const override { return desc; }
        const FramebufferInfoEx& getFramebufferInfo() const override { return framebufferInfo; }
    };

    struct DX11_ViewportState
    {
        uint32_t numViewports = 0;
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_MAX_INDEX] = {};
        uint32_t numScissorRects = 0;
        D3D11_RECT scissorRects[D3D11_VIEWPORT_AND_SCISSORRECT_MAX_INDEX] = {};
    };

    class GraphicsPipeline : public RefCounter<rhi::GraphicsPipeline>
    {
    public:
        GraphicsPipelineDesc desc;
        ShaderType shaderMask = ShaderType::None;
        FramebufferInfo framebufferInfo;

        D3D11_PRIMITIVE_TOPOLOGY primitiveTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        InputLayout *inputLayout = nullptr;

        ID3D11RasterizerState *pRS = nullptr;

        ID3D11BlendState *pBlendState = nullptr;
        ID3D11DepthStencilState *pDepthStencilState = nullptr;
        bool requiresBlendFactor = false;
        bool pixelShaderHasUAVs = false;

        RefCountPtr<ID3D11VertexShader> pVS;
        RefCountPtr<ID3D11HullShader> pHS;
        RefCountPtr<ID3D11DomainShader> pDS;
        RefCountPtr<ID3D11GeometryShader> pGS;
        RefCountPtr<ID3D11PixelShader> pPS;
        
        const GraphicsPipelineDesc& getDesc() const override { return desc; }
        const FramebufferInfo& getFramebufferInfo() const override { return framebufferInfo; }
    };

    class ComputePipeline : public RefCounter<rhi::ComputePipeline>
    {
    public:
        ComputePipelineDesc desc;

        RefCountPtr<ID3D11ComputeShader> shader;
        
        const ComputePipelineDesc& getDesc() const override { return desc; }
    };

    class BindingLayout : public RefCounter<rhi::BindingLayout>
    {
    public:
        BindingLayoutDesc desc;

        const BindingLayoutDesc* getDesc() const override { return &desc; }
        const BindlessLayoutDesc* getBindlessDesc() const override { return nullptr; }
    };

    class BindingSet : public RefCounter<rhi::BindingSet>
    {
    public:
        BindingSetDesc desc;
        BindingLayoutHandle layout;
        ShaderType visibility = ShaderType::None;

        ID3D11ShaderResourceView* SRVs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
        uint32_t minSRVSlot = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
        uint32_t maxSRVSlot = 0;

        ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
        uint32_t minSamplerSlot = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
        uint32_t maxSamplerSlot = 0;

        ID3D11Buffer* constantBuffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
        UINT constantBufferOffsets[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
        UINT constantBufferCounts[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
        uint32_t minConstantBufferSlot = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
        uint32_t maxConstantBufferSlot = 0;

        ID3D11UnorderedAccessView* UAVs[D3D11_1_UAV_SLOT_COUNT] = {};
        uint32_t minUAVSlot = D3D11_1_UAV_SLOT_COUNT;
        uint32_t maxUAVSlot = 0;

        std::vector<RefCountPtr<Resource>> resources;
        
        const BindingSetDesc* getDesc() const override { return &desc; }
        BindingLayout* getLayout() const override { return layout; }
        bool isSupersetOf(const BindingSet& other) const;
    };

    class CommandList : public RefCounter<rhi::CommandList>
    {
    public:
        explicit CommandList(const Context& context, Device* device, const CommandListParameters& params);
        ~CommandList() override;

        // Resource implementation

        Object getNativeObject(ObjectType objectType) override;

        // CommandList implementation

        void open() override;
        void close() override;
        void clearState() override;

        void clearTextureFloat(rhi::Texture* t, TextureSubresourceSet subresources, const Color& clearColor) override;
        void clearDepthStencilTexture(rhi::Texture* t, TextureSubresourceSet subresources, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) override;
        void clearTextureUInt(rhi::Texture* t, TextureSubresourceSet subresources, uint32_t clearColor) override;
        void clearSamplerFeedbackTexture(rhi::SamplerFeedbackTexture* texture) override;
        void decodeSamplerFeedbackTexture(rhi::Buffer* buffer, rhi::SamplerFeedbackTexture* texture, Format format) override;
        void setSamplerFeedbackTextureState(rhi::SamplerFeedbackTexture* texture, ResourceStates stateBits) override;

        void copyTexture(rhi::Texture* dest, const TextureSlice& destSlice, rhi::Texture* src, const TextureSlice& srcSlice) override;
        void copyTexture(rhi::StagingTexture* dest, const TextureSlice& destSlice, rhi::Texture* src, const TextureSlice& srcSlice) override;
        void copyTexture(rhi::Texture* dest, const TextureSlice& destSlice, rhi::StagingTexture* src, const TextureSlice& srcSlice) override;
        void writeTexture(rhi::Texture* dest, uint32_t arraySlice, uint32_t mipLevel, const void* data, size_t rowPitch, size_t depthPitch) override;
        void resolveTexture(rhi::Texture* dest, const TextureSubresourceSet& dstSubresources, rhi::Texture* src, const TextureSubresourceSet& srcSubresources) override;

        void writeBuffer(rhi::Buffer* b, const void* data, size_t dataSize, uint64_t destOffsetBytes = 0) override;
        void clearBufferUInt(rhi::Buffer* b, uint32_t clearValue) override;
        void copyBuffer(rhi::Buffer* dest, uint64_t destOffsetBytes, rhi::Buffer* src, uint64_t srcOffsetBytes, uint64_t dataSizeBytes) override;

        void setPushConstants(const void* data, size_t byteSize) override;

        void setGraphicsState(const GraphicsState& state) override;
        void draw(const DrawArguments& args) override;
        void drawIndexed(const DrawArguments& args) override;
        void drawIndirect(uint32_t offsetBytes, uint32_t drawCount) override;
        void drawIndexedIndirect(uint32_t offsetBytes, uint32_t drawCount) override;
        void drawIndexedIndirectCount(uint32_t paramOffsetBytes, uint32_t countOffsetBytes, uint32_t maxDrawCount) override;

        void setComputeState(const ComputeState& state) override;
        void dispatch(uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1) override;
        void dispatchIndirect(uint32_t offsetBytes)  override;

        void setMeshletState(const MeshletState& state) override;
        void dispatchMesh(uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1) override;

        void setRayTracingState(const rt::State& state) override;
        void dispatchRays(const rt::DispatchRaysArguments& args) override;

        void buildOpacityMicromap(rt::OpacityMicromap* omm, const rt::OpacityMicromapDesc& desc) override;
        void buildBottomLevelAccelStruct(rt::AccelStruct* as, const rt::GeometryDesc* pGeometries, size_t numGeometries, rt::AccelStructBuildFlags buildFlags) override;
        void compactBottomLevelAccelStructs() override;
        void buildTopLevelAccelStruct(rt::AccelStruct* as, const rt::InstanceDesc* pInstances, size_t numInstances, rt::AccelStructBuildFlags buildFlags) override;
        void buildTopLevelAccelStructFromBuffer(rt::AccelStruct* as, caustica::rhi::Buffer* instanceBuffer, uint64_t instanceBufferOffset, size_t numInstances,
            rt::AccelStructBuildFlags buildFlags = rt::AccelStructBuildFlags::None) override;
        void executeMultiIndirectClusterOperation(const rt::cluster::OperationDesc& desc) override;

        void convertCoopVecMatrices(coopvec::ConvertMatrixLayoutDesc const* convertDescs, size_t numDescs) override;

        void beginTimerQuery(rhi::TimerQuery* query) override;
        void endTimerQuery(rhi::TimerQuery* query) override;

        // perf markers
        void beginMarker(const char* name) override;
        void endMarker() override;

        void setEnableAutomaticBarriers(bool enable) override { (void)enable; }
        void setResourceStatesForBindingSet(rhi::BindingSet* bindingSet) override { (void)bindingSet; }

        void setEnableUavBarriersForTexture(rhi::Texture* texture, bool enableBarriers) override;
        void setEnableUavBarriersForBuffer(rhi::Buffer* buffer, bool enableBarriers) override;

        void beginTrackingTextureState(rhi::Texture* texture, TextureSubresourceSet subresources, ResourceStates stateBits) override { (void)texture; (void)subresources; (void)stateBits; }
        void beginTrackingBufferState(rhi::Buffer* buffer, ResourceStates stateBits) override { (void)buffer; (void)stateBits; }

        void setTextureState(rhi::Texture* texture, TextureSubresourceSet subresources, ResourceStates stateBits) override { (void)texture; (void)subresources; (void)stateBits; }
        void setBufferState(rhi::Buffer* buffer, ResourceStates stateBits) override { (void)buffer; (void)stateBits; }
        void textureAliasingBarrier(rhi::Texture* before, rhi::Texture* after) override { (void)before; (void)after; }
        void bufferAliasingBarrier(rhi::Buffer* before, rhi::Buffer* after) override { (void)before; (void)after; }
        void setAccelStructState(rt::AccelStruct* as, ResourceStates stateBits) override { (void)as; (void)stateBits; }

        void setPermanentTextureState(rhi::Texture* texture, ResourceStates stateBits) override { (void)texture; (void)stateBits; }
        void setPermanentBufferState(rhi::Buffer* buffer, ResourceStates stateBits) override { (void)buffer; (void)stateBits; }

        void commitBarriers() override { }

        ResourceStates getTextureSubresourceState(rhi::Texture* texture, ArraySlice arraySlice, MipLevel mipLevel) override { (void)texture; (void)arraySlice; (void)mipLevel; return ResourceStates::Common; }
        ResourceStates getBufferState(rhi::Buffer* buffer) override { (void)buffer; return ResourceStates::Common; }

        Device* getDevice() override { return m_Device; }
        const CommandListParameters& getDesc() override { return m_Desc; }

    private:
        const Context& m_Context;
        Device* m_Device; // weak reference - to avoid a cyclic reference between Device and its ImmediateCommandList
        CommandListParameters m_Desc;

        RefCountPtr<ID3DUserDefinedAnnotation> m_UserDefinedAnnotation;
#if CAUSTICA_RHI_WITH_AFTERMATH
        AftermathMarkerTracker m_AftermathTracker;
#endif

        int m_NumUAVOverlapCommands = 0;
        void enterUAVOverlapSection();
        void leaveUAVOverlapSection();

        // State cache.
        // Use strong references (handles) instead of just a copy of GraphicsState etc.
        // If user code creates some object, draws using it, and releases it, a weak pointer would become invalid.
        // Using strong references in all state objects would solve this problem, but it means there will be an extra AddRef/Release cost everywhere.

        GraphicsPipelineHandle m_CurrentGraphicsPipeline;
        FramebufferHandle m_CurrentFramebuffer;
        ViewportState m_CurrentViewports{};
        static_vector<BindingSetHandle, c_MaxBindingLayouts> m_CurrentBindings;
        static_vector<VertexBufferBinding, c_MaxVertexAttributes> m_CurrentVertexBufferBindings;
        IndexBufferBinding m_CurrentIndexBufferBinding{};
        static_vector<BufferHandle, c_MaxVertexAttributes> m_CurrentVertexBuffers;
        BufferHandle m_CurrentIndexBuffer;
        ComputePipelineHandle m_CurrentComputePipeline;
        SinglePassStereoState m_CurrentSinglePassStereoState{};
        BufferHandle m_CurrentIndirectBuffer;
        Color m_CurrentBlendConstantColor{};
        uint8_t m_CurrentStencilRefValue = 0;
        bool m_CurrentGraphicsStateValid = false;
        bool m_CurrentComputeStateValid = false;

        void copyTexture(ID3D11Resource* dst, const TextureDesc& dstDesc, const TextureSlice& dstSlice,
            ID3D11Resource* src, const TextureDesc& srcDesc, const TextureSlice& srcSlice);
        
        void bindGraphicsPipeline(const GraphicsPipeline* pso) const;

        void prepareToBindGraphicsResourceSets(
            const BindingSetVector& resourceSets,
            const static_vector<BindingSetHandle, c_MaxBindingLayouts>* currentResourceSets,
            const GraphicsPipeline* currentPipeline,
            const GraphicsPipeline* newPipeline,
            bool updateFramebuffer,
            BindingSetVector& outSetsToBind) const;
        void bindGraphicsResourceSets(const BindingSetVector& setsToBind, const GraphicsPipeline* newPipeline) const;
        void bindComputeResourceSets(const BindingSetVector& resourceSets, const static_vector<BindingSetHandle, c_MaxBindingLayouts>* currentResourceSets) const;
    };

    class Device : public RefCounter<rhi::Device>
    {
    public:
        explicit Device(const DeviceDesc& desc);
        ~Device() override;

        // Resource implementation

        Object getNativeObject(ObjectType objectType) override;

        // Device implementation

        HeapHandle createHeap(const HeapDesc& d) override;

        TextureHandle createTexture(const TextureDesc& d) override;
        MemoryRequirements getTextureMemoryRequirements(rhi::Texture* texture) override;
        bool bindTextureMemory(rhi::Texture* texture, rhi::Heap* heap, uint64_t offset) override;

        TextureHandle createHandleForNativeTexture(ObjectType objectType, Object texture, const TextureDesc& desc) override;

        StagingTextureHandle createStagingTexture(const TextureDesc& d, CpuAccessMode cpuAccess) override;
        void *mapStagingTexture(rhi::StagingTexture* tex, const TextureSlice& slice, CpuAccessMode cpuAccess, size_t *outRowPitch) override;
        void unmapStagingTexture(rhi::StagingTexture* tex) override;

        void getTextureTiling(rhi::Texture* texture, uint32_t* numTiles, PackedMipDesc* desc, TileShape* tileShape, uint32_t* subresourceTilingsNum, SubresourceTiling* subresourceTilings) override;
        void updateTextureTileMappings(rhi::Texture* texture, const TextureTilesMapping* tileMappings, uint32_t numTileMappings, CommandQueue executionQueue = CommandQueue::Graphics) override;

        SamplerFeedbackTextureHandle createSamplerFeedbackTexture(rhi::Texture* pairedTexture, const SamplerFeedbackTextureDesc& desc) override;
        SamplerFeedbackTextureHandle createSamplerFeedbackForNativeTexture(ObjectType objectType, Object texture, rhi::Texture* pairedTexture) override;

        BufferHandle createBuffer(const BufferDesc& d) override;
        void *mapBuffer(rhi::Buffer* b, CpuAccessMode mapFlags) override;
        void unmapBuffer(rhi::Buffer* b) override;
        MemoryRequirements getBufferMemoryRequirements(rhi::Buffer* buffer) override;
        bool bindBufferMemory(rhi::Buffer* buffer, rhi::Heap* heap, uint64_t offset) override;

        BufferHandle createHandleForNativeBuffer(ObjectType objectType, Object buffer, const BufferDesc& desc) override;

        ShaderHandle createShader(const ShaderDesc& d, const void* binary, const size_t binarySize) override;
        ShaderHandle createShaderSpecialization(rhi::Shader* baseShader, const ShaderSpecialization* constants, uint32_t numConstants) override;
        ShaderLibraryHandle createShaderLibrary(const void* binary, const size_t binarySize) override { (void)binary; (void)binarySize; return nullptr; }

        SamplerHandle createSampler(const SamplerDesc& d) override;

        InputLayoutHandle createInputLayout(const VertexAttributeDesc* d, uint32_t attributeCount, rhi::Shader* vertexShader) override;

        // event queries
        EventQueryHandle createEventQuery(void) override;
        void setEventQuery(rhi::EventQuery* query, CommandQueue queue) override;
        bool pollEventQuery(rhi::EventQuery* query) override;
        void waitEventQuery(rhi::EventQuery* query) override;
        void resetEventQuery(rhi::EventQuery* query) override;

        // timer queries
        TimerQueryHandle createTimerQuery(void) override;
        bool pollTimerQuery(rhi::TimerQuery* query) override;
        float getTimerQueryTime(rhi::TimerQuery* query) override;
        void resetTimerQuery(rhi::TimerQuery* query) override;

        GraphicsAPI getGraphicsAPI() override;

        FramebufferHandle createFramebuffer(const FramebufferDesc& desc) override;

        GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc, FramebufferInfo const& fbinfo) override;

        GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc, rhi::Framebuffer* fb) override;

        ComputePipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override;

        MeshletPipelineHandle createMeshletPipeline(const MeshletPipelineDesc& desc, FramebufferInfo const& fbinfo) override;

        MeshletPipelineHandle createMeshletPipeline(const MeshletPipelineDesc& desc, rhi::Framebuffer* fb) override;

        rt::PipelineHandle createRayTracingPipeline(const rt::PipelineDesc& desc) override;

        BindingLayoutHandle createBindingLayout(const BindingLayoutDesc& desc) override;
        BindingLayoutHandle createBindlessLayout(const BindlessLayoutDesc& desc) override;

        BindingSetHandle createBindingSet(const BindingSetDesc& desc, rhi::BindingLayout* layout) override;
        DescriptorTableHandle createDescriptorTable(rhi::BindingLayout* layout) override;

        void resizeDescriptorTable(rhi::DescriptorTable* descriptorTable, uint32_t newSize, bool keepContents = true) override;
        bool writeDescriptorTable(rhi::DescriptorTable* descriptorTable, const BindingSetItem& item) override;

        rt::OpacityMicromapHandle createOpacityMicromap(const rt::OpacityMicromapDesc& desc) override;
        rt::AccelStructHandle createAccelStruct(const rt::AccelStructDesc& desc) override;
        MemoryRequirements getAccelStructMemoryRequirements(rt::AccelStruct* as) override;
        rt::cluster::OperationSizeInfo getClusterOperationSizeInfo(const rt::cluster::OperationParams& params) override;
        bool bindAccelStructMemory(rt::AccelStruct* as, rhi::Heap* heap, uint64_t offset) override;

        CommandListHandle createCommandList(const CommandListParameters& params = CommandListParameters()) override;
        uint64_t executeCommandLists(rhi::CommandList* const* pCommandLists, size_t numCommandLists, CommandQueue executionQueue = CommandQueue::Graphics) override { (void)pCommandLists; (void)numCommandLists; (void)executionQueue; return 0; }
        void queueWaitForCommandList(CommandQueue waitQueue, CommandQueue executionQueue, uint64_t instance) override { (void)waitQueue; (void)executionQueue; (void)instance; }
        bool waitForIdle() override;
        void runGarbageCollection() override { }
        bool queryFeatureSupport(Feature feature, void* pInfo = nullptr, size_t infoSize = 0) override;
        FormatSupport queryFormatSupport(Format format) override;
        coopvec::DeviceFeatures queryCoopVecFeatures() override;
        size_t getCoopVecMatrixSize(coopvec::DataType type, coopvec::MatrixLayout layout, int rows, int columns) override;
        Object getNativeQueue(ObjectType objectType, CommandQueue queue) override { (void)objectType; (void)queue;  return nullptr; }
        MessageCallback* getMessageCallback() override { return m_Context.messageCallback; }
        bool isAftermathEnabled() override { return m_AftermathEnabled; }
        AftermathCrashDumpHelper& getAftermathCrashDumpHelper() override { return m_AftermathCrashDumpHelper; }

    private:
        Context m_Context;
        EventQueryHandle m_WaitForIdleQuery;
        CommandListHandle m_ImmediateCommandList;

        std::unordered_map<size_t, RefCountPtr<ID3D11BlendState>> m_BlendStates;
        std::unordered_map<size_t, RefCountPtr<ID3D11DepthStencilState>> m_DepthStencilStates;
        std::unordered_map<size_t, RefCountPtr<ID3D11RasterizerState>> m_RasterizerStates;

        bool m_SinglePassStereoSupported = false;
        bool m_HlslExtensionsSupported = false;
        bool m_FastGeometryShaderSupported = false;

        TextureHandle createTexture(const TextureDesc& d, CpuAccessMode cpuAccess) const;

        ID3D11RenderTargetView* getRTVForAttachment(const FramebufferAttachment& attachment);
        ID3D11DepthStencilView* getDSVForAttachment(const FramebufferAttachment& attachment);

        ID3D11BlendState* getBlendState(const BlendState& blendState);
        ID3D11DepthStencilState* getDepthStencilState(const DepthStencilState& depthStencilState);
        ID3D11RasterizerState* getRasterizerState(const RasterState& rasterState);

        bool m_AftermathEnabled = false;
        AftermathCrashDumpHelper m_AftermathCrashDumpHelper;
    };

} // namespace caustica::rhi::d3d11
