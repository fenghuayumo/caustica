#pragma once

#include <rhi/validation.h>
#include <unordered_set>

namespace caustica::rhi::validation
{
    class DeviceWrapper;

    struct Range
    {
        uint32_t min = ~0u;
        uint32_t max = 0;

        void add(uint32_t item);
        [[nodiscard]] bool empty() const;
        [[nodiscard]] bool overlapsWith(const Range& other) const;
    };

    enum class GraphicsResourceType : uint32_t
    {
        SRV,
        Sampler,
        UAV,
        CB
    };

    struct BindingLocation
    {
        GraphicsResourceType type = GraphicsResourceType::SRV;
        uint32_t registerSpace = 0;
        uint32_t slot = 0;
        uint32_t arrayElement = 0;

        bool operator==(BindingLocation const& other) const
        {
            return type == other.type
                && registerSpace == other.registerSpace
                && slot == other.slot
                && arrayElement == other.arrayElement;
        }

        bool operator!=(BindingLocation const& other) const
        {
            return !(*this == other);
        }
    };
} // namespace caustica::rhi::validation

namespace std
{
    template<> struct hash<caustica::rhi::validation::BindingLocation>
    {
        std::size_t operator()(caustica::rhi::validation::BindingLocation const& s) const noexcept
        {
            size_t hash = 0;
            caustica::rhi::hash_combine(hash, uint32_t(s.type));
            caustica::rhi::hash_combine(hash, s.registerSpace);
            caustica::rhi::hash_combine(hash, s.slot);
            caustica::rhi::hash_combine(hash, s.arrayElement);
            return hash;
        }
    };
} // namespace std

namespace caustica::rhi::validation
{
    typedef std::unordered_set<BindingLocation> BindingLocationSet;

    struct BindingSummary
    {
        BindingLocationSet locations;
        uint32_t numVolatileCBs = 0;
        Range rangeSRV;
        Range rangeSampler;
        Range rangeUAV;
        Range rangeCB;

        [[nodiscard]] bool any() const;
        [[nodiscard]] bool overlapsWith(const BindingSummary& other) const;
    };
    
    std::ostream& operator<<(std::ostream& os, const BindingLocationSet& set);

    enum class CommandListState
    {
        INITIAL,
        OPEN,
        CLOSED
    };

    Resource* unwrapResource(Resource* resource);

    class AccelStructWrapper : public RefCounter<rt::AccelStruct>
    {
    public:
        bool isTopLevel = false;
        bool allowCompaction = false;
        bool allowUpdate = false;
        bool wasBuilt = false;

        // BLAS only
        std::vector<rt::GeometryDesc> buildGeometries;

        // TLAS only
        size_t maxInstances = 0;
        size_t buildInstances = 0;

        AccelStructWrapper(AccelStruct* as) : m_AccelStruct(as) { }
        AccelStruct* getUnderlyingObject() const { return m_AccelStruct; }

        // Resource

        Object getNativeObject(ObjectType objectType) override { return m_AccelStruct->getNativeObject(objectType); }

        // AccelStruct

        const rt::AccelStructDesc& getDesc() const override { return m_AccelStruct->getDesc(); }
        bool isCompacted() const override { return m_AccelStruct->isCompacted(); }
        uint64_t getDeviceAddress() const override { return m_AccelStruct->getDeviceAddress(); };
        
    private:
        rt::AccelStructHandle m_AccelStruct;
    };
    
    class CommandListWrapper : public RefCounter<CommandList>
    {
    public:
        friend class DeviceWrapper;

        CommandListWrapper(DeviceWrapper* device, CommandList* commandList, bool isImmediate, CommandQueue queueType);

    protected:
        CommandListHandle m_CommandList;
        RefCountPtr<DeviceWrapper> m_Device;
        MessageCallback* m_MessageCallback;
        bool m_IsImmediate;
        CommandQueue m_type;

