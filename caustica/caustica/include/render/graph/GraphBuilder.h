#pragma once

#include <render/graph/GpuTypes.h>
#include <rhi/rhi.h>

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
    const char* executeAfter = nullptr;
};

class PassBuilder
{
public:
    explicit PassBuilder(GraphBuilder& graph);

    void read(TextureHandle texture, TextureAccess access = TextureAccess::ShaderResource);
    void write(TextureHandle texture, TextureAccess access = TextureAccess::RenderTarget);
    void read(BufferHandle buffer, BufferAccess access = BufferAccess::ShaderResource);
    void write(BufferHandle buffer, BufferAccess access = BufferAccess::UnorderedAccess);

    [[nodiscard]] TextureHandle createTexture(const TextureDesc& desc);
    [[nodiscard]] BufferHandle createBuffer(const BufferDesc& desc);

    [[nodiscard]] const std::vector<std::pair<TextureHandle, TextureAccess>>& textureReads() const { return m_textureReads; }
    [[nodiscard]] const std::vector<std::pair<TextureHandle, TextureAccess>>& textureWrites() const { return m_textureWrites; }
    [[nodiscard]] const std::vector<std::pair<BufferHandle, BufferAccess>>& bufferReads() const { return m_bufferReads; }
    [[nodiscard]] const std::vector<std::pair<BufferHandle, BufferAccess>>& bufferWrites() const { return m_bufferWrites; }

private:
    GraphBuilder* m_graph = nullptr;
    std::vector<std::pair<TextureHandle, TextureAccess>> m_textureReads;
    std::vector<std::pair<TextureHandle, TextureAccess>> m_textureWrites;
    std::vector<std::pair<BufferHandle, BufferAccess>> m_bufferReads;
    std::vector<std::pair<BufferHandle, BufferAccess>> m_bufferWrites;
};

class RenderPassContext
{
public:
    RenderPassContext(caustica::rhi::ICommandList* commandList, const GraphBuilder& graph);

    [[nodiscard]] caustica::rhi::ICommandList* commandList() const { return m_commandList; }
    [[nodiscard]] caustica::rhi::ITexture* texture(TextureHandle handle) const;
    [[nodiscard]] caustica::rhi::IBuffer* buffer(BufferHandle handle) const;

private:
    caustica::rhi::ICommandList* m_commandList = nullptr;
    const GraphBuilder* m_graph = nullptr;
};

class GraphBuilder
{
public:
    using SetupFn = std::function<void(PassBuilder&)>;
    using ExecuteFn = std::function<void(RenderPassContext&)>;

    void setDevice(caustica::rhi::IDevice* device);
    void setRenderTargetPool(RenderTargetPool* pool) { m_renderTargetPool = pool; }
    [[nodiscard]] RenderTargetPool* renderTargetPool() const { return m_renderTargetPool; }
    void setRenderBufferPool(RenderBufferPool* pool) { m_renderBufferPool = pool; }
    [[nodiscard]] RenderBufferPool* renderBufferPool() const { return m_renderBufferPool; }

    TextureHandle importTexture(caustica::rhi::ITexture* texture, caustica::rhi::ResourceStates initialState);
    TextureHandle importTexture(caustica::rhi::ITexture* texture, TextureAccess initialAccess = TextureAccess::ShaderResource);
    BufferHandle importBuffer(caustica::rhi::IBuffer* buffer, caustica::rhi::ResourceStates initialState);
    BufferHandle importBuffer(caustica::rhi::IBuffer* buffer, BufferAccess initialAccess = BufferAccess::ShaderResource);

    [[nodiscard]] TextureHandle createTexture(const TextureDesc& desc);
    [[nodiscard]] BufferHandle createBuffer(const BufferDesc& desc);

