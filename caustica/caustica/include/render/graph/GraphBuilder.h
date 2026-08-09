#pragma once

#include <render/graph/GpuTypes.h>
#include <render/graph/VolatileConstantBinder.h>
#include <rhi/rhi.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace caustica::rg
{
class RenderTargetPool;
class RenderBufferPool;
class TransientResourceAllocator;
}

namespace caustica::rg
{

enum class TextureAccess : uint8_t
{
    ShaderResource,
    RenderTarget,
    DepthWrite,
    UnorderedAccess,
    CopySource,
    CopyDest,
};

enum class BufferAccess : uint8_t
{
    ShaderResource,
    UnorderedAccess,
    ConstantBuffer,
    CopySource,
    CopyDest,
    VertexBuffer,
    IndexBuffer,
    IndirectArgument,
    AccelStructBuildInput,
};

class GraphBuilder;

struct PassOptions
{
    bool enabled = true;
    bool sideEffect = false;
    // Force this pass onto the primary list (and alone in its wave). Use for
    // mid-pass close/execute sync-points (e.g. ToneMapping first-frame AE).
    bool serialOnPrimary = false;
    // Relative CPU recording cost. The compiler groups independent passes into
    // a bounded number of worker command lists instead of one list per pass.
    uint16_t recordingCost = 1;
    const char* executeAfter = nullptr;
};

struct ExecuteParams
{
    // Parallel waves flush the primary list between multi-pass waves, which clears
    // NVRHI volatile CB addresses. VolatileConstantBinder rewrites registered
    // shadows at the start of every recordPass so consumers stay valid on forks.
    bool parallelWaves = true;
    uint32_t minParallelRecordingCost = 4;
    uint32_t maxParallelRecordingJobs = 0; // 0 = TaskRuntime worker count
};

struct GpuPassTiming
{
    std::string name;
    double milliseconds = 0.0;
};

struct GpuTimingFrame
{
    uint32_t frameIndex = 0;
    std::vector<GpuPassTiming> passes;
};

class PassBuilder
{
public:
    using TextureAccessList = std::vector<std::pair<TextureHandle, TextureAccess>>;
    using BufferAccessList = std::vector<std::pair<BufferHandle, BufferAccess>>;

    PassBuilder(
        GraphBuilder& graph,
        TextureAccessList& textureReads,
        TextureAccessList& textureWrites,
        BufferAccessList& bufferReads,
        BufferAccessList& bufferWrites);

    void read(TextureHandle texture, TextureAccess access = TextureAccess::ShaderResource);
    void write(TextureHandle texture, TextureAccess access = TextureAccess::RenderTarget);
    void read(BufferHandle buffer, BufferAccess access = BufferAccess::ShaderResource);
    void write(BufferHandle buffer, BufferAccess access = BufferAccess::UnorderedAccess);

    [[nodiscard]] TextureHandle createTexture(const TextureDesc& desc);
    [[nodiscard]] BufferHandle createBuffer(const BufferDesc& desc);

private:
    GraphBuilder* m_graph = nullptr;
    TextureAccessList* m_textureReads = nullptr;
    TextureAccessList* m_textureWrites = nullptr;
    BufferAccessList* m_bufferReads = nullptr;
    BufferAccessList* m_bufferWrites = nullptr;
};

class RenderPassContext
{
public:
    RenderPassContext(caustica::rhi::CommandList* commandList, const GraphBuilder& graph);

    [[nodiscard]] caustica::rhi::CommandList* commandList() const { return m_commandList; }
    [[nodiscard]] caustica::rhi::Texture* texture(TextureHandle handle) const;
    [[nodiscard]] caustica::rhi::Buffer* buffer(BufferHandle handle) const;

private:
    caustica::rhi::CommandList* m_commandList = nullptr;
    const GraphBuilder* m_graph = nullptr;
};

class GraphBuilder
{
public:
    using SetupFn = std::function<void(PassBuilder&)>;
    using ExecuteFn = std::function<void(RenderPassContext&)>;