        CommandListState m_State = CommandListState::INITIAL;
        bool m_GraphicsStateSet = false;
        bool m_ComputeStateSet = false;
        bool m_MeshletStateSet = false;
        bool m_RayTracingStateSet = false;
        GraphicsState m_CurrentGraphicsState;
        ComputeState m_CurrentComputeState;
        MeshletState m_CurrentMeshletState;
        rt::State m_CurrentRayTracingState;

        size_t m_PipelinePushConstantSize = 0;
        bool m_PushConstantsSet = false;

        void error(const std::string& messageText) const;
        void warning(const std::string& messageText) const;

        bool requireOpenState() const;
        bool requireExecuteState();
        bool requireType(CommandQueue queueType, const char* operation) const;
        CommandList* getUnderlyingCommandList() const { return m_CommandList; }

        void evaluatePushConstantSize(const caustica::rhi::BindingLayoutVector& bindingLayouts);
        bool validatePushConstants(const char* pipelineType, const char* stateFunctionName) const;
        bool validateBindingSetsAgainstLayouts(const static_vector<BindingLayoutHandle, c_MaxBindingLayouts>& layouts, const static_vector<BindingSet*, c_MaxBindingLayouts>& sets) const;

        bool validateBuildTopLevelAccelStruct(AccelStructWrapper* wrapper, size_t numInstances, rt::AccelStructBuildFlags buildFlags) const;

    public:

        // Resource implementation

        Object getNativeObject(ObjectType objectType) override;

        // CommandList implementation

        void open() override;
        void close() override;
        void clearState() override;

        void clearTextureFloat(Texture* t, TextureSubresourceSet subresources, const Color& clearColor) override;
        void clearDepthStencilTexture(Texture* t, TextureSubresourceSet subresources, bool clearDepth, float depth, bool clearStencil, uint8_t stencil) override;
        void clearTextureUInt(Texture* t, TextureSubresourceSet subresources, uint32_t clearColor) override;

        void copyTexture(Texture* dest, const TextureSlice& destSlice, Texture* src, const TextureSlice& srcSlice) override;
        void copyTexture(StagingTexture* dest, const TextureSlice& destSlice, Texture* src, const TextureSlice& srcSlice) override;
        void copyTexture(Texture* dest, const TextureSlice& destSlice, StagingTexture* src, const TextureSlice& srcSlice) override;
        void writeTexture(Texture* dest, uint32_t arraySlice, uint32_t mipLevel, const void* data, size_t rowPitch, size_t depthPitch) override;
        void resolveTexture(Texture* dest, const TextureSubresourceSet& dstSubresources, Texture* src, const TextureSubresourceSet& srcSubresources) override;

        void writeBuffer(Buffer* b, const void* data, size_t dataSize, uint64_t destOffsetBytes) override;
        void clearBufferUInt(Buffer* b, uint32_t clearValue) override;
        void copyBuffer(Buffer* dest, uint64_t destOffsetBytes, Buffer* src, uint64_t srcOffsetBytes, uint64_t dataSizeBytes) override;

        void clearSamplerFeedbackTexture(SamplerFeedbackTexture* texture) override;
        void decodeSamplerFeedbackTexture(Buffer* buffer, SamplerFeedbackTexture* texture, caustica::rhi::Format format) override;
        void setSamplerFeedbackTextureState(SamplerFeedbackTexture* texture, ResourceStates stateBits) override;

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

        void beginTimerQuery(TimerQuery* query) override;
        void endTimerQuery(TimerQuery* query) override;

        void beginMarker(const char* name) override;
        void endMarker() override;

        void setEnableAutomaticBarriers(bool enable) override;
        void setResourceStatesForBindingSet(BindingSet* bindingSet) override;

        void setEnableUavBarriersForTexture(Texture* texture, bool enableBarriers) override;
        void setEnableUavBarriersForBuffer(Buffer* buffer, bool enableBarriers) override;

        void beginTrackingTextureState(Texture* texture, TextureSubresourceSet subresources, ResourceStates stateBits) override;
        void beginTrackingBufferState(Buffer* buffer, ResourceStates stateBits) override;

