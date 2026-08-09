#include <render/graph/GraphBuilder.h>
#include <render/graph/RenderBufferPool.h>
#include <render/graph/RenderTargetPool.h>
#include <render/graph/TransientResourceAllocator.h>
#include <core/task/TaskRuntime.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

namespace caustica::rg
{

namespace
{
    bool isValid(TextureHandle handle, size_t textureCount)
    {
        return handle.isValid() && handle.index < textureCount;
    }

    bool isValid(BufferHandle handle, size_t bufferCount)
    {
        return handle.isValid() && handle.index < bufferCount;
    }

}

PassBuilder::PassBuilder(
    GraphBuilder& graph,
    TextureAccessList& textureReads,
    TextureAccessList& textureWrites,
    BufferAccessList& bufferReads,
    BufferAccessList& bufferWrites)
    : m_graph(&graph)
    , m_textureReads(&textureReads)
    , m_textureWrites(&textureWrites)
    , m_bufferReads(&bufferReads)
    , m_bufferWrites(&bufferWrites)
{
}

void PassBuilder::read(TextureHandle texture, TextureAccess access)
{
    m_textureReads->emplace_back(texture, access);
}

void PassBuilder::write(TextureHandle texture, TextureAccess access)
{
    m_textureWrites->emplace_back(texture, access);
}

void PassBuilder::read(BufferHandle buffer, BufferAccess access)
{
    m_bufferReads->emplace_back(buffer, access);
}

void PassBuilder::write(BufferHandle buffer, BufferAccess access)
{
    m_bufferWrites->emplace_back(buffer, access);
}

TextureHandle PassBuilder::createTexture(const TextureDesc& desc)
{
    assert(m_graph);
    return m_graph->createTexture(desc);
}

BufferHandle PassBuilder::createBuffer(const BufferDesc& desc)
{
    assert(m_graph);
    return m_graph->createBuffer(desc);
}

RenderPassContext::RenderPassContext(caustica::rhi::CommandList* commandList, const GraphBuilder& graph)
    : m_commandList(commandList)
    , m_graph(&graph)
{
}

caustica::rhi::Texture* RenderPassContext::texture(TextureHandle handle) const
{
    assert(m_graph);
    return m_graph->resolveTexture(handle);
}

caustica::rhi::Buffer* RenderPassContext::buffer(BufferHandle handle) const
{
    assert(m_graph);
    return m_graph->resolveBuffer(handle);
}

caustica::rhi::ResourceStates GraphBuilder::accessToState(TextureAccess access)
{
    switch (access)
    {
    case TextureAccess::ShaderResource:
        return caustica::rhi::ResourceStates::ShaderResource;
    case TextureAccess::RenderTarget:
        return caustica::rhi::ResourceStates::RenderTarget;
    case TextureAccess::DepthWrite:
        return caustica::rhi::ResourceStates::DepthWrite;
    case TextureAccess::UnorderedAccess:
        return caustica::rhi::ResourceStates::UnorderedAccess;
    case TextureAccess::CopySource:
        return caustica::rhi::ResourceStates::CopySource;
    case TextureAccess::CopyDest:
        return caustica::rhi::ResourceStates::CopyDest;
    default:
        return caustica::rhi::ResourceStates::Common;
    }
}

caustica::rhi::ResourceStates GraphBuilder::accessToState(BufferAccess access)
{
    switch (access)
    {
    case BufferAccess::ShaderResource:
        return caustica::rhi::ResourceStates::ShaderResource;
    case BufferAccess::UnorderedAccess:
        return caustica::rhi::ResourceStates::UnorderedAccess;
    case BufferAccess::ConstantBuffer:
        return caustica::rhi::ResourceStates::ConstantBuffer;
    case BufferAccess::CopySource:
        return caustica::rhi::ResourceStates::CopySource;
    case BufferAccess::CopyDest:
        return caustica::rhi::ResourceStates::CopyDest;
    case BufferAccess::VertexBuffer:
        return caustica::rhi::ResourceStates::VertexBuffer;
    case BufferAccess::IndexBuffer:
        return caustica::rhi::ResourceStates::IndexBuffer;
    case BufferAccess::IndirectArgument:
        return caustica::rhi::ResourceStates::IndirectArgument;
    case BufferAccess::AccelStructBuildInput:
        return caustica::rhi::ResourceStates::AccelStructBuildInput;
    default:
        return caustica::rhi::ResourceStates::Common;
    }
}

void GraphBuilder::setDevice(caustica::rhi::Device* device)
{
    if (m_device != device)
    {
        m_transientHeaps.clear();
        m_transientHeapPool.clear();
        m_persistentTransients = {};
        m_compiledPlanCache.clear();
        m_gpuTimingSlots = {};
        m_activeGpuTimingSlot = -1;
    }
    m_device = device;
}

size_t GraphBuilder::activePassCount() const
{
    size_t count = 0;
    for (const Pass& pass : m_passes)
    {
        if (pass.active)
            ++count;
    }
    return count;
}

bool GraphBuilder::isPassRegistered(const std::string_view name) const
{
    for (const Pass& pass : m_passes)
    {
        if (pass.name == name)
            return true;
    }
    return false;
}

bool GraphBuilder::isPassActive(const std::string_view name) const
{
    assert(m_compiled);

    for (const uint32_t passIndex : m_compiledPassOrder)
    {
        if (passIndex >= m_passes.size())
            continue;

        const Pass& pass = m_passes[passIndex];
        if (pass.active && pass.name == name)
            return true;
    }
    return false;
}

TextureHandle GraphBuilder::importTexture(caustica::rhi::Texture* texture, caustica::rhi::ResourceStates initialState)
{
    assert(texture);

    if (const auto existing = m_importIndexByTexture.find(texture); existing != m_importIndexByTexture.end())
        return TextureHandle{ existing->second };

    const TextureHandle handle{ static_cast<uint32_t>(m_textures.size()) };
    GraphTexture imported{};
    imported.texture = texture;
    imported.currentState = initialState;
    imported.lifetime = ResourceLifetime::Imported;
    m_textures.push_back(imported);
    m_importIndexByTexture.emplace(texture, handle.index);
    return handle;
}

TextureHandle GraphBuilder::importTexture(caustica::rhi::Texture* texture, TextureAccess initialAccess)
{
    return importTexture(texture, accessToState(initialAccess));
}

BufferHandle GraphBuilder::importBuffer(caustica::rhi::Buffer* buffer, caustica::rhi::ResourceStates initialState)
{
    assert(buffer);

    if (const auto existing = m_importIndexByBuffer.find(buffer); existing != m_importIndexByBuffer.end())
        return BufferHandle{ existing->second };

    const BufferHandle handle{ static_cast<uint32_t>(m_buffers.size()) };
    GraphBuffer imported{};
    imported.buffer = buffer;
    imported.currentState = initialState;
    imported.lifetime = ResourceLifetime::Imported;
    m_buffers.push_back(imported);
    m_importIndexByBuffer.emplace(buffer, handle.index);
    return handle;
}

BufferHandle GraphBuilder::importBuffer(caustica::rhi::Buffer* buffer, BufferAccess initialAccess)
{
    return importBuffer(buffer, accessToState(initialAccess));
}

caustica::rhi::TextureHandle GraphBuilder::createNativeTexture(const TextureDesc& desc, bool isVirtual) const
{
    assert(m_device);

    const FormatInfo formatInfo = getFormatInfo(desc.format);
    caustica::rhi::TextureDesc nativeDesc;
    nativeDesc.debugName = desc.name.empty() ? "rg_transient" : desc.name.c_str();
    nativeDesc.width = desc.width;
    nativeDesc.height = desc.height;
    nativeDesc.depth = desc.depth;
    nativeDesc.mipLevels = desc.mipLevels;
    nativeDesc.arraySize = desc.arraySize;
    nativeDesc.format = toNativeFormat(desc.format);
    nativeDesc.isRenderTarget = desc.isRenderTarget || formatInfo.isRenderTargetCompatible;
    nativeDesc.isUAV = desc.isUAV || formatInfo.isUAVCompatible;
    nativeDesc.isTypeless = desc.isTypeless;
    nativeDesc.isVirtual = isVirtual;
    nativeDesc.initialState = caustica::rhi::ResourceStates::Common;
    nativeDesc.keepInitialState = true;

    return m_device->createTexture(nativeDesc);
}

TextureHandle GraphBuilder::createTexture(const TextureDesc& desc)
{
    assert(m_device);

    const TextureHandle handle{ static_cast<uint32_t>(m_textures.size()) };
    GraphTexture resource{};
    resource.currentState = caustica::rhi::ResourceStates::Common;
    resource.lifetime = ResourceLifetime::Transient;
    resource.desc = desc;
    m_textures.push_back(resource);
    return handle;
}

caustica::rhi::BufferHandle GraphBuilder::createNativeBuffer(const BufferDesc& desc, bool isVirtual) const
{
    assert(m_device);

    caustica::rhi::BufferDesc nativeDesc;
    nativeDesc.debugName = desc.name.empty() ? "rg_transient_buffer" : desc.name;
    nativeDesc.byteSize = desc.byteSize;
    nativeDesc.structStride = desc.isStructuredBuffer ? desc.structuredStride : 0;
    nativeDesc.isConstantBuffer = desc.isConstantBuffer;
    nativeDesc.canHaveUAVs = desc.isUAV;
    nativeDesc.isVertexBuffer = desc.isVertexBuffer;
    nativeDesc.isIndexBuffer = desc.isIndexBuffer;
    nativeDesc.isDrawIndirectArgs = desc.isDrawIndirectArgs;
    nativeDesc.canHaveRawViews = desc.canHaveRawViews;
    nativeDesc.canHaveTypedViews = desc.canHaveTypedViews;
    nativeDesc.format = toNativeFormat(desc.format);
    nativeDesc.isVirtual = isVirtual;
    nativeDesc.initialState = caustica::rhi::ResourceStates::Common;
    nativeDesc.keepInitialState = true;

    return m_device->createBuffer(nativeDesc);
}

BufferHandle GraphBuilder::createBuffer(const BufferDesc& desc)
{
    assert(m_device);

    const BufferHandle handle{ static_cast<uint32_t>(m_buffers.size()) };
    GraphBuffer resource{};
    resource.currentState = caustica::rhi::ResourceStates::Common;
    resource.lifetime = ResourceLifetime::Transient;
    resource.desc = desc;
    m_buffers.push_back(resource);
    return handle;
}

void GraphBuilder::extractTexture(TextureHandle handle, caustica::rhi::ResourceStates finalState)
{
    assert(isValid(handle, m_textures.size()) && "RenderGraph extract references invalid texture handle");
    if (!isValid(handle, m_textures.size()))
        return;
    m_textures[handle.index].finalState = finalState;
}

void GraphBuilder::extractTexture(TextureHandle handle, TextureAccess finalAccess)
{
    extractTexture(handle, accessToState(finalAccess));
}

void GraphBuilder::extractBuffer(BufferHandle handle, caustica::rhi::ResourceStates finalState)
{
    assert(isValid(handle, m_buffers.size()) && "RenderGraph extract references invalid buffer handle");
    if (!isValid(handle, m_buffers.size()))
        return;
    m_buffers[handle.index].finalState = finalState;
}

void GraphBuilder::extractBuffer(BufferHandle handle, BufferAccess finalAccess)
{
    extractBuffer(handle, accessToState(finalAccess));
}

void GraphBuilder::addPass(std::string_view name, SetupFn setup, ExecuteFn execute, PassOptions options)
{
    Pass pass;
    const size_t passIndex = m_passes.size();
    if (passIndex < m_recycledPasses.size())
        pass = std::move(m_recycledPasses[passIndex]);
    pass.textureReads.clear();
    pass.textureWrites.clear();
    pass.bufferReads.clear();
    pass.bufferWrites.clear();
    pass.name.assign(name);
    pass.setup = std::move(setup);
    pass.execute = std::move(execute);
    pass.options = options;
    pass.gpuTimer = nullptr;

    if (pass.options.enabled && pass.setup)
    {
        PassBuilder builder(
            *this,
            pass.textureReads,
            pass.textureWrites,
            pass.bufferReads,
            pass.bufferWrites);
        pass.setup(builder);
    }

    m_passNames.push_back(pass.name);
    m_passes.push_back(std::move(pass));
    m_compiled = false;
}

uint64_t GraphBuilder::compiledPlanKey() const
{
    constexpr uint64_t offset = 1469598103934665603ull;
    constexpr uint64_t prime = 1099511628211ull;
    uint64_t hash = offset;
    const auto mix = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= prime;
        }
    };
    const auto mixValue = [&](const auto& value) { mix(&value, sizeof(value)); };
    const auto mixString = [&](std::string_view value) {
        mix(value.data(), value.size());
        const uint8_t terminator = 0xff;
        mixValue(terminator);
    };

    mixValue(m_passes.size());
    mixValue(m_textures.size());
    mixValue(m_buffers.size());
    for (const GraphTexture& texture : m_textures)
    {
        mixValue(texture.lifetime);
        if (texture.lifetime == ResourceLifetime::Transient)
        {
            const uint64_t descHash = hashTextureDesc(texture.desc);
            mixValue(descHash);
        }
        const bool hasFinal = texture.finalState.has_value();
        mixValue(hasFinal);
        if (hasFinal)
            mixValue(*texture.finalState);
    }
    for (const GraphBuffer& buffer : m_buffers)
    {
        mixValue(buffer.lifetime);
        if (buffer.lifetime == ResourceLifetime::Transient)
        {
            const uint64_t descHash = hashBufferDesc(buffer.desc);
            mixValue(descHash);
        }
        const bool hasFinal = buffer.finalState.has_value();
        mixValue(hasFinal);
        if (hasFinal)
            mixValue(*buffer.finalState);
    }
    for (const Pass& pass : m_passes)
    {
        mixString(pass.name);
        mixValue(pass.options.enabled);
        mixValue(pass.options.sideEffect);
        mixValue(pass.options.serialOnPrimary);
        mixValue(pass.options.recordingCost);
        mixString(pass.options.executeAfter ? std::string_view(pass.options.executeAfter) : std::string_view{});
        const auto mixAccesses = [&](const auto& accesses) {
            mixValue(accesses.size());
            for (const auto& [handle, access] : accesses)
            {
                mixValue(handle.index);
                mixValue(access);
            }
        };
        mixAccesses(pass.textureReads);
        mixAccesses(pass.textureWrites);
        mixAccesses(pass.bufferReads);
        mixAccesses(pass.bufferWrites);
    }
    return hash;
}