    void setDevice(caustica::rhi::Device* device);
    void setRenderTargetPool(RenderTargetPool* pool) { m_renderTargetPool = pool; }
    [[nodiscard]] RenderTargetPool* renderTargetPool() const { return m_renderTargetPool; }
    void setRenderBufferPool(RenderBufferPool* pool) { m_renderBufferPool = pool; }
    [[nodiscard]] RenderBufferPool* renderBufferPool() const { return m_renderBufferPool; }

    TextureHandle importTexture(caustica::rhi::Texture* texture, caustica::rhi::ResourceStates initialState);
    TextureHandle importTexture(caustica::rhi::Texture* texture, TextureAccess initialAccess = TextureAccess::ShaderResource);
    BufferHandle importBuffer(caustica::rhi::Buffer* buffer, caustica::rhi::ResourceStates initialState);
    BufferHandle importBuffer(caustica::rhi::Buffer* buffer, BufferAccess initialAccess = BufferAccess::ShaderResource);

    [[nodiscard]] TextureHandle createTexture(const TextureDesc& desc);
    [[nodiscard]] BufferHandle createBuffer(const BufferDesc& desc);

    void extractTexture(TextureHandle handle, caustica::rhi::ResourceStates finalState);
    void extractTexture(TextureHandle handle, TextureAccess finalAccess);
    void extractBuffer(BufferHandle handle, caustica::rhi::ResourceStates finalState);
    void extractBuffer(BufferHandle handle, BufferAccess finalAccess);

    void addPass(std::string_view name, SetupFn setup, ExecuteFn execute, PassOptions options = {});

    void compile();
    // Primary must already be open. Parallel waves fork deferred lists and submit
    // them before continuing; serial waves / serialOnPrimary record on primary.
    void execute(caustica::rhi::FrameCommandContext& frameCtx, ExecuteParams params = {});

    // Timestamp queries are allocated/reset on the render thread, then written
    // by whichever command list records each pass. Results are consumed later
    // without stalling the GPU.
    void beginGpuTimingFrame(uint32_t frameIndex);
    [[nodiscard]] std::optional<GpuTimingFrame> collectCompletedGpuTimings();

    // Frame-scoped volatile CB binder (ADR 0001 R2). Register CPU shadows before
    // execute(); recordPass applies them on each command list open session.
    [[nodiscard]] VolatileConstantBinder& volatileConstants() { return m_volatileConstants; }
    [[nodiscard]] const VolatileConstantBinder& volatileConstants() const { return m_volatileConstants; }

    void reset();

    [[nodiscard]] caustica::rhi::Texture* resolveTexture(TextureHandle handle) const;
    [[nodiscard]] caustica::rhi::Buffer* resolveBuffer(BufferHandle handle) const;
    [[nodiscard]] caustica::rhi::ResourceStates textureState(TextureHandle handle) const;
    [[nodiscard]] caustica::rhi::ResourceStates bufferState(BufferHandle handle) const;
    [[nodiscard]] bool isCompiled() const { return m_compiled; }

    [[nodiscard]] size_t passCount() const { return m_passes.size(); }
    [[nodiscard]] const std::vector<std::string>& passNames() const { return m_passNames; }
    [[nodiscard]] const std::vector<uint32_t>& compiledPassOrder() const { return m_compiledPassOrder; }
    [[nodiscard]] const std::vector<std::vector<uint32_t>>& compiledWaves() const { return m_compiledWaves; }
    [[nodiscard]] const TransientResourceStats& transientResourceStats() const { return m_transientStats; }
    [[nodiscard]] bool lastCompileCacheHit() const { return m_lastCompileCacheHit; }
    [[nodiscard]] uint32_t lastParallelBatchCount() const { return m_lastParallelBatchCount; }
    [[nodiscard]] size_t activePassCount() const;
    [[nodiscard]] bool isPassRegistered(std::string_view name) const;
    [[nodiscard]] bool isPassActive(std::string_view name) const;

private:
    friend class PassBuilder;
    friend class RenderPassContext;
    friend class TransientResourceAllocator;

