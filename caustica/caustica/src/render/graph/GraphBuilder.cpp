#include <render/graph/GraphBuilder.h>
#include <render/graph/RenderBufferPool.h>
#include <render/graph/RenderTargetPool.h>
#include <render/graph/TransientResourceAllocator.h>
#include <core/task/TaskRuntime.h>

#include <array>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <numeric>
#include <vector>

namespace caustica::rg
{

namespace
{
    bool isValid(TextureHandle handle, size_t textureCount, uint32_t generation)
    {
        return handle.isValid() && handle.generation == generation && handle.index < textureCount;
    }

    bool isValid(BufferHandle handle, size_t bufferCount, uint32_t generation)
    {
        return handle.isValid() && handle.generation == generation && handle.index < bufferCount;
    }

    bool isValid(AccelStructHandle handle, size_t accelCount, uint32_t generation)
    {
        return handle.isValid() && handle.generation == generation && handle.index < accelCount;
    }

}

PassBuilder::PassBuilder(
    GraphBuilder& graph,
    TextureAccessList& textureReads,
    TextureAccessList& textureWrites,
    BufferAccessList& bufferReads,
    BufferAccessList& bufferWrites,
    AccelStructAccessList& accelStructReads,
    AccelStructAccessList& accelStructWrites)
    : m_graph(&graph)
    , m_textureReads(&textureReads)
    , m_textureWrites(&textureWrites)
    , m_bufferReads(&bufferReads)
    , m_bufferWrites(&bufferWrites)
    , m_accelStructReads(&accelStructReads)
    , m_accelStructWrites(&accelStructWrites)
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

void PassBuilder::read(AccelStructHandle accel, AccelStructAccess access)
{
    m_accelStructReads->emplace_back(accel, access);
}

void PassBuilder::write(AccelStructHandle accel, AccelStructAccess access)
{
    m_accelStructWrites->emplace_back(accel, access);
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

caustica::rhi::rt::AccelStruct* RenderPassContext::accelStruct(AccelStructHandle handle) const
{
    assert(m_graph);
    return m_graph->resolveAccelStruct(handle);
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

caustica::rhi::ResourceStates GraphBuilder::accessToState(AccelStructAccess access)
{
    switch (access)
    {
    case AccelStructAccess::Build:
        return caustica::rhi::ResourceStates::AccelStructWrite;
    case AccelStructAccess::ShaderResource:
    default:
        return caustica::rhi::ResourceStates::AccelStructRead;
    }
}

TextureHandle GraphBuilder::makeTextureHandle(uint32_t index) const
{
    return TextureHandle{ index, m_handleGeneration };
}

BufferHandle GraphBuilder::makeBufferHandle(uint32_t index) const
{
    return BufferHandle{ index, m_handleGeneration };
}

AccelStructHandle GraphBuilder::makeAccelStructHandle(uint32_t index) const
{
    return AccelStructHandle{ index, m_handleGeneration };
}

PassHandle GraphBuilder::makePassHandle(uint32_t index) const
{
    return PassHandle{ index, m_handleGeneration };
}

void GraphBuilder::setDevice(caustica::rhi::Device* device)
{
    if (m_device != device)
    {
        m_transientHeaps.clear();
        m_transientHeapPool.clear();
        m_persistentTransients = {};
        m_compiledPlanCache.clear();
        m_compiledPlanCacheOrder.clear();
        m_activeCachedPlan = nullptr;
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

const std::vector<uint32_t>& GraphBuilder::compiledPassOrder() const
{
    return m_activeCachedPlan ? m_activeCachedPlan->passOrder : m_compiledPassOrder;
}

const std::vector<std::vector<uint32_t>>& GraphBuilder::compiledWaves() const
{
    return m_activeCachedPlan ? m_activeCachedPlan->waves : m_compiledWaves;
}

const std::vector<caustica::rhi::CommandQueue>& GraphBuilder::compiledWaveQueues() const
{
    return m_activeCachedPlan ? m_activeCachedPlan->waveQueues : m_compiledWaveQueues;
}

const std::vector<std::vector<uint32_t>>& GraphBuilder::compiledWaveWaits() const
{
    return m_activeCachedPlan ? m_activeCachedPlan->waveWaits : m_compiledWaveWaits;
}

ResourceOwnership GraphBuilder::textureOwnership(TextureHandle handle) const
{
    if (!isValid(handle, m_textures.size(), m_handleGeneration))
        return ResourceOwnership::External;
    return m_textures[handle.index].lifetime == ResourceLifetime::Transient
        ? ResourceOwnership::Graph
        : ResourceOwnership::External;
}

ResourceOwnership GraphBuilder::bufferOwnership(BufferHandle handle) const
{
    if (!isValid(handle, m_buffers.size(), m_handleGeneration))
        return ResourceOwnership::External;
    return m_buffers[handle.index].lifetime == ResourceLifetime::Transient
        ? ResourceOwnership::Graph
        : ResourceOwnership::External;
}

PassHandle GraphBuilder::findPass(const std::string_view name) const
{
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_passes.size()); ++i)
    {
        if (m_passes[i].name == name)
            return makePassHandle(i);
    }
    return {};
}

bool GraphBuilder::isPassRegistered(const std::string_view name) const
{
    return findPass(name).isValid();
}

bool GraphBuilder::isPassActive(const std::string_view name) const
{
    assert(m_compiled);
    const PassHandle handle = findPass(name);
    return handle.isValid() && handle.index < m_passes.size() && m_passes[handle.index].active;
}

TextureHandle GraphBuilder::importTexture(caustica::rhi::Texture* texture, caustica::rhi::ResourceStates initialState)
{
    assert(texture);

    if (const auto existing = m_importIndexByTexture.find(texture); existing != m_importIndexByTexture.end())
        return makeTextureHandle(existing->second);

    const TextureHandle handle = makeTextureHandle(static_cast<uint32_t>(m_textures.size()));
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
        return makeBufferHandle(existing->second);

    const BufferHandle handle = makeBufferHandle(static_cast<uint32_t>(m_buffers.size()));
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

AccelStructHandle GraphBuilder::importAccelStruct(
    caustica::rhi::rt::AccelStruct* accel,
    caustica::rhi::ResourceStates initialState)
{
    assert(accel);

    if (const auto existing = m_importIndexByAccelStruct.find(accel); existing != m_importIndexByAccelStruct.end())
        return makeAccelStructHandle(existing->second);

    const AccelStructHandle handle = makeAccelStructHandle(static_cast<uint32_t>(m_accelStructs.size()));
    GraphAccelStruct imported{};
    imported.accel = accel;
    imported.currentState = initialState;
    imported.lifetime = ResourceLifetime::Imported;
    m_accelStructs.push_back(imported);
    m_importIndexByAccelStruct.emplace(accel, handle.index);
    return handle;
}

AccelStructHandle GraphBuilder::importAccelStruct(
    caustica::rhi::rt::AccelStruct* accel,
    AccelStructAccess initialAccess)
{
    return importAccelStruct(accel, accessToState(initialAccess));
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
    if (!desc.name.empty())
    {
        if (const auto existing = m_createIndexByName.find(desc.name); existing != m_createIndexByName.end())
        {
            assert(existing->second < m_textures.size());
            assert(m_textures[existing->second].lifetime == ResourceLifetime::Transient);
            return makeTextureHandle(existing->second);
        }
    }

    const TextureHandle handle = makeTextureHandle(static_cast<uint32_t>(m_textures.size()));
    GraphTexture resource{};
    resource.currentState = caustica::rhi::ResourceStates::Common;
    resource.lifetime = ResourceLifetime::Transient;
    resource.desc = desc;
    m_textures.push_back(resource);
    if (!desc.name.empty())
        m_createIndexByName.emplace(desc.name, handle.index);
    return handle;
}

TextureHandle GraphBuilder::findTexture(const std::string_view name) const
{
    if (name.empty())
        return {};
    const auto existing = m_createIndexByName.find(std::string(name));
    if (existing == m_createIndexByName.end())
        return {};
    return makeTextureHandle(existing->second);
}

caustica::rhi::TextureHandle GraphBuilder::ownedTextureHandle(TextureHandle handle) const
{
    if (!isValid(handle, m_textures.size(), m_handleGeneration))
        return {};
    return m_textures[handle.index].owned;
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
    const BufferHandle handle = makeBufferHandle(static_cast<uint32_t>(m_buffers.size()));
    GraphBuffer resource{};
    resource.currentState = caustica::rhi::ResourceStates::Common;
    resource.lifetime = ResourceLifetime::Transient;
    resource.desc = desc;
    m_buffers.push_back(resource);
    return handle;
}

void GraphBuilder::extractTexture(TextureHandle handle, caustica::rhi::ResourceStates finalState)
{
    assert(isValid(handle, m_textures.size(), m_handleGeneration) && "RenderGraph extract references invalid texture handle");
    if (!isValid(handle, m_textures.size(), m_handleGeneration))
        return;
    m_textures[handle.index].finalState = finalState;
}

void GraphBuilder::extractTexture(TextureHandle handle, TextureAccess finalAccess)
{
    extractTexture(handle, accessToState(finalAccess));
}

void GraphBuilder::extractBuffer(BufferHandle handle, caustica::rhi::ResourceStates finalState)
{
    assert(isValid(handle, m_buffers.size(), m_handleGeneration) && "RenderGraph extract references invalid buffer handle");
    if (!isValid(handle, m_buffers.size(), m_handleGeneration))
        return;
    m_buffers[handle.index].finalState = finalState;
}

void GraphBuilder::extractBuffer(BufferHandle handle, BufferAccess finalAccess)
{
    extractBuffer(handle, accessToState(finalAccess));
}

PassHandle GraphBuilder::addPass(std::string_view name, SetupFn setup, ExecuteFn execute, PassOptions options)
{
    assert(!isPassRegistered(name) && "RenderGraph pass names must be unique");

    Pass pass;
    const size_t passIndex = m_passes.size();
    if (passIndex < m_recycledPasses.size())
        pass = std::move(m_recycledPasses[passIndex]);
    pass.textureReads.clear();
    pass.textureWrites.clear();
    pass.bufferReads.clear();
    pass.bufferWrites.clear();
    pass.accelStructReads.clear();
    pass.accelStructWrites.clear();
    if (pass.name != name)
        pass.measuredRecordingCost = 0.0;
    pass.name.assign(name);
    pass.execute = std::move(execute);
    pass.options = options;
    pass.gpuTimer = nullptr;

    // Setup is registration-only. Never retain its captures beyond addPass():
    // callers commonly declare frame-local handles immediately before this call.
    if (pass.options.enabled && setup)
    {
        PassBuilder builder(
            *this,
            pass.textureReads,
            pass.textureWrites,
            pass.bufferReads,
            pass.bufferWrites,
            pass.accelStructReads,
            pass.accelStructWrites);
        setup(builder);
    }

    m_passNames.push_back(pass.name);
    m_passes.push_back(std::move(pass));
    m_compiled = false;
    return makePassHandle(static_cast<uint32_t>(passIndex));
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
    mixValue(m_accelStructs.size());
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
        mixValue(pass.options.after.index);
        mixValue(pass.options.queue);
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
        mixAccesses(pass.accelStructReads);
        mixAccesses(pass.accelStructWrites);
    }
    return hash;
}

void GraphBuilder::evictOldestCompiledPlanIfNeeded()
{
    while (m_compiledPlanCache.size() >= kCompiledPlanCacheLimit && !m_compiledPlanCacheOrder.empty())
    {
        const uint64_t oldest = m_compiledPlanCacheOrder.front();
        m_compiledPlanCacheOrder.erase(m_compiledPlanCacheOrder.begin());
        if (m_activeCachedPlan != nullptr)
        {
            const auto active = m_compiledPlanCache.find(oldest);
            if (active != m_compiledPlanCache.end() && m_activeCachedPlan == &active->second)
                m_activeCachedPlan = nullptr;
        }
        m_compiledPlanCache.erase(oldest);
    }
}

caustica::rhi::CommandQueue GraphBuilder::passQueue(uint32_t passIndex) const
{
    if (passIndex >= m_passes.size())
        return caustica::rhi::CommandQueue::Graphics;
    const Pass& pass = m_passes[passIndex];
    if (pass.options.serialOnPrimary)
        return caustica::rhi::CommandQueue::Graphics;
    return pass.options.queue;
}

caustica::rhi::CommandQueue GraphBuilder::resolveQueue(caustica::rhi::CommandQueue queue) const
{
    if (queue == caustica::rhi::CommandQueue::Graphics || !m_device)
        return caustica::rhi::CommandQueue::Graphics;
    if (queue == caustica::rhi::CommandQueue::Compute
        && !m_device->queryFeatureSupport(caustica::rhi::Feature::ComputeQueue))
        return caustica::rhi::CommandQueue::Graphics;
    if (queue == caustica::rhi::CommandQueue::Copy
        && !m_device->queryFeatureSupport(caustica::rhi::Feature::CopyQueue))
        return caustica::rhi::CommandQueue::Graphics;
    return queue;
}

void GraphBuilder::compile()
{
    m_activeCachedPlan = nullptr;
    for (Pass& pass : m_passes)
        pass.active = false;
    m_compiledPassOrder.clear();
    m_compiledWaves.clear();
    m_compiledWaveQueues.clear();
    m_compiledWaveWaits.clear();
    m_lastCompileCacheHit = false;
    m_lastCompileHadCycle = false;

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
        m_activeCachedPlan = &plan;

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
    std::vector<int32_t> lastAccelWriter(m_accelStructs.size(), -1);
    std::vector<std::vector<uint32_t>> lastTextureReaders(m_textures.size());
    std::vector<std::vector<uint32_t>> lastBufferReaders(m_buffers.size());
    std::vector<std::vector<uint32_t>> lastAccelReaders(m_accelStructs.size());

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
            assert(isValid(handle, m_textures.size(), m_handleGeneration) && "RenderGraph pass read references invalid texture handle");
            if (!isValid(handle, m_textures.size(), m_handleGeneration))
                continue;

            if (lastTextureWriter[handle.index] >= 0)
                addDependency(static_cast<uint32_t>(lastTextureWriter[handle.index]), passIndex);
            lastTextureReaders[handle.index].push_back(passIndex);
        }
        for (const auto& [handle, access] : pass.textureWrites)
        {
            (void)access;
            assert(isValid(handle, m_textures.size(), m_handleGeneration) && "RenderGraph pass write references invalid texture handle");
            if (!isValid(handle, m_textures.size(), m_handleGeneration))
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
            assert(isValid(handle, m_buffers.size(), m_handleGeneration) && "RenderGraph pass read references invalid buffer handle");
            if (!isValid(handle, m_buffers.size(), m_handleGeneration))
                continue;

            if (lastBufferWriter[handle.index] >= 0)
                addDependency(static_cast<uint32_t>(lastBufferWriter[handle.index]), passIndex);
            lastBufferReaders[handle.index].push_back(passIndex);
        }
        for (const auto& [handle, access] : pass.bufferWrites)
        {
            (void)access;
            assert(isValid(handle, m_buffers.size(), m_handleGeneration) && "RenderGraph pass write references invalid buffer handle");
            if (!isValid(handle, m_buffers.size(), m_handleGeneration))
                continue;

            for (const uint32_t reader : lastBufferReaders[handle.index])
                addDependency(reader, passIndex);
            lastBufferReaders[handle.index].clear();

            if (lastBufferWriter[handle.index] >= 0)
                addDependency(static_cast<uint32_t>(lastBufferWriter[handle.index]), passIndex);
            lastBufferWriter[handle.index] = static_cast<int32_t>(passIndex);
        }
        for (const auto& [handle, access] : pass.accelStructReads)
        {
            (void)access;
            assert(isValid(handle, m_accelStructs.size(), m_handleGeneration)
                && "RenderGraph pass read references invalid accel struct handle");
            if (!isValid(handle, m_accelStructs.size(), m_handleGeneration))
                continue;

            if (lastAccelWriter[handle.index] >= 0)
                addDependency(static_cast<uint32_t>(lastAccelWriter[handle.index]), passIndex);
            lastAccelReaders[handle.index].push_back(passIndex);
        }
        for (const auto& [handle, access] : pass.accelStructWrites)
        {
            (void)access;
            assert(isValid(handle, m_accelStructs.size(), m_handleGeneration)
                && "RenderGraph pass write references invalid accel struct handle");
            if (!isValid(handle, m_accelStructs.size(), m_handleGeneration))
                continue;

            for (const uint32_t reader : lastAccelReaders[handle.index])
                addDependency(reader, passIndex);
            lastAccelReaders[handle.index].clear();

            if (lastAccelWriter[handle.index] >= 0)
                addDependency(static_cast<uint32_t>(lastAccelWriter[handle.index]), passIndex);
            lastAccelWriter[handle.index] = static_cast<int32_t>(passIndex);
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
        if (!pass.options.enabled || !isHandleCurrent(pass.options.after))
            continue;

        assert(pass.options.after.index < m_passes.size()
            && "RenderGraph PassOptions::after references an invalid pass");
        if (pass.options.after.index >= m_passes.size())
            continue;
        addDependency(pass.options.after.index, passIndex);
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
            if (isValid(handle, m_textures.size(), m_handleGeneration))
                referenced[handle.index] = true;
        }

        for (const auto& [handle, access] : pass.textureWrites)
        {
            (void)access;
            if (isValid(handle, m_textures.size(), m_handleGeneration))
                referenced[handle.index] = true;
        }

        for (const auto& [handle, access] : pass.bufferReads)
        {
            (void)access;
            if (isValid(handle, m_buffers.size(), m_handleGeneration))
                referencedBuffers[handle.index] = true;
        }

        for (const auto& [handle, access] : pass.bufferWrites)
        {
            (void)access;
            if (isValid(handle, m_buffers.size(), m_handleGeneration))
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
        // Flattening to registration order can emit the wrong GPU sequence.
        m_lastCompileHadCycle = true;
        m_compiledPassOrder.clear();
        m_compiledWaves.clear();
        m_compiledWaveQueues.clear();
        m_compiledWaveWaits.clear();
        for (Pass& pass : m_passes)
            pass.active = false;
        m_compiled = false;
        return;
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
    plan.waveQueues = m_compiledWaveQueues;
    plan.waveWaits = m_compiledWaveWaits;
    plan.referencedTextures = referenced;
    plan.referencedBuffers = referencedBuffers;
    plan.textureLifetimes = textureLifetimes;
    plan.bufferLifetimes = bufferLifetimes;
    if (m_compiledPlanCache.find(planKey) == m_compiledPlanCache.end())
    {
        evictOldestCompiledPlanIfNeeded();
        m_compiledPlanCacheOrder.push_back(planKey);
    }
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
            if (!isValid(handle, m_textures.size(), m_handleGeneration))
                return;
            if (m_textures[handle.index].lifetime != ResourceLifetime::Transient)
                return;

            TransientLifetime& lifetime = textureLifetimes[handle.index];
            lifetime.firstPassOrder = std::min(lifetime.firstPassOrder, order);
            lifetime.lastPassOrder = std::max(lifetime.lastPassOrder, order);
        };

        const auto touchBuffer = [&](BufferHandle handle) {
            if (!isValid(handle, m_buffers.size(), m_handleGeneration))
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

    const auto hasReferencedTransient = [](const auto& resources, const std::vector<bool>& referenced) {
        for (size_t i = 0; i < resources.size() && i < referenced.size(); ++i)
        {
            if (referenced[i] && resources[i].lifetime == ResourceLifetime::Transient)
                return true;
        }
        return false;
    };
    if (!hasReferencedTransient(m_textures, referencedTextures)
        && !hasReferencedTransient(m_buffers, referencedBuffers))
    {
        return;
    }

    if (!m_device)
        return;

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
    if (!isValid(handle, m_textures.size(), m_handleGeneration))
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
    if (!isValid(handle, m_buffers.size(), m_handleGeneration))
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
    if (!isValid(handle, m_textures.size(), m_handleGeneration))
        return;

    for (TextureAliasingBarrier& barrier : m_textureAliasingBarriers)
    {
        if (barrier.emitted || barrier.after.index != handle.index)
            continue;

        caustica::rhi::Texture* before = isValid(barrier.before, m_textures.size(), m_handleGeneration)
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
    if (!isValid(handle, m_buffers.size(), m_handleGeneration))
        return;

    for (BufferAliasingBarrier& barrier : m_bufferAliasingBarriers)
    {
        if (barrier.emitted || barrier.after.index != handle.index)
            continue;

        caustica::rhi::Buffer* before = isValid(barrier.before, m_buffers.size(), m_handleGeneration)
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
        if (isValid(handle, m_textures.size(), m_handleGeneration))
            m_textures[handle.index].currentState = accessToState(access);
    }

    for (const auto& [handle, access] : pass.textureReads)
    {
        if (!isValid(handle, m_textures.size(), m_handleGeneration) || passUsesTextureAsWrite(pass, handle))
            continue;
        m_textures[handle.index].currentState = accessToState(access);
    }

    for (const auto& [handle, access] : pass.bufferWrites)
    {
        if (isValid(handle, m_buffers.size(), m_handleGeneration))
            m_buffers[handle.index].currentState = accessToState(access);
    }

    for (const auto& [handle, access] : pass.bufferReads)
    {
        if (!isValid(handle, m_buffers.size(), m_handleGeneration) || passUsesBufferAsWrite(pass, handle))
            continue;
        m_buffers[handle.index].currentState = accessToState(access);
    }

    for (const auto& [handle, access] : pass.accelStructWrites)
    {
        if (isValid(handle, m_accelStructs.size(), m_handleGeneration))
            m_accelStructs[handle.index].currentState = accessToState(access);
    }

    for (const auto& [handle, access] : pass.accelStructReads)
    {
        if (!isValid(handle, m_accelStructs.size(), m_handleGeneration) || passUsesAccelStructAsWrite(pass, handle))
            continue;
        m_accelStructs[handle.index].currentState = accessToState(access);
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

bool GraphBuilder::passUsesAccelStructAsWrite(const Pass& pass, AccelStructHandle handle)
{
    for (const auto& [writeHandle, access] : pass.accelStructWrites)
    {
        if (writeHandle.index == handle.index)
            return true;
        (void)access;
    }
    return false;
}

void GraphBuilder::transitionAccelStruct(
    caustica::rhi::CommandList* commandList,
    AccelStructHandle handle,
    AccelStructAccess access)
{
    if (!isValid(handle, m_accelStructs.size(), m_handleGeneration))
        return;
    GraphAccelStruct& resource = m_accelStructs[handle.index];
    if (!resource.accel)
        return;
    const caustica::rhi::ResourceStates target = accessToState(access);
    if (resource.currentState == target)
        return;
    commandList->setAccelStructState(resource.accel, target);
    resource.currentState = target;
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
        transitionTexture(commandList, makeTextureHandle(static_cast<uint32_t>(i)), *resource.finalState);
        hasTransitions = hasTransitions || before != resource.currentState;
    }

    for (size_t i = 0; i < m_buffers.size(); ++i)
    {
        GraphBuffer& resource = m_buffers[i];
        if (!resource.finalState.has_value())
            continue;

        const caustica::rhi::ResourceStates before = resource.currentState;
        transitionBuffer(commandList, makeBufferHandle(static_cast<uint32_t>(i)), *resource.finalState);
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
    m_compiledWaveQueues.clear();
    m_compiledWaveWaits.clear();

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
    std::vector<uint32_t> passWave(m_passes.size(), UINT32_MAX);

    const auto emitWave = [&](caustica::rhi::CommandQueue queue, const std::vector<uint32_t>& wave) {
        if (wave.empty())
            return;

        std::vector<uint32_t> waits;
        const uint32_t waveIndex = static_cast<uint32_t>(m_compiledWaves.size());
        for (const uint32_t passIndex : wave)
        {
            for (const uint32_t dependency : incoming[passIndex])
            {
                if (!needed[dependency] || passWave[dependency] == UINT32_MAX)
                    continue;
                if (passQueue(dependency) == queue)
                    continue;
                if (std::find(waits.begin(), waits.end(), passWave[dependency]) == waits.end())
                    waits.push_back(passWave[dependency]);
            }
            passWave[passIndex] = waveIndex;
            emitted[passIndex] = true;
            m_compiledPassOrder.push_back(passIndex);
            for (const uint32_t dependent : outgoing[passIndex])
            {
                if (needed[dependent] && indegree[dependent] > 0)
                    --indegree[dependent];
            }
        }

        m_compiledWaves.push_back(wave);
        m_compiledWaveQueues.push_back(queue);
        m_compiledWaveWaits.push_back(std::move(waits));
    };

    for (;;)
    {
        std::array<std::vector<uint32_t>, size_t(caustica::rhi::CommandQueue::Count)> byQueue{};
        std::vector<uint32_t> serialSolo;
        for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(m_passes.size()); ++passIndex)
        {
            if (!needed[passIndex] || emitted[passIndex] || indegree[passIndex] != 0)
                continue;
            if (m_passes[passIndex].options.serialOnPrimary)
                serialSolo.push_back(passIndex);
            else
                byQueue[size_t(passQueue(passIndex))].push_back(passIndex);
        }

        const bool anyQueue =
            !byQueue[size_t(caustica::rhi::CommandQueue::Graphics)].empty()
            || !byQueue[size_t(caustica::rhi::CommandQueue::Compute)].empty()
            || !byQueue[size_t(caustica::rhi::CommandQueue::Copy)].empty();
        if (serialSolo.empty() && !anyQueue)
            break;

        // Submit async queues first so they can overlap later graphics recording.
        emitWave(caustica::rhi::CommandQueue::Copy, byQueue[size_t(caustica::rhi::CommandQueue::Copy)]);
        emitWave(caustica::rhi::CommandQueue::Compute, byQueue[size_t(caustica::rhi::CommandQueue::Compute)]);
        if (!serialSolo.empty())
            emitWave(caustica::rhi::CommandQueue::Graphics, { serialSolo.front() });
        emitWave(
            caustica::rhi::CommandQueue::Graphics,
            byQueue[size_t(caustica::rhi::CommandQueue::Graphics)]);
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
        if (!isValid(handle, m_textures.size(), m_handleGeneration))
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
        if (!isValid(handle, m_buffers.size(), m_handleGeneration))
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
    for (const auto& [handle, access] : pass.accelStructReads)
        transitionAccelStruct(commandList, handle, access);
    for (const auto& [handle, access] : pass.accelStructWrites)
        transitionAccelStruct(commandList, handle, access);

    if (!pass.textureReads.empty() || !pass.textureWrites.empty()
        || !pass.bufferReads.empty() || !pass.bufferWrites.empty()
        || !pass.accelStructReads.empty() || !pass.accelStructWrites.empty())
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
    std::vector<uint32_t>& activePasses = m_activePassScratch;
    activePasses.clear();
    activePasses.reserve(wave.size());
    const auto estimatedCost = [&](uint32_t passIndex) {
        const Pass& pass = m_passes[passIndex];
        return std::max<double>(
            std::max<uint32_t>(1, pass.options.recordingCost),
            pass.measuredRecordingCost > 0.0 ? pass.measuredRecordingCost : 1.0);
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

    if (m_recordingBatchScratch.size() < jobCount)
        m_recordingBatchScratch.resize(jobCount);
    for (RecordingBatch& batch : m_recordingBatchScratch)
    {
        batch.passes.clear();
        batch.cost = 0.0;
        batch.commandList = nullptr;
    }
    auto batchBegin = m_recordingBatchScratch.begin();
    auto batchEnd = batchBegin + jobCount;
    std::stable_sort(activePasses.begin(), activePasses.end(), [&](uint32_t a, uint32_t b) {
        return estimatedCost(a) > estimatedCost(b);
    });
    for (const uint32_t passIndex : activePasses)
    {
        auto target = std::min_element(
            batchBegin, batchEnd,
            [](const RecordingBatch& a, const RecordingBatch& b) { return a.cost < b.cost; });
        target->passes.push_back(passIndex);
        target->cost += estimatedCost(passIndex);
    }

    // Prior serial waves may still be pending on the open primary. Flush them
    // before submitting forks so GPU order matches the compiled wave order.
    // WARNING: flush closes the primary and clears volatile CB address maps on
    // that list — later primary passes must writeBuffer those CBs again.
    if (frameCtx.primaryOpen())
    {
        m_lastQueueInstance[size_t(caustica::rhi::CommandQueue::Graphics)] = frameCtx.flushPrimary();
        m_queueSubmitted[size_t(caustica::rhi::CommandQueue::Graphics)] = 1;
    }

    if (m_parallelTextureStateScratch.size() < jobCount)
        m_parallelTextureStateScratch.resize(jobCount);
    if (m_parallelBufferStateScratch.size() < jobCount)
        m_parallelBufferStateScratch.resize(jobCount);
    for (uint32_t slot = 0; slot < jobCount; ++slot)
    {
        auto& textureStates = m_parallelTextureStateScratch[slot];
        textureStates.resize(m_textures.size());
        for (size_t i = 0; i < m_textures.size(); ++i)
            textureStates[i] = m_textures[i].currentState;

        auto& bufferStates = m_parallelBufferStateScratch[slot];
        bufferStates.resize(m_buffers.size());
        for (size_t i = 0; i < m_buffers.size(); ++i)
            bufferStates[i] = m_buffers[i].currentState;
    }

    for (auto batchIt = batchBegin; batchIt != batchEnd; ++batchIt)
    {
        RecordingBatch& batch = *batchIt;
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
    caustica::task::parallelFor(
        jobs,
        jobCount,
        caustica::task::Priority::High,
        caustica::task::Affinity::Any,
        [this](uint32_t slot) {
            auto& localTex = m_parallelTextureStateScratch[slot];
            auto& localBuf = m_parallelBufferStateScratch[slot];
            RecordingBatch& batch = m_recordingBatchScratch[slot];
            for (const uint32_t passIndex : batch.passes)
            {
                const auto begin = std::chrono::steady_clock::now();
                Pass& pass = m_passes[passIndex];
                recordPass(batch.commandList.Get(), pass, &localTex, &localBuf);
                const double microseconds = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - begin).count();
                // One cost unit is approximately 100 us of command recording.
                const double measured = std::max(1.0, microseconds / 100.0);
                pass.measuredRecordingCost = pass.measuredRecordingCost > 0.0
                    ? pass.measuredRecordingCost * 0.8 + measured * 0.2
                    : measured;
            }
        },
        1);
    caustica::task::wait(jobs);

    m_lastQueueInstance[size_t(caustica::rhi::CommandQueue::Graphics)] = frameCtx.submitForks();
    m_queueSubmitted[size_t(caustica::rhi::CommandQueue::Graphics)] = 1;
    for (auto batchIt = batchBegin; batchIt != batchEnd; ++batchIt)
        batchIt->commandList = nullptr;

    for (const uint32_t passIndex : wave)
    {
        if (passIndex >= m_passes.size())
            continue;
        const Pass& pass = m_passes[passIndex];
        if (pass.active)
            syncPassEndStates(pass);
    }
    return jobCount;
}

void GraphBuilder::applyWaveWaits(
    caustica::rhi::FrameCommandContext& frameCtx,
    caustica::rhi::CommandQueue consumer,
    const std::vector<uint32_t>& waitWaves)
{
    if (!m_device || waitWaves.empty())
        return;

    const auto& queues = compiledWaveQueues();
    for (const uint32_t waitWave : waitWaves)
    {
        if (waitWave >= queues.size())
            continue;
        const caustica::rhi::CommandQueue producer = resolveQueue(queues[waitWave]);
        if (producer == consumer)
            continue;

        if (!m_queueSubmitted[size_t(producer)]
            && producer == caustica::rhi::CommandQueue::Graphics
            && frameCtx.primaryOpen())
        {
            m_lastQueueInstance[size_t(producer)] = frameCtx.flushPrimary();
            m_queueSubmitted[size_t(producer)] = 1;
        }

        if (!m_queueSubmitted[size_t(producer)])
            continue;

        if (consumer == caustica::rhi::CommandQueue::Graphics && frameCtx.primaryOpen())
        {
            m_lastQueueInstance[size_t(consumer)] = frameCtx.flushPrimary();
            m_queueSubmitted[size_t(consumer)] = 1;
        }

        m_device->queueWaitForCommandList(
            consumer,
            producer,
            m_lastQueueInstance[size_t(producer)]);
    }
}

uint64_t GraphBuilder::executeWaveAsync(
    caustica::rhi::FrameCommandContext& frameCtx,
    caustica::rhi::CommandQueue queue,
    const std::vector<uint32_t>& wave)
{
    caustica::rhi::CommandListHandle list = frameCtx.pool().acquire(queue);
    if (!list || !list->open())
    {
        if (list)
            frameCtx.pool().release(std::move(list));
        executeWaveSerial(frameCtx.primary(), wave);
        return 0;
    }

    executeWaveSerial(list.Get(), wave);
    list->close();
    const uint64_t instance = m_device
        ? m_device->executeCommandList(list, queue)
        : 0;
    frameCtx.pool().release(std::move(list));
    return instance;
}

void GraphBuilder::execute(caustica::rhi::FrameCommandContext& frameCtx, ExecuteParams params)
{
    caustica::rhi::CommandList* primary = frameCtx.primary();
    assert(primary);
    assert(frameCtx.primaryOpen());
    if (!m_compiled)
        compile();
    m_lastParallelBatchCount = 0;
    m_lastQueueInstance = {};
    m_queueSubmitted = {};

    const auto& waves = compiledWaves();
    const auto& waveQueues = compiledWaveQueues();
    const auto& waveWaits = compiledWaveWaits();

    for (size_t waveIndex = 0; waveIndex < waves.size(); ++waveIndex)
    {
        const std::vector<uint32_t>& wave = waves[waveIndex];
        if (wave.empty())
            continue;

        const caustica::rhi::CommandQueue requested =
            waveIndex < waveQueues.size() ? waveQueues[waveIndex] : caustica::rhi::CommandQueue::Graphics;
        const caustica::rhi::CommandQueue queue = resolveQueue(requested);
        if (waveIndex < waveWaits.size())
            applyWaveWaits(frameCtx, queue, waveWaits[waveIndex]);

        if (queue != caustica::rhi::CommandQueue::Graphics)
        {
            const uint64_t instance = executeWaveAsync(frameCtx, queue, wave);
            if (instance != 0)
            {
                m_lastQueueInstance[size_t(queue)] = instance;
                m_queueSubmitted[size_t(queue)] = 1;
            }
            continue;
        }

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
            executeWaveSerial(frameCtx.primary(), wave);
        else
            m_lastParallelBatchCount += executeWaveParallel(frameCtx, wave, params);
    }

    if (m_device)
    {
        for (uint8_t queue = 1; queue < uint8_t(caustica::rhi::CommandQueue::Count); ++queue)
        {
            if (!m_queueSubmitted[queue])
                continue;
            if (frameCtx.primaryOpen())
            {
                m_lastQueueInstance[size_t(caustica::rhi::CommandQueue::Graphics)] = frameCtx.flushPrimary();
                m_queueSubmitted[size_t(caustica::rhi::CommandQueue::Graphics)] = 1;
            }
            m_device->queueWaitForCommandList(
                caustica::rhi::CommandQueue::Graphics,
                caustica::rhi::CommandQueue(queue),
                m_lastQueueInstance[queue]);
        }
    }

    transitionExtractedResources(frameCtx.primary());

    if (m_activeGpuTimingSlot >= 0)
    {
        m_gpuTimingSlots[size_t(m_activeGpuTimingSlot)].pending = true;
        m_activeGpuTimingSlot = -1;
    }

    // Recording is synchronous from the caller's perspective: worker jobs have
    // joined and command lists own the recorded data. Drop frame snapshots now
    // so GraphBuilder never carries CPU frame pointers into the next frame.
    for (Pass& pass : m_passes)
        pass.execute = {};
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

    const std::vector<uint32_t>& passOrder = compiledPassOrder();
    const size_t passCount = passOrder.size();
    slot.entries.resize(passCount);
    bool hasQuery = false;
    for (size_t i = 0; i < passCount; ++i)
    {
        const uint32_t passIndex = passOrder[i];
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
    m_activeCachedPlan = nullptr;
    releaseTransientResources();
    m_textures.clear();
    m_buffers.clear();
    m_accelStructs.clear();
    ++m_handleGeneration;
    if (m_handleGeneration == 0)
        m_handleGeneration = 1;
    // Stable graphs reuse the storage owned by the same pass index. Do not use a
    // LIFO pool here: exchanging capacities between unrelated passes causes
    // avoidable reallocations when their access-list shapes differ.
    m_recycledPasses.clear();
    m_recycledPasses.swap(m_passes);
    for (Pass& pass : m_recycledPasses)
    {
        pass.execute = {};
        pass.textureReads.clear();
        pass.textureWrites.clear();
        pass.bufferReads.clear();
        pass.bufferWrites.clear();
        pass.accelStructReads.clear();
        pass.accelStructWrites.clear();
    }
    m_compiledPassOrder.clear();
    m_compiledWaves.clear();
    m_compiledWaveQueues.clear();
    m_compiledWaveWaits.clear();
    m_passNames.clear();
    m_importIndexByTexture.clear();
    m_importIndexByBuffer.clear();
    m_importIndexByAccelStruct.clear();
    m_createIndexByName.clear();
    m_transientStats = {};
    m_volatileConstants.clear();
    m_lastCompileHadCycle = false;
    m_lastQueueInstance = {};
    m_queueSubmitted = {};
    m_compiled = false;
}

caustica::rhi::Texture* GraphBuilder::resolveTexture(TextureHandle handle) const
{
    assert((!handle.isValid() || isHandleCurrent(handle)) && "RenderGraph texture handle is stale after reset()");
    if (!isValid(handle, m_textures.size(), m_handleGeneration))
        return nullptr;
    return m_textures[handle.index].texture;
}

caustica::rhi::Buffer* GraphBuilder::resolveBuffer(BufferHandle handle) const
{
    assert((!handle.isValid() || isHandleCurrent(handle)) && "RenderGraph buffer handle is stale after reset()");
    if (!isValid(handle, m_buffers.size(), m_handleGeneration))
        return nullptr;
    return m_buffers[handle.index].buffer;
}

caustica::rhi::rt::AccelStruct* GraphBuilder::resolveAccelStruct(AccelStructHandle handle) const
{
    assert((!handle.isValid() || isHandleCurrent(handle)) && "RenderGraph accel-struct handle is stale after reset()");
    if (!isValid(handle, m_accelStructs.size(), m_handleGeneration))
        return nullptr;
    return m_accelStructs[handle.index].accel;
}

caustica::rhi::ResourceStates GraphBuilder::textureState(TextureHandle handle) const
{
    if (!isValid(handle, m_textures.size(), m_handleGeneration))
        return caustica::rhi::ResourceStates::Common;
    return m_textures[handle.index].currentState;
}

caustica::rhi::ResourceStates GraphBuilder::bufferState(BufferHandle handle) const
{
    if (!isValid(handle, m_buffers.size(), m_handleGeneration))
        return caustica::rhi::ResourceStates::Common;
    return m_buffers[handle.index].currentState;
}

caustica::rhi::ResourceStates GraphBuilder::accelStructState(AccelStructHandle handle) const
{
    if (!isValid(handle, m_accelStructs.size(), m_handleGeneration))
        return caustica::rhi::ResourceStates::AccelStructRead;
    return m_accelStructs[handle.index].currentState;
}

bool GraphBuilder::isHandleCurrent(TextureHandle handle) const
{
    return isValid(handle, m_textures.size(), m_handleGeneration);
}

bool GraphBuilder::isHandleCurrent(BufferHandle handle) const
{
    return isValid(handle, m_buffers.size(), m_handleGeneration);
}

bool GraphBuilder::isHandleCurrent(AccelStructHandle handle) const
{
    return isValid(handle, m_accelStructs.size(), m_handleGeneration);
}

bool GraphBuilder::isHandleCurrent(PassHandle handle) const
{
    return handle.isValid() && handle.generation == m_handleGeneration && handle.index < m_passes.size();
}

} // namespace caustica::rg