        void setTextureState(Texture* texture, TextureSubresourceSet subresources, ResourceStates stateBits) override;
        void setBufferState(Buffer* buffer, ResourceStates stateBits) override;
        void textureAliasingBarrier(Texture* before, Texture* after) override;
        void bufferAliasingBarrier(Buffer* before, Buffer* after) override;
        void setAccelStructState(rt::AccelStruct* as, ResourceStates stateBits) override;

        void setPermanentTextureState(Texture* texture, ResourceStates stateBits) override;
        void setPermanentBufferState(Buffer* buffer, ResourceStates stateBits) override;

        void commitBarriers() override;
        
        ResourceStates getTextureSubresourceState(Texture* texture, ArraySlice arraySlice, MipLevel mipLevel) override;
        ResourceStates getBufferState(Buffer* buffer) override;

        Device* getDevice() override;
        const CommandListParameters& getDesc() override;
    };

    class DeviceWrapper : public RefCounter<Device>
    {
    public:
        friend class CommandListWrapper;

        DeviceWrapper(Device* device);
        
    protected:
        DeviceHandle m_Device;
        MessageCallback* m_MessageCallback;
        std::atomic<unsigned int> m_NumOpenImmediateCommandLists = 0;

        void error(const std::string& messageText) const;
        void warning(const std::string& messageText) const;

        bool validateBindingSetItem(const BindingSetItem& binding, DescriptorTable *pOptDescriptorTable, std::stringstream& errorStream);
        bool validatePipelineBindingLayouts(const static_vector<BindingLayoutHandle, c_MaxBindingLayouts>& bindingLayouts, const std::vector<Shader*>& shaders) const;
        bool validateShaderType(ShaderType expected, const ShaderDesc& shaderDesc, const char* function) const;
        bool validateRenderState(const RenderState& renderState, FramebufferInfo const& fbinfo) const;

        bool validateClusterOperationParams(const rt::cluster::OperationParams& params) const;
    public:

        // Resource implementation

        Object getNativeObject(ObjectType objectType) override;

        // Device implementation

        HeapHandle createHeap(const HeapDesc& d) override;

        TextureHandle createTexture(const TextureDesc& d) override;
        MemoryRequirements getTextureMemoryRequirements(Texture* texture) override;
        bool bindTextureMemory(Texture* texture, Heap* heap, uint64_t offset) override;

        TextureHandle createHandleForNativeTexture(ObjectType objectType, Object texture, const TextureDesc& desc) override;

        StagingTextureHandle createStagingTexture(const TextureDesc& d, CpuAccessMode cpuAccess) override;
        void *mapStagingTexture(StagingTexture* tex, const TextureSlice& slice, CpuAccessMode cpuAccess, size_t *outRowPitch) override;
        void unmapStagingTexture(StagingTexture* tex) override;

        void getTextureTiling(Texture* texture, uint32_t* numTiles, PackedMipDesc* desc, TileShape* tileShape, uint32_t* subresourceTilingsNum, SubresourceTiling* subresourceTilings) override;
        void updateTextureTileMappings(Texture* texture, const TextureTilesMapping* tileMappings, uint32_t numTileMappings, CommandQueue executionQueue = CommandQueue::Graphics) override;

        SamplerFeedbackTextureHandle createSamplerFeedbackTexture(Texture* pairedTexture, const SamplerFeedbackTextureDesc& desc) override;
        SamplerFeedbackTextureHandle createSamplerFeedbackForNativeTexture(ObjectType objectType, Object texture, Texture* pairedTexture) override;

        BufferHandle createBuffer(const BufferDesc& d) override;
        void *mapBuffer(Buffer* b, CpuAccessMode mapFlags) override;
        void unmapBuffer(Buffer* b) override;
        MemoryRequirements getBufferMemoryRequirements(Buffer* buffer) override;
        bool bindBufferMemory(Buffer* buffer, Heap* heap, uint64_t offset) override;

        BufferHandle createHandleForNativeBuffer(ObjectType objectType, Object buffer, const BufferDesc& desc) override;