void GraphBuilder::compile()
{
    for (Pass& pass : m_passes)
        pass.active = false;
    m_compiledPassOrder.clear();
    m_compiledWaves.clear();
    m_lastCompileCacheHit = false;

    const uint64_t planKey = compiledPlanKey();
    if (const auto cached = m_compiledPlanCache.find(planKey);
        cached != m_compiledPlanCache.end()
        && cached->second.activePasses.size() == m_passes.size()
        && cached->second.referencedTextures.size() == m_textures.size()
        && cached->second.referencedBuffers.size() == m_buffers.size())
    {
        const CompiledPlan& plan = cached->second;
        for (size_t i = 0; i < m_passes.size(); ++i)
            m_passes[i].active = plan.activePasses[i];
        m_compiledPassOrder = plan.passOrder;
        m_compiledWaves = plan.waves;

        for (size_t i = 0; i < m_textures.size(); ++i)
        {
            GraphTexture& resource = m_textures[i];
            if (resource.lifetime == ResourceLifetime::Transient && !plan.referencedTextures[i])
            {
                resource.owned = nullptr;
                resource.texture = nullptr;
            }
        }
        for (size_t i = 0; i < m_buffers.size(); ++i)
        {
            GraphBuffer& resource = m_buffers[i];
            if (resource.lifetime == ResourceLifetime::Transient && !plan.referencedBuffers[i])
            {
                resource.owned = nullptr;
                resource.buffer = nullptr;
            }
        }

        if (!restorePersistentTransientResources(planKey))
        {
            allocateTransientResources(
                plan.referencedTextures,
                plan.referencedBuffers,
                plan.textureLifetimes,
                plan.bufferLifetimes);
            capturePersistentTransientResources(planKey);
        }
        m_lastCompileCacheHit = true;
        m_compiled = true;
        return;
    }

    std::vector<std::vector<uint32_t>> incoming(m_passes.size());
    std::vector<std::vector<uint32_t>> outgoing(m_passes.size());
    std::vector<bool> rootPass(m_passes.size(), false);
    std::vector<bool> referenced(m_textures.size(), false);
    std::vector<bool> referencedBuffers(m_buffers.size(), false);
    std::vector<int32_t> lastTextureWriter(m_textures.size(), -1);
    std::vector<int32_t> lastBufferWriter(m_buffers.size(), -1);
    std::vector<std::vector<uint32_t>> lastTextureReaders(m_textures.size());
    std::vector<std::vector<uint32_t>> lastBufferReaders(m_buffers.size());

    const auto addDependency = [&](uint32_t before, uint32_t after) {
        if (before == after)
            return;
        if (std::find(outgoing[before].begin(), outgoing[before].end(), after) != outgoing[before].end())
            return;

        outgoing[before].push_back(after);
        incoming[after].push_back(before);
    };

    for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(m_passes.size()); ++passIndex)
    {
        const Pass& pass = m_passes[passIndex];
        if (!pass.options.enabled)
            continue;

        rootPass[passIndex] = pass.options.sideEffect;

        for (const auto& [handle, access] : pass.textureReads)
        {
            (void)access;
            assert(isValid(handle, m_textures.size()) && "RenderGraph pass read references invalid texture handle");
            if (!isValid(handle, m_textures.size()))
                continue;

            if (lastTextureWriter[handle.index] >= 0)
                addDependency(static_cast<uint32_t>(lastTextureWriter[handle.index]), passIndex);
            lastTextureReaders[handle.index].push_back(passIndex);
        }
        for (const auto& [handle, access] : pass.textureWrites)
        {
            (void)access;
            assert(isValid(handle, m_textures.size()) && "RenderGraph pass write references invalid texture handle");
            if (!isValid(handle, m_textures.size()))
                continue;

            // WAR: writers wait for prior readers in the same resource.
            for (const uint32_t reader : lastTextureReaders[handle.index])
                addDependency(reader, passIndex);
            lastTextureReaders[handle.index].clear();

            if (lastTextureWriter[handle.index] >= 0)
                addDependency(static_cast<uint32_t>(lastTextureWriter[handle.index]), passIndex);
            lastTextureWriter[handle.index] = static_cast<int32_t>(passIndex);
        }
        for (const auto& [handle, access] : pass.bufferReads)
        {
            (void)access;
            assert(isValid(handle, m_buffers.size()) && "RenderGraph pass read references invalid buffer handle");
            if (!isValid(handle, m_buffers.size()))
                continue;

            if (lastBufferWriter[handle.index] >= 0)
                addDependency(static_cast<uint32_t>(lastBufferWriter[handle.index]), passIndex);
            lastBufferReaders[handle.index].push_back(passIndex);
        }
        for (const auto& [handle, access] : pass.bufferWrites)
        {
            (void)access;
            assert(isValid(handle, m_buffers.size()) && "RenderGraph pass write references invalid buffer handle");
            if (!isValid(handle, m_buffers.size()))
                continue;

            for (const uint32_t reader : lastBufferReaders[handle.index])
                addDependency(reader, passIndex);
            lastBufferReaders[handle.index].clear();

            if (lastBufferWriter[handle.index] >= 0)
                addDependency(static_cast<uint32_t>(lastBufferWriter[handle.index]), passIndex);
            lastBufferWriter[handle.index] = static_cast<int32_t>(passIndex);
        }
    }

    for (size_t i = 0; i < m_textures.size(); ++i)
    {
        if (m_textures[i].finalState.has_value())
        {
            referenced[i] = true;
            if (lastTextureWriter[i] >= 0)
                rootPass[static_cast<uint32_t>(lastTextureWriter[i])] = true;
        }
    }

    for (size_t i = 0; i < m_buffers.size(); ++i)
    {
        if (m_buffers[i].finalState.has_value())
        {
            referencedBuffers[i] = true;
            if (lastBufferWriter[i] >= 0)
                rootPass[static_cast<uint32_t>(lastBufferWriter[i])] = true;
        }
    }

    for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(m_passes.size()); ++passIndex)
    {
        const Pass& pass = m_passes[passIndex];
        if (!pass.options.enabled || pass.options.executeAfter == nullptr)
            continue;

        const auto predecessor = std::find_if(
            m_passes.begin(),
            m_passes.end(),
            [&](const Pass& candidate) { return candidate.name == pass.options.executeAfter; });
        if (predecessor == m_passes.end())
            continue;

        const uint32_t predecessorIndex = static_cast<uint32_t>(std::distance(m_passes.begin(), predecessor));
        addDependency(predecessorIndex, passIndex);
    }

    std::vector<bool> needed(m_passes.size(), false);
    std::vector<uint32_t> stack;
    for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(m_passes.size()); ++passIndex)
    {
        if (!rootPass[passIndex])
            continue;

        needed[passIndex] = true;
        stack.push_back(passIndex);
    }

    while (!stack.empty())
    {
        const uint32_t passIndex = stack.back();
        stack.pop_back();

        for (const uint32_t dependency : incoming[passIndex])
        {
            if (needed[dependency])
                continue;

            needed[dependency] = true;
            stack.push_back(dependency);
        }
    }

    for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(m_passes.size()); ++passIndex)
    {
        if (!needed[passIndex])
            continue;

        Pass& pass = m_passes[passIndex];
        pass.active = true;

        for (const auto& [handle, access] : pass.textureReads)
        {
            (void)access;
            if (isValid(handle, m_textures.size()))
                referenced[handle.index] = true;
        }

        for (const auto& [handle, access] : pass.textureWrites)
        {
            (void)access;
            if (isValid(handle, m_textures.size()))
                referenced[handle.index] = true;
        }

        for (const auto& [handle, access] : pass.bufferReads)
        {
            (void)access;
            if (isValid(handle, m_buffers.size()))
                referencedBuffers[handle.index] = true;
        }

        for (const auto& [handle, access] : pass.bufferWrites)
        {
            (void)access;
            if (isValid(handle, m_buffers.size()))
                referencedBuffers[handle.index] = true;
        }
    }

    buildCompiledWaves(needed, incoming, outgoing);

    size_t neededPassCount = 0;
    for (const bool isNeeded : needed)
    {
        if (isNeeded)
            ++neededPassCount;
    }

    if (m_compiledPassOrder.size() != neededPassCount)
    {
        assert(false && "RenderGraph dependency cycle detected");
        m_compiledPassOrder.clear();
        m_compiledWaves.clear();
        for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(m_passes.size()); ++passIndex)
        {
            if (needed[passIndex])
                m_compiledPassOrder.push_back(passIndex);
        }
        for (const uint32_t passIndex : m_compiledPassOrder)
            m_compiledWaves.push_back({ passIndex });
    }

    for (size_t i = 0; i < m_textures.size(); ++i)
    {
        GraphTexture& resource = m_textures[i];
        if (resource.lifetime != ResourceLifetime::Transient || referenced[i])
            continue;

        resource.owned = nullptr;
        resource.texture = nullptr;
    }

    for (size_t i = 0; i < m_buffers.size(); ++i)
    {
        GraphBuffer& resource = m_buffers[i];
        if (resource.lifetime != ResourceLifetime::Transient || referencedBuffers[i])
            continue;

        resource.owned = nullptr;
        resource.buffer = nullptr;
    }

    std::vector<TransientLifetime> textureLifetimes;
    std::vector<TransientLifetime> bufferLifetimes;
    computeTransientLifetimes(textureLifetimes, bufferLifetimes);

    CompiledPlan plan;
    plan.activePasses.reserve(m_passes.size());
    for (const Pass& pass : m_passes)
        plan.activePasses.push_back(pass.active);
    plan.passOrder = m_compiledPassOrder;
    plan.waves = m_compiledWaves;
    plan.referencedTextures = referenced;
    plan.referencedBuffers = referencedBuffers;
    plan.textureLifetimes = textureLifetimes;
    plan.bufferLifetimes = bufferLifetimes;
    if (m_compiledPlanCache.size() >= 16)
        m_compiledPlanCache.erase(m_compiledPlanCache.begin());
    m_compiledPlanCache.insert_or_assign(planKey, std::move(plan));

    allocateTransientResources(referenced, referencedBuffers, textureLifetimes, bufferLifetimes);
    capturePersistentTransientResources(planKey);
    m_compiled = true;
}