    void extractTexture(TextureHandle handle, caustica::rhi::ResourceStates finalState);
    void extractTexture(TextureHandle handle, TextureAccess finalAccess);
    void extractBuffer(BufferHandle handle, caustica::rhi::ResourceStates finalState);
    void extractBuffer(BufferHandle handle, BufferAccess finalAccess);

    void addPass(std::string_view name, SetupFn setup, ExecuteFn execute, PassOptions options = {});

    void compile();
    void execute(caustica::rhi::ICommandList* commandList);

    void reset();

    [[nodiscard]] caustica::rhi::ITexture* resolveTexture(TextureHandle handle) const;
    [[nodiscard]] caustica::rhi::IBuffer* resolveBuffer(BufferHandle handle) const;
    [[nodiscard]] caustica::rhi::ResourceStates textureState(TextureHandle handle) const;
    [[nodiscard]] caustica::rhi::ResourceStates bufferState(BufferHandle handle) const;
    [[nodiscard]] bool isCompiled() const { return m_compiled; }

    [[nodiscard]] size_t passCount() const { return m_passes.size(); }
    [[nodiscard]] const std::vector<std::string>& passNames() const { return m_passNames; }
    [[nodiscard]] const std::vector<uint32_t>& compiledPassOrder() const { return m_compiledPassOrder; }
    [[nodiscard]] const TransientResourceStats& transientResourceStats() const { return m_transientStats; }
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
        caustica::rhi::ITexture* texture = nullptr;
        caustica::rhi::ResourceStates currentState = caustica::rhi::ResourceStates::Common;
        std::optional<caustica::rhi::ResourceStates> finalState;
        ResourceLifetime lifetime = ResourceLifetime::Imported;
        TextureDesc desc;
        caustica::rhi::TextureHandle owned;
    };

    struct GraphBuffer
    {
        caustica::rhi::IBuffer* buffer = nullptr;
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
    void releaseTransientResources();
    void transitionTexture(caustica::rhi::ICommandList* commandList, TextureHandle handle, TextureAccess access);
    void transitionTexture(caustica::rhi::ICommandList* commandList, TextureHandle handle, caustica::rhi::ResourceStates targetState);
    void transitionBuffer(caustica::rhi::ICommandList* commandList, BufferHandle handle, BufferAccess access);
    void transitionBuffer(caustica::rhi::ICommandList* commandList, BufferHandle handle, caustica::rhi::ResourceStates targetState);
    void emitTextureAliasingBarrier(caustica::rhi::ICommandList* commandList, TextureHandle handle);
    void emitBufferAliasingBarrier(caustica::rhi::ICommandList* commandList, BufferHandle handle);
    void syncPassEndStates(const Pass& pass);
    static bool passUsesTextureAsWrite(const Pass& pass, TextureHandle handle);
    static bool passUsesBufferAsWrite(const Pass& pass, BufferHandle handle);
    void transitionExtractedResources(caustica::rhi::ICommandList* commandList);

    caustica::rhi::IDevice* m_device = nullptr;
    RenderTargetPool* m_renderTargetPool = nullptr;
    RenderBufferPool* m_renderBufferPool = nullptr;
    bool m_compiled = false;
    std::vector<GraphTexture> m_textures;
    std::vector<GraphBuffer> m_buffers;
    std::vector<Pass> m_passes;
    std::vector<uint32_t> m_compiledPassOrder;
    std::vector<std::string> m_passNames;
    std::unordered_map<caustica::rhi::ITexture*, uint32_t> m_importIndexByTexture;
    std::unordered_map<caustica::rhi::IBuffer*, uint32_t> m_importIndexByBuffer;
    std::vector<caustica::rhi::HeapHandle> m_transientHeaps;
    std::vector<caustica::rhi::HeapHandle> m_transientHeapPool;
    std::vector<TextureAliasingBarrier> m_textureAliasingBarriers;
    std::vector<BufferAliasingBarrier> m_bufferAliasingBarriers;
    TransientResourceStats m_transientStats;
};

} // namespace caustica::rg