        ShaderHandle createShader(const ShaderDesc& d, const void* binary, size_t binarySize) override;
        ShaderHandle createShaderSpecialization(Shader* baseShader, const ShaderSpecialization* constants, uint32_t numConstants) override;
        ShaderLibraryHandle createShaderLibrary(const void* binary, size_t binarySize) override;

        SamplerHandle createSampler(const SamplerDesc& d) override;

        InputLayoutHandle createInputLayout(const VertexAttributeDesc* d, uint32_t attributeCount, Shader* vertexShader) override;

        // event queries
        EventQueryHandle createEventQuery() override;
        void setEventQuery(EventQuery* query, CommandQueue queue) override;
        bool pollEventQuery(EventQuery* query) override;
        void waitEventQuery(EventQuery* query) override;
        void resetEventQuery(EventQuery* query) override;

        // timer queries
        TimerQueryHandle createTimerQuery() override;
        bool pollTimerQuery(TimerQuery* query) override;
        float getTimerQueryTime(TimerQuery* query) override;
        void resetTimerQuery(TimerQuery* query) override;

        GraphicsAPI getGraphicsAPI() override;

        FramebufferHandle createFramebuffer(const FramebufferDesc& desc) override;

        GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc, FramebufferInfo const& fbinfo) override;

        GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc, Framebuffer* fb) override;

        ComputePipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override;

        MeshletPipelineHandle createMeshletPipeline(const MeshletPipelineDesc& desc, FramebufferInfo const& fbinfo) override;

        MeshletPipelineHandle createMeshletPipeline(const MeshletPipelineDesc& desc, Framebuffer* fb) override;

        rt::PipelineHandle createRayTracingPipeline(const rt::PipelineDesc& desc) override;

        BindingLayoutHandle createBindingLayout(const BindingLayoutDesc& desc) override;
        BindingLayoutHandle createBindlessLayout(const BindlessLayoutDesc& desc) override;

        BindingSetHandle createBindingSet(const BindingSetDesc& desc, BindingLayout* layout) override;
        DescriptorTableHandle createDescriptorTable(BindingLayout* layout) override;

        void resizeDescriptorTable(DescriptorTable* descriptorTable, uint32_t newSize, bool keepContents = true) override;
        bool writeDescriptorTable(DescriptorTable* descriptorTable, const BindingSetItem& item) override;

        rt::OpacityMicromapHandle createOpacityMicromap(const rt::OpacityMicromapDesc& desc)  override;
        rt::AccelStructHandle createAccelStruct(const rt::AccelStructDesc& desc) override;
        MemoryRequirements getAccelStructMemoryRequirements(rt::AccelStruct* as) override;
        rt::cluster::OperationSizeInfo getClusterOperationSizeInfo(const rt::cluster::OperationParams& params) override;
        bool bindAccelStructMemory(rt::AccelStruct* as, Heap* heap, uint64_t offset) override;

        CommandListHandle createCommandList(const CommandListParameters& params = CommandListParameters()) override;
        uint64_t executeCommandLists(CommandList* const* pCommandLists, size_t numCommandLists, CommandQueue executionQueue = CommandQueue::Graphics) override;
        void queueWaitForCommandList(CommandQueue waitQueue, CommandQueue executionQueue, uint64_t instance) override;
        bool waitForIdle() override;
        void runGarbageCollection() override;
        bool queryFeatureSupport(Feature feature, void* pInfo = nullptr, size_t infoSize = 0) override;
        FormatSupport queryFormatSupport(Format format) override;
        coopvec::DeviceFeatures queryCoopVecFeatures() override;
        size_t getCoopVecMatrixSize(coopvec::DataType type, coopvec::MatrixLayout layout, int rows, int columns) override;
        Object getNativeQueue(ObjectType objectType, CommandQueue queue) override;
        MessageCallback* getMessageCallback() override;
        bool isAftermathEnabled() override;
        AftermathCrashDumpHelper& getAftermathCrashDumpHelper() override;
    };

} // namespace caustica::rhi::validation