void GraphBuilder::computeTransientLifetimes(
    std::vector<TransientLifetime>& textureLifetimes,
    std::vector<TransientLifetime>& bufferLifetimes) const
{
    textureLifetimes.assign(m_textures.size(), {});
    bufferLifetimes.assign(m_buffers.size(), {});

    // Passes in one wave may execute concurrently. Give them the same lifetime
    // coordinate so the transient allocator can never alias their resources.
    for (size_t waveIndex = 0; waveIndex < m_compiledWaves.size(); ++waveIndex)
    {
        const int32_t order = static_cast<int32_t>(waveIndex);

        const auto touchTexture = [&](TextureHandle handle) {
            if (!isValid(handle, m_textures.size()))
                return;
            if (m_textures[handle.index].lifetime != ResourceLifetime::Transient)
                return;

            TransientLifetime& lifetime = textureLifetimes[handle.index];
            lifetime.firstPassOrder = std::min(lifetime.firstPassOrder, order);
            lifetime.lastPassOrder = std::max(lifetime.lastPassOrder, order);
        };

        const auto touchBuffer = [&](BufferHandle handle) {
            if (!isValid(handle, m_buffers.size()))
                return;
            if (m_buffers[handle.index].lifetime != ResourceLifetime::Transient)
                return;

            TransientLifetime& lifetime = bufferLifetimes[handle.index];
            lifetime.firstPassOrder = std::min(lifetime.firstPassOrder, order);
            lifetime.lastPassOrder = std::max(lifetime.lastPassOrder, order);
        };

        for (const uint32_t passIndex : m_compiledWaves[waveIndex])
        {
            if (passIndex >= m_passes.size())
                continue;
            const Pass& pass = m_passes[passIndex];
            if (!pass.active)
                continue;

            for (const auto& [handle, access] : pass.textureReads)
            {
                (void)access;
                touchTexture(handle);
            }
            for (const auto& [handle, access] : pass.textureWrites)
            {
                (void)access;
                touchTexture(handle);
            }
            for (const auto& [handle, access] : pass.bufferReads)
            {
                (void)access;
                touchBuffer(handle);
            }
            for (const auto& [handle, access] : pass.bufferWrites)
            {
                (void)access;
                touchBuffer(handle);
            }
        }
    }
}