    enum class ResourceLifetime : uint8_t
    {
        Imported,
        Transient,
    };

    struct GraphTexture
    {
        caustica::rhi::Texture* texture = nullptr;
        caustica::rhi::ResourceStates currentState = caustica::rhi::ResourceStates::Common;
        std::optional<caustica::rhi::ResourceStates> finalState;
        ResourceLifetime lifetime = ResourceLifetime::Imported;
        TextureDesc desc;
        caustica::rhi::TextureHandle owned;
    };

    struct GraphBuffer
    {
        caustica::rhi::Buffer* buffer = nullptr;
        caustica::rhi::ResourceStates currentState = caustica::rhi::ResourceStates::Common;
        std::optional<caustica::rhi::ResourceStates> finalState;
        ResourceLifetime lifetime = ResourceLifetime::Imported;
        BufferDesc desc;
        caustica::rhi::BufferHandle owned;
    };

    struct Pass
    {
        std::string name;
        SetupFn setup;
        ExecuteFn execute;
        PassOptions options;
        bool active = false;
        std::vector<std::pair<TextureHandle, TextureAccess>> textureReads;
        std::vector<std::pair<TextureHandle, TextureAccess>> textureWrites;
        std::vector<std::pair<BufferHandle, BufferAccess>> bufferReads;
        std::vector<std::pair<BufferHandle, BufferAccess>> bufferWrites;
        caustica::rhi::TimerQuery* gpuTimer = nullptr;
    };

    struct TextureAliasingBarrier
    {
        TextureHandle before;
        TextureHandle after;
        bool emitted = false;
    };

    struct BufferAliasingBarrier
    {
        BufferHandle before;
        BufferHandle after;
        bool emitted = false;
    };

    static caustica::rhi::ResourceStates accessToState(TextureAccess access);
    static caustica::rhi::ResourceStates accessToState(BufferAccess access);

    [[nodiscard]] caustica::rhi::TextureHandle createNativeTexture(const TextureDesc& desc, bool isVirtual = false) const;
    [[nodiscard]] caustica::rhi::BufferHandle createNativeBuffer(const BufferDesc& desc, bool isVirtual = false) const;
    struct TransientLifetime
    {
        int32_t firstPassOrder = INT32_MAX;
        int32_t lastPassOrder = -1;
    };

    void computeTransientLifetimes(
        std::vector<TransientLifetime>& textureLifetimes,
        std::vector<TransientLifetime>& bufferLifetimes) const;
    void allocateTransientResources(
        const std::vector<bool>& referencedTextures,
        const std::vector<bool>& referencedBuffers,
        const std::vector<TransientLifetime>& textureLifetimes,
        const std::vector<TransientLifetime>& bufferLifetimes);
    [[nodiscard]] bool restorePersistentTransientResources(uint64_t planKey);
    void capturePersistentTransientResources(uint64_t planKey);
    void releaseTransientResources();
    void transitionTexture(caustica::rhi::CommandList* commandList, TextureHandle handle, TextureAccess access);
    void transitionTexture(caustica::rhi::CommandList* commandList, TextureHandle handle, caustica::rhi::ResourceStates targetState);
    void transitionBuffer(caustica::rhi::CommandList* commandList, BufferHandle handle, BufferAccess access);
    void transitionBuffer(caustica::rhi::CommandList* commandList, BufferHandle handle, caustica::rhi::ResourceStates targetState);
    void emitTextureAliasingBarrier(caustica::rhi::CommandList* commandList, TextureHandle handle);
    void emitBufferAliasingBarrier(caustica::rhi::CommandList* commandList, BufferHandle handle);
    void syncPassEndStates(const Pass& pass);
    static bool passUsesTextureAsWrite(const Pass& pass, TextureHandle handle);
    static bool passUsesBufferAsWrite(const Pass& pass, BufferHandle handle);
    void transitionExtractedResources(caustica::rhi::CommandList* commandList);
    void recordPass(
        caustica::rhi::CommandList* commandList,
        const Pass& pass,
        std::vector<caustica::rhi::ResourceStates>* localTextureStates = nullptr,
        std::vector<caustica::rhi::ResourceStates>* localBufferStates = nullptr);
    void executeWaveSerial(caustica::rhi::CommandList* commandList, const std::vector<uint32_t>& wave);
    uint32_t executeWaveParallel(
        caustica::rhi::FrameCommandContext& frameCtx,
        const std::vector<uint32_t>& wave,
        ExecuteParams params);
    void buildCompiledWaves(
        const std::vector<bool>& needed,
        const std::vector<std::vector<uint32_t>>& incoming,
        const std::vector<std::vector<uint32_t>>& outgoing);
    [[nodiscard]] uint64_t compiledPlanKey() const;

