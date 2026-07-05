#pragma once

#include <render/graph/GpuTypes.h>
#include <rhi/nvrhi.h>

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

namespace nvrhi
{
class ICommandList;
class IDevice;
class ITexture;
class IBuffer;
} // namespace nvrhi

namespace caustica::rg
{

enum class TextureAccess : uint8_t
{
    ShaderResource,
    RenderTarget,
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
    RenderPassContext(nvrhi::ICommandList* commandList, const GraphBuilder& graph);

    [[nodiscard]] nvrhi::ICommandList* commandList() const { return m_commandList; }
    [[nodiscard]] nvrhi::ITexture* texture(TextureHandle handle) const;
    [[nodiscard]] nvrhi::IBuffer* buffer(BufferHandle handle) const;

private:
    nvrhi::ICommandList* m_commandList = nullptr;
    const GraphBuilder* m_graph = nullptr;
};

class GraphBuilder
{
public:
    using SetupFn = std::function<void(PassBuilder&)>;
    using ExecuteFn = std::function<void(RenderPassContext&)>;

    void setDevice(nvrhi::IDevice* device);
    void setRenderTargetPool(RenderTargetPool* pool) { m_renderTargetPool = pool; }
    [[nodiscard]] RenderTargetPool* renderTargetPool() const { return m_renderTargetPool; }
    void setRenderBufferPool(RenderBufferPool* pool) { m_renderBufferPool = pool; }
    [[nodiscard]] RenderBufferPool* renderBufferPool() const { return m_renderBufferPool; }

    TextureHandle importTexture(nvrhi::ITexture* texture, nvrhi::ResourceStates initialState);
    TextureHandle importTexture(nvrhi::ITexture* texture, TextureAccess initialAccess = TextureAccess::ShaderResource);
    BufferHandle importBuffer(nvrhi::IBuffer* buffer, nvrhi::ResourceStates initialState);
    BufferHandle importBuffer(nvrhi::IBuffer* buffer, BufferAccess initialAccess = BufferAccess::ShaderResource);

    [[nodiscard]] TextureHandle createTexture(const TextureDesc& desc);
    [[nodiscard]] BufferHandle createBuffer(const BufferDesc& desc);

    void extractTexture(TextureHandle handle, nvrhi::ResourceStates finalState);
    void extractTexture(TextureHandle handle, TextureAccess finalAccess);
    void extractBuffer(BufferHandle handle, nvrhi::ResourceStates finalState);
    void extractBuffer(BufferHandle handle, BufferAccess finalAccess);

    void addPass(std::string_view name, SetupFn setup, ExecuteFn execute, PassOptions options = {});

    void compile();
    void execute(nvrhi::ICommandList* commandList);

    void reset();

    [[nodiscard]] nvrhi::ITexture* resolveTexture(TextureHandle handle) const;
    [[nodiscard]] nvrhi::IBuffer* resolveBuffer(BufferHandle handle) const;
    [[nodiscard]] nvrhi::ResourceStates textureState(TextureHandle handle) const;
    [[nodiscard]] nvrhi::ResourceStates bufferState(BufferHandle handle) const;
    [[nodiscard]] bool isCompiled() const { return m_compiled; }

    [[nodiscard]] size_t passCount() const { return m_passes.size(); }
    [[nodiscard]] const std::vector<std::string>& passNames() const { return m_passNames; }
    [[nodiscard]] const std::vector<uint32_t>& compiledPassOrder() const { return m_compiledPassOrder; }
    [[nodiscard]] const TransientResourceStats& transientResourceStats() const { return m_transientStats; }
    [[nodiscard]] size_t activePassCount() const;

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
        nvrhi::ITexture* texture = nullptr;
        nvrhi::ResourceStates currentState = nvrhi::ResourceStates::Common;
        std::optional<nvrhi::ResourceStates> finalState;
        ResourceLifetime lifetime = ResourceLifetime::Imported;
        TextureDesc desc;
        nvrhi::TextureHandle owned;
    };

    struct GraphBuffer
    {
        nvrhi::IBuffer* buffer = nullptr;
        nvrhi::ResourceStates currentState = nvrhi::ResourceStates::Common;
        std::optional<nvrhi::ResourceStates> finalState;
        ResourceLifetime lifetime = ResourceLifetime::Imported;
        BufferDesc desc;
        nvrhi::BufferHandle owned;
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

    static nvrhi::ResourceStates accessToState(TextureAccess access);
    static nvrhi::ResourceStates accessToState(BufferAccess access);

    [[nodiscard]] nvrhi::TextureHandle createNativeTexture(const TextureDesc& desc, bool isVirtual = false) const;
    [[nodiscard]] nvrhi::BufferHandle createNativeBuffer(const BufferDesc& desc, bool isVirtual = false) const;
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
    void transitionTexture(nvrhi::ICommandList* commandList, TextureHandle handle, TextureAccess access);
    void transitionTexture(nvrhi::ICommandList* commandList, TextureHandle handle, nvrhi::ResourceStates targetState);
    void transitionBuffer(nvrhi::ICommandList* commandList, BufferHandle handle, BufferAccess access);
    void transitionBuffer(nvrhi::ICommandList* commandList, BufferHandle handle, nvrhi::ResourceStates targetState);
    void emitTextureAliasingBarrier(nvrhi::ICommandList* commandList, TextureHandle handle);
    void emitBufferAliasingBarrier(nvrhi::ICommandList* commandList, BufferHandle handle);
    void syncPassEndStates(const Pass& pass);
    static bool passUsesTextureAsWrite(const Pass& pass, TextureHandle handle);
    static bool passUsesBufferAsWrite(const Pass& pass, BufferHandle handle);
    void transitionExtractedResources(nvrhi::ICommandList* commandList);

    nvrhi::IDevice* m_device = nullptr;
    RenderTargetPool* m_renderTargetPool = nullptr;
    RenderBufferPool* m_renderBufferPool = nullptr;
    bool m_compiled = false;
    std::vector<GraphTexture> m_textures;
    std::vector<GraphBuffer> m_buffers;
    std::vector<Pass> m_passes;
    std::vector<uint32_t> m_compiledPassOrder;
    std::vector<std::string> m_passNames;
    std::unordered_map<nvrhi::ITexture*, uint32_t> m_importIndexByTexture;
    std::unordered_map<nvrhi::IBuffer*, uint32_t> m_importIndexByBuffer;
    std::vector<nvrhi::HeapHandle> m_transientHeaps;
    std::vector<nvrhi::HeapHandle> m_transientHeapPool;
    std::vector<TextureAliasingBarrier> m_textureAliasingBarriers;
    std::vector<BufferAliasingBarrier> m_bufferAliasingBarriers;
    TransientResourceStats m_transientStats;
};

} // namespace caustica::rg