void GraphBuilder::allocateTransientResources(
    const std::vector<bool>& referencedTextures,
    const std::vector<bool>& referencedBuffers,
    const std::vector<TransientLifetime>& textureLifetimes,
    const std::vector<TransientLifetime>& bufferLifetimes)
{
    m_textureAliasingBarriers.clear();
    m_bufferAliasingBarriers.clear();
    m_transientStats = {};

    TransientResourceAllocator allocator;
    allocator.allocate(*this, referencedTextures, referencedBuffers, textureLifetimes, bufferLifetimes);
}

bool GraphBuilder::restorePersistentTransientResources(uint64_t planKey)
{
    const PersistentTransientResources& persistent = m_persistentTransients;
    if (!persistent.valid
        || persistent.planKey != planKey
        || persistent.textures.size() != m_textures.size()
        || persistent.buffers.size() != m_buffers.size())
    {
        return false;
    }

    for (size_t i = 0; i < m_textures.size(); ++i)
    {
        GraphTexture& resource = m_textures[i];
        if (resource.lifetime != ResourceLifetime::Transient)
            continue;

        resource.owned = persistent.textures[i];
        resource.texture = resource.owned;
        resource.currentState = caustica::rhi::ResourceStates::Common;
    }

    for (size_t i = 0; i < m_buffers.size(); ++i)
    {
        GraphBuffer& resource = m_buffers[i];
        if (resource.lifetime != ResourceLifetime::Transient)
            continue;

        resource.owned = persistent.buffers[i];
        resource.buffer = resource.owned;
        resource.currentState = caustica::rhi::ResourceStates::Common;
    }

    m_textureAliasingBarriers = persistent.textureBarriers;
    m_bufferAliasingBarriers = persistent.bufferBarriers;
    m_transientStats = persistent.stats;
    m_transientStats.createdHeapCount = 0;
    m_transientStats.reusedHeapCount = static_cast<uint32_t>(persistent.heaps.size());
    return true;
}