    struct CompiledPlan
    {
        std::vector<bool> activePasses;
        std::vector<uint32_t> passOrder;
        std::vector<std::vector<uint32_t>> waves;
        std::vector<bool> referencedTextures;
        std::vector<bool> referencedBuffers;
        std::vector<TransientLifetime> textureLifetimes;
        std::vector<TransientLifetime> bufferLifetimes;
    };

    struct PersistentTransientResources
    {
        uint64_t planKey = 0;
        bool valid = false;
        std::vector<caustica::rhi::TextureHandle> textures;
        std::vector<caustica::rhi::BufferHandle> buffers;
        std::vector<caustica::rhi::HeapHandle> heaps;
        std::vector<TextureAliasingBarrier> textureBarriers;
        std::vector<BufferAliasingBarrier> bufferBarriers;
        TransientResourceStats stats;
    };

    struct GpuTimingEntry
    {
        std::string name;
        caustica::rhi::TimerQueryHandle query;
    };

    struct GpuTimingSlot
    {
        uint32_t frameIndex = 0;
        bool pending = false;
        std::vector<GpuTimingEntry> entries;
    };

    // Three frames may be in flight; one extra slot absorbs a delayed readback
    // without consuming the device's entire timer-query budget.
    static constexpr size_t kGpuTimingSlotCount = 4;

    caustica::rhi::Device* m_device = nullptr;
    RenderTargetPool* m_renderTargetPool = nullptr;
    RenderBufferPool* m_renderBufferPool = nullptr;
    bool m_compiled = false;
    std::vector<GraphTexture> m_textures;
    std::vector<GraphBuffer> m_buffers;
    std::vector<Pass> m_passes;
    // Keeps Pass-owned strings and access-list capacities alive across reset().
    // Graph topology is rebuilt each frame, but its allocator footprint is not.
    std::vector<Pass> m_recycledPasses;
    std::vector<uint32_t> m_compiledPassOrder;
    std::vector<std::vector<uint32_t>> m_compiledWaves;
    std::vector<std::string> m_passNames;
    std::unordered_map<caustica::rhi::Texture*, uint32_t> m_importIndexByTexture;
    std::unordered_map<caustica::rhi::Buffer*, uint32_t> m_importIndexByBuffer;
    std::vector<caustica::rhi::HeapHandle> m_transientHeaps;
    std::vector<caustica::rhi::HeapHandle> m_transientHeapPool;
    std::vector<TextureAliasingBarrier> m_textureAliasingBarriers;
    std::vector<BufferAliasingBarrier> m_bufferAliasingBarriers;
    PersistentTransientResources m_persistentTransients;
    TransientResourceStats m_transientStats;
    VolatileConstantBinder m_volatileConstants;
    std::unordered_map<uint64_t, CompiledPlan> m_compiledPlanCache;
    std::unordered_map<std::string, double> m_recordingCostHistory;
    std::array<GpuTimingSlot, kGpuTimingSlotCount> m_gpuTimingSlots{};
    int32_t m_activeGpuTimingSlot = -1;
    bool m_lastCompileCacheHit = false;
    uint32_t m_lastParallelBatchCount = 0;
};

} // namespace caustica::rg