void GraphBuilder::capturePersistentTransientResources(uint64_t planKey)
{
    // Pool entries are frame leases and become available again at endFrame().
    // Only graph-owned resources can safely outlive a frame.
    if (m_transientStats.pooledTextureCount != 0 || m_transientStats.pooledBufferCount != 0)
    {
        m_persistentTransients = {};
        return;
    }

    PersistentTransientResources persistent;
    persistent.planKey = planKey;
    persistent.valid = true;
    persistent.textures.resize(m_textures.size());
    persistent.buffers.resize(m_buffers.size());

    for (size_t i = 0; i < m_textures.size(); ++i)
    {
        if (m_textures[i].lifetime == ResourceLifetime::Transient)
            persistent.textures[i] = m_textures[i].owned;
    }
    for (size_t i = 0; i < m_buffers.size(); ++i)
    {
        if (m_buffers[i].lifetime == ResourceLifetime::Transient)
            persistent.buffers[i] = m_buffers[i].owned;
    }

    persistent.heaps = std::move(m_transientHeaps);
    persistent.textureBarriers = m_textureAliasingBarriers;
    persistent.bufferBarriers = m_bufferAliasingBarriers;
    persistent.stats = m_transientStats;
    m_persistentTransients = std::move(persistent);
}

void GraphBuilder::releaseTransientResources()
{
    for (GraphTexture& resource : m_textures)
    {
        if (resource.lifetime == ResourceLifetime::Transient)
        {
            resource.owned = nullptr;
            resource.texture = nullptr;
        }
    }

    for (GraphBuffer& resource : m_buffers)
    {
        if (resource.lifetime == ResourceLifetime::Transient)
        {
            resource.owned = nullptr;
            resource.buffer = nullptr;
        }
    }

    for (caustica::rhi::HeapHandle& heap : m_transientHeaps)
    {
        if (heap)
            m_transientHeapPool.push_back(heap);
    }
    m_transientHeaps.clear();
    m_textureAliasingBarriers.clear();
    m_bufferAliasingBarriers.clear();
}

void GraphBuilder::transitionTexture(caustica::rhi::CommandList* commandList, TextureHandle handle, TextureAccess access)
{
    transitionTexture(commandList, handle, accessToState(access));
}

void GraphBuilder::transitionTexture(caustica::rhi::CommandList* commandList, TextureHandle handle, caustica::rhi::ResourceStates targetState)
{
    if (!isValid(handle, m_textures.size()))
        return;

    GraphTexture& resource = m_textures[handle.index];
    if (!resource.texture)
        return;

    if (resource.currentState == targetState)
        return;

    commandList->setTextureState(resource.texture, caustica::rhi::AllSubresources, targetState);
    resource.currentState = targetState;
}

void GraphBuilder::transitionBuffer(caustica::rhi::CommandList* commandList, BufferHandle handle, BufferAccess access)
{
    transitionBuffer(commandList, handle, accessToState(access));
}

void GraphBuilder::transitionBuffer(caustica::rhi::CommandList* commandList, BufferHandle handle, caustica::rhi::ResourceStates targetState)
{
    if (!isValid(handle, m_buffers.size()))
        return;

    GraphBuffer& resource = m_buffers[handle.index];
    if (!resource.buffer)
        return;

    if (resource.currentState == targetState)
        return;

    commandList->setBufferState(resource.buffer, targetState);
    resource.currentState = targetState;
}

void GraphBuilder::emitTextureAliasingBarrier(caustica::rhi::CommandList* commandList, TextureHandle handle)
{
    if (!isValid(handle, m_textures.size()))
        return;

    for (TextureAliasingBarrier& barrier : m_textureAliasingBarriers)
    {
        if (barrier.emitted || barrier.after.index != handle.index)
            continue;

        caustica::rhi::Texture* before = isValid(barrier.before, m_textures.size())
            ? m_textures[barrier.before.index].texture
            : nullptr;
        caustica::rhi::Texture* after = m_textures[handle.index].texture;
        if (after)
        {
            commandList->textureAliasingBarrier(before, after);
            ++m_transientStats.aliasingBarrierCount;
        }
        barrier.emitted = true;
    }
}

void GraphBuilder::emitBufferAliasingBarrier(caustica::rhi::CommandList* commandList, BufferHandle handle)
{
    if (!isValid(handle, m_buffers.size()))
        return;

    for (BufferAliasingBarrier& barrier : m_bufferAliasingBarriers)
    {
        if (barrier.emitted || barrier.after.index != handle.index)
            continue;

        caustica::rhi::Buffer* before = isValid(barrier.before, m_buffers.size())
            ? m_buffers[barrier.before.index].buffer
            : nullptr;
        caustica::rhi::Buffer* after = m_buffers[handle.index].buffer;
        if (after)
        {
            commandList->bufferAliasingBarrier(before, after);
            ++m_transientStats.aliasingBarrierCount;
        }
        barrier.emitted = true;
    }
}

void GraphBuilder::syncPassEndStates(const Pass& pass)
{
    for (const auto& [handle, access] : pass.textureWrites)
    {
        if (isValid(handle, m_textures.size()))
            m_textures[handle.index].currentState = accessToState(access);
    }

    for (const auto& [handle, access] : pass.textureReads)
    {
        if (!isValid(handle, m_textures.size()) || passUsesTextureAsWrite(pass, handle))
            continue;
        m_textures[handle.index].currentState = accessToState(access);
    }

    for (const auto& [handle, access] : pass.bufferWrites)
    {
        if (isValid(handle, m_buffers.size()))
            m_buffers[handle.index].currentState = accessToState(access);
    }

    for (const auto& [handle, access] : pass.bufferReads)
    {
        if (!isValid(handle, m_buffers.size()) || passUsesBufferAsWrite(pass, handle))
            continue;
        m_buffers[handle.index].currentState = accessToState(access);
    }
}

bool GraphBuilder::passUsesTextureAsWrite(const Pass& pass, TextureHandle handle)
{
    for (const auto& [writeHandle, access] : pass.textureWrites)
    {
        if (writeHandle.index == handle.index)
            return true;
        (void)access;
    }
    return false;
}

bool GraphBuilder::passUsesBufferAsWrite(const Pass& pass, BufferHandle handle)
{
    for (const auto& [writeHandle, access] : pass.bufferWrites)
    {
        if (writeHandle.index == handle.index)
            return true;
        (void)access;
    }
    return false;
}

void GraphBuilder::transitionExtractedResources(caustica::rhi::CommandList* commandList)
{
    bool hasTransitions = false;

    for (size_t i = 0; i < m_textures.size(); ++i)
    {
        GraphTexture& resource = m_textures[i];
        if (!resource.finalState.has_value())
            continue;

        const caustica::rhi::ResourceStates before = resource.currentState;
        transitionTexture(commandList, TextureHandle{ static_cast<uint32_t>(i) }, *resource.finalState);
        hasTransitions = hasTransitions || before != resource.currentState;
    }

    for (size_t i = 0; i < m_buffers.size(); ++i)
    {
        GraphBuffer& resource = m_buffers[i];
        if (!resource.finalState.has_value())
            continue;

        const caustica::rhi::ResourceStates before = resource.currentState;
        transitionBuffer(commandList, BufferHandle{ static_cast<uint32_t>(i) }, *resource.finalState);
        hasTransitions = hasTransitions || before != resource.currentState;
    }

    if (hasTransitions)
        commandList->commitBarriers();
}

void GraphBuilder::buildCompiledWaves(
    const std::vector<bool>& needed,
    const std::vector<std::vector<uint32_t>>& incoming,
    const std::vector<std::vector<uint32_t>>& outgoing)
{
    m_compiledPassOrder.clear();
    m_compiledWaves.clear();

    std::vector<uint32_t> indegree(m_passes.size(), 0);
    for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(m_passes.size()); ++passIndex)
    {
        if (!needed[passIndex])
            continue;
        for (const uint32_t dependency : incoming[passIndex])
        {
            if (needed[dependency])
                ++indegree[passIndex];
        }
    }

    std::vector<bool> emitted(m_passes.size(), false);
    for (;;)
    {
        std::vector<uint32_t> wave;
        wave.reserve(8);
        for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(m_passes.size()); ++passIndex)
        {
            if (!needed[passIndex] || emitted[passIndex] || indegree[passIndex] != 0)
                continue;
            wave.push_back(passIndex);
        }

        if (wave.empty())
            break;

        // serialOnPrimary passes never share a parallel wave.
        std::vector<uint32_t> parallelEligible;
        for (const uint32_t passIndex : wave)
        {
            if (m_passes[passIndex].options.serialOnPrimary)
            {
                m_compiledWaves.push_back({ passIndex });
                m_compiledPassOrder.push_back(passIndex);
                emitted[passIndex] = true;
                for (const uint32_t dependent : outgoing[passIndex])
                {
                    if (needed[dependent] && indegree[dependent] > 0)
                        --indegree[dependent];
                }
            }
            else
            {
                parallelEligible.push_back(passIndex);
            }
        }

        if (!parallelEligible.empty())
        {
            m_compiledWaves.push_back(parallelEligible);
            for (const uint32_t passIndex : parallelEligible)
            {
                m_compiledPassOrder.push_back(passIndex);
                emitted[passIndex] = true;
                for (const uint32_t dependent : outgoing[passIndex])
                {
                    if (needed[dependent] && indegree[dependent] > 0)
                        --indegree[dependent];
                }
            }
        }
    }
}

void GraphBuilder::recordPass(
    caustica::rhi::CommandList* commandList,
    const Pass& pass,
    std::vector<caustica::rhi::ResourceStates>* localTextureStates,
    std::vector<caustica::rhi::ResourceStates>* localBufferStates)
{
    if (pass.gpuTimer)
        commandList->beginTimerQuery(pass.gpuTimer);
    commandList->beginMarker(pass.name.c_str());

    // Aliasing barriers mutate shared emitted flags — only safe on the render thread
    // (serial path, or RT setup before parallel body). Skip when using local state.
    if (!localTextureStates)
    {
        for (const auto& [handle, access] : pass.textureReads)
        {
            (void)access;
            emitTextureAliasingBarrier(commandList, handle);
        }
        for (const auto& [handle, access] : pass.textureWrites)
        {
            (void)access;
            emitTextureAliasingBarrier(commandList, handle);
        }
        for (const auto& [handle, access] : pass.bufferReads)
        {
            (void)access;
            emitBufferAliasingBarrier(commandList, handle);
        }
        for (const auto& [handle, access] : pass.bufferWrites)
        {
            (void)access;
            emitBufferAliasingBarrier(commandList, handle);
        }
    }

    const auto transitionTex = [&](TextureHandle handle, TextureAccess access) {
        if (!isValid(handle, m_textures.size()))
            return;
        GraphTexture& resource = m_textures[handle.index];
        if (!resource.texture)
            return;
        const caustica::rhi::ResourceStates target = accessToState(access);
        caustica::rhi::ResourceStates& current = localTextureStates
            ? (*localTextureStates)[handle.index]
            : resource.currentState;
        if (current == target)
            return;
        commandList->setTextureState(resource.texture, caustica::rhi::AllSubresources, target);
        current = target;
    };
    const auto transitionBuf = [&](BufferHandle handle, BufferAccess access) {
        if (!isValid(handle, m_buffers.size()))
            return;
        GraphBuffer& resource = m_buffers[handle.index];
        if (!resource.buffer)
            return;
        const caustica::rhi::ResourceStates target = accessToState(access);
        caustica::rhi::ResourceStates& current = localBufferStates
            ? (*localBufferStates)[handle.index]
            : resource.currentState;
        if (current == target)
            return;
        commandList->setBufferState(resource.buffer, target);
        current = target;
    };

    for (const auto& [handle, access] : pass.textureReads)
        transitionTex(handle, access);
    for (const auto& [handle, access] : pass.textureWrites)
        transitionTex(handle, access);
    for (const auto& [handle, access] : pass.bufferReads)
        transitionBuf(handle, access);
    for (const auto& [handle, access] : pass.bufferWrites)
        transitionBuf(handle, access);

    if (!pass.textureReads.empty() || !pass.textureWrites.empty() || !pass.bufferReads.empty() || !pass.bufferWrites.empty())
        commandList->commitBarriers();

    // Volatile CBs are per command-list open session (ADR 0001 R2 binder).
    m_volatileConstants.apply(commandList);

    if (pass.execute)
    {
        RenderPassContext context(commandList, *this);
        pass.execute(context);
    }

    commandList->endMarker();
    if (pass.gpuTimer)
        commandList->endTimerQuery(pass.gpuTimer);
}

void GraphBuilder::executeWaveSerial(caustica::rhi::CommandList* commandList, const std::vector<uint32_t>& wave)
{
    for (const uint32_t passIndex : wave)
    {
        if (passIndex >= m_passes.size())
            continue;
        const Pass& pass = m_passes[passIndex];
        if (!pass.active)
            continue;
        recordPass(commandList, pass);
        syncPassEndStates(pass);
    }
}

uint32_t GraphBuilder::executeWaveParallel(
    caustica::rhi::FrameCommandContext& frameCtx,
    const std::vector<uint32_t>& wave,
    ExecuteParams params)
{
    std::vector<uint32_t> activePasses;
    const auto estimatedCost = [&](uint32_t passIndex) {
        const Pass& pass = m_passes[passIndex];
        const auto measured = m_recordingCostHistory.find(pass.name);
        return std::max<double>(
            std::max<uint32_t>(1, pass.options.recordingCost),
            measured == m_recordingCostHistory.end() ? 1.0 : measured->second);
    };
    double totalCost = 0.0;
    for (const uint32_t passIndex : wave)
    {
        if (passIndex >= m_passes.size() || !m_passes[passIndex].active)
            continue;
        activePasses.push_back(passIndex);
        totalCost += estimatedCost(passIndex);
    }

    const uint32_t minCost = std::max(1u, params.minParallelRecordingCost);
    if (activePasses.size() < 2 || totalCost < minCost)
    {
        executeWaveSerial(frameCtx.primary(), wave);
        return 0;
    }

    uint32_t maxJobs = params.maxParallelRecordingJobs;
    if (maxJobs == 0)
        maxJobs = std::max(1u, caustica::task::workerCount());
    const uint32_t costJobs = std::max(
        2u, static_cast<uint32_t>(std::ceil(totalCost / double(minCost))));
    const uint32_t jobCount = std::min<uint32_t>(
        static_cast<uint32_t>(activePasses.size()),
        std::min(maxJobs, costJobs));
    if (jobCount < 2)
    {
        executeWaveSerial(frameCtx.primary(), wave);
        return 0;
    }

    struct RecordingBatch
    {
        std::vector<uint32_t> passes;
        double cost = 0.0;
        caustica::rhi::CommandListHandle commandList;
    };
    std::vector<RecordingBatch> batches(jobCount);
    std::stable_sort(activePasses.begin(), activePasses.end(), [&](uint32_t a, uint32_t b) {
        return estimatedCost(a) > estimatedCost(b);
    });
    for (const uint32_t passIndex : activePasses)
    {
        auto target = std::min_element(
            batches.begin(), batches.end(),
            [](const RecordingBatch& a, const RecordingBatch& b) { return a.cost < b.cost; });
        target->passes.push_back(passIndex);
        target->cost += estimatedCost(passIndex);
    }

    // Prior serial waves may still be pending on the open primary. Flush them
    // before submitting forks so GPU order matches the compiled wave order.
    // WARNING: flush closes the primary and clears volatile CB address maps on
    // that list — later primary passes must writeBuffer those CBs again.
    if (frameCtx.primaryOpen())
        frameCtx.flushPrimary();

    std::vector<caustica::rhi::ResourceStates> waveTextureStates(m_textures.size());
    std::vector<caustica::rhi::ResourceStates> waveBufferStates(m_buffers.size());
    for (size_t i = 0; i < m_textures.size(); ++i)
        waveTextureStates[i] = m_textures[i].currentState;
    for (size_t i = 0; i < m_buffers.size(); ++i)
        waveBufferStates[i] = m_buffers[i].currentState;

    for (RecordingBatch& batch : batches)
    {
        batch.commandList = frameCtx.fork();
        // Emit aliasing on the RT before workers touch the lists.
        for (const uint32_t passIndex : batch.passes)
        {
            const Pass& pass = m_passes[passIndex];
            for (const auto& [handle, access] : pass.textureReads)
            {
                (void)access;
                emitTextureAliasingBarrier(batch.commandList.Get(), handle);
            }
            for (const auto& [handle, access] : pass.textureWrites)
            {
                (void)access;
                emitTextureAliasingBarrier(batch.commandList.Get(), handle);
            }
            for (const auto& [handle, access] : pass.bufferReads)
            {
                (void)access;
                emitBufferAliasingBarrier(batch.commandList.Get(), handle);
            }
            for (const auto& [handle, access] : pass.bufferWrites)
            {
                (void)access;
                emitBufferAliasingBarrier(batch.commandList.Get(), handle);
            }
        }
    }

    caustica::task::Group jobs;
    std::vector<double> recordedCost(activePasses.size(), 0.0);
    std::vector<uint32_t> costSlotByPass(m_passes.size(), UINT32_MAX);
    for (uint32_t i = 0; i < static_cast<uint32_t>(activePasses.size()); ++i)
        costSlotByPass[activePasses[i]] = i;
    caustica::task::parallelFor(
        jobs,
        static_cast<uint32_t>(batches.size()),
        caustica::task::Priority::High,
        caustica::task::Affinity::Any,
        [&](uint32_t slot) {
            auto localTex = waveTextureStates;
            auto localBuf = waveBufferStates;
            RecordingBatch& batch = batches[slot];
            for (const uint32_t passIndex : batch.passes)
            {
                const auto begin = std::chrono::steady_clock::now();
                recordPass(batch.commandList.Get(), m_passes[passIndex], &localTex, &localBuf);
                const double microseconds = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - begin).count();
                // One cost unit is approximately 100 us of command recording.
                recordedCost[costSlotByPass[passIndex]] = std::max(1.0, microseconds / 100.0);
            }
        },
        1);
    caustica::task::wait(jobs);

    for (uint32_t i = 0; i < static_cast<uint32_t>(activePasses.size()); ++i)
    {
        const std::string& name = m_passes[activePasses[i]].name;
        double& history = m_recordingCostHistory[name];
        history = history > 0.0 ? history * 0.8 + recordedCost[i] * 0.2 : recordedCost[i];
    }

    frameCtx.submitForks();

    for (const uint32_t passIndex : wave)
    {
        if (passIndex >= m_passes.size())
            continue;
        const Pass& pass = m_passes[passIndex];
        if (pass.active)
            syncPassEndStates(pass);
    }
    return static_cast<uint32_t>(batches.size());
}

void GraphBuilder::execute(caustica::rhi::FrameCommandContext& frameCtx, ExecuteParams params)
{
    caustica::rhi::CommandList* primary = frameCtx.primary();
    assert(primary);
    assert(frameCtx.primaryOpen());
    if (!m_compiled)
        compile();
    m_lastParallelBatchCount = 0;

    for (const std::vector<uint32_t>& wave : m_compiledWaves)
    {
        if (wave.empty())
            continue;

        const bool forceSerial = !params.parallelWaves || wave.size() == 1;
        bool hasSerialPass = false;
        for (const uint32_t passIndex : wave)
        {
            if (passIndex < m_passes.size() && m_passes[passIndex].options.serialOnPrimary)
            {
                hasSerialPass = true;
                break;
            }
        }

        if (forceSerial || hasSerialPass)
            executeWaveSerial(primary, wave);
        else
            m_lastParallelBatchCount += executeWaveParallel(frameCtx, wave, params);
    }

    transitionExtractedResources(primary);

    if (m_activeGpuTimingSlot >= 0)
    {
        m_gpuTimingSlots[size_t(m_activeGpuTimingSlot)].pending = true;
        m_activeGpuTimingSlot = -1;
    }
}

void GraphBuilder::beginGpuTimingFrame(uint32_t frameIndex)
{
    assert(m_device);
    assert(m_compiled);

    m_activeGpuTimingSlot = -1;
    for (Pass& pass : m_passes)
        pass.gpuTimer = nullptr;

    if (frameIndex % kGpuTimingSampleInterval != 0)
        return;

    const size_t slotIndex = (frameIndex / kGpuTimingSampleInterval) % kGpuTimingSlotCount;
    GpuTimingSlot& slot = m_gpuTimingSlots[slotIndex];
    if (slot.pending)
        return;

    const size_t passCount = m_compiledPassOrder.size();
    slot.entries.resize(passCount);
    bool hasQuery = false;
    for (size_t i = 0; i < passCount; ++i)
    {
        const uint32_t passIndex = m_compiledPassOrder[i];
        if (passIndex >= m_passes.size() || !m_passes[passIndex].active)
            continue;

        Pass& pass = m_passes[passIndex];
        GpuTimingEntry& entry = slot.entries[i];
        entry.name = pass.name;
        if (!entry.query)
            entry.query = m_device->createTimerQuery();
        if (!entry.query)
            continue;

        m_device->resetTimerQuery(entry.query);
        pass.gpuTimer = entry.query;
        hasQuery = true;
    }

    if (!hasQuery)
        return;

    slot.frameIndex = frameIndex;
    m_activeGpuTimingSlot = static_cast<int32_t>(slotIndex);
}

std::optional<GpuTimingFrame> GraphBuilder::collectCompletedGpuTimings()
{
    if (!m_device)
        return std::nullopt;

    for (GpuTimingSlot& slot : m_gpuTimingSlots)
    {
        if (!slot.pending)
            continue;

        bool ready = true;
        for (const GpuTimingEntry& entry : slot.entries)
        {
            if (entry.query && !m_device->pollTimerQuery(entry.query))
            {
                ready = false;
                break;
            }
        }
        if (!ready)
            continue;

        GpuTimingFrame result;
        result.frameIndex = slot.frameIndex;
        result.passes.reserve(slot.entries.size());
        for (const GpuTimingEntry& entry : slot.entries)
        {
            if (!entry.query)
                continue;
            result.passes.push_back({
                entry.name,
                double(m_device->getTimerQueryTime(entry.query)) * 1000.0,
            });
        }
        slot.pending = false;
        return result;
    }

    return std::nullopt;
}

void GraphBuilder::reset()
{
    releaseTransientResources();
    m_textures.clear();
    m_buffers.clear();
    // Stable graphs reuse the storage owned by the same pass index. Do not use a
    // LIFO pool here: exchanging capacities between unrelated passes causes
    // avoidable reallocations when their access-list shapes differ.
    m_recycledPasses.clear();
    m_recycledPasses.swap(m_passes);
    for (Pass& pass : m_recycledPasses)
    {
        pass.setup = {};
        pass.execute = {};
        pass.textureReads.clear();
        pass.textureWrites.clear();
        pass.bufferReads.clear();
        pass.bufferWrites.clear();
    }
    m_compiledPassOrder.clear();
    m_compiledWaves.clear();
    m_passNames.clear();
    m_importIndexByTexture.clear();
    m_importIndexByBuffer.clear();
    m_transientStats = {};
    m_volatileConstants.clear();
    m_compiled = false;
}

caustica::rhi::Texture* GraphBuilder::resolveTexture(TextureHandle handle) const
{
    if (!isValid(handle, m_textures.size()))
        return nullptr;
    return m_textures[handle.index].texture;
}

caustica::rhi::Buffer* GraphBuilder::resolveBuffer(BufferHandle handle) const
{
    if (!isValid(handle, m_buffers.size()))
        return nullptr;
    return m_buffers[handle.index].buffer;
}

caustica::rhi::ResourceStates GraphBuilder::textureState(TextureHandle handle) const
{
    if (!isValid(handle, m_textures.size()))
        return caustica::rhi::ResourceStates::Common;
    return m_textures[handle.index].currentState;
}

caustica::rhi::ResourceStates GraphBuilder::bufferState(BufferHandle handle) const
{
    if (!isValid(handle, m_buffers.size()))
        return caustica::rhi::ResourceStates::Common;
    return m_buffers[handle.index].currentState;
}

} // namespace caustica::rg
