#include <render/graph/GraphBuilder.h>

#include <cassert>
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

PassBuilder::PassBuilder(GraphBuilder& graph)
    : m_graph(&graph)
{
}

void PassBuilder::read(TextureHandle texture, TextureAccess access)
{
    m_textureReads.emplace_back(texture, access);
}

void PassBuilder::write(TextureHandle texture, TextureAccess access)
{
    m_textureWrites.emplace_back(texture, access);
}

void PassBuilder::read(BufferHandle buffer, BufferAccess access)
{
    m_bufferReads.emplace_back(buffer, access);
}

void PassBuilder::write(BufferHandle buffer, BufferAccess access)
{
    m_bufferWrites.emplace_back(buffer, access);
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

RenderPassContext::RenderPassContext(nvrhi::ICommandList* commandList, const GraphBuilder& graph)
    : m_commandList(commandList)
    , m_graph(&graph)
{
}

nvrhi::ITexture* RenderPassContext::texture(TextureHandle handle) const
{
    assert(m_graph);
    return m_graph->resolveTexture(handle);
}

nvrhi::IBuffer* RenderPassContext::buffer(BufferHandle handle) const
{
    assert(m_graph);
    return m_graph->resolveBuffer(handle);
}

nvrhi::ResourceStates GraphBuilder::accessToState(TextureAccess access)
{
    switch (access)
    {
    case TextureAccess::ShaderResource:
        return nvrhi::ResourceStates::ShaderResource;
    case TextureAccess::RenderTarget:
        return nvrhi::ResourceStates::RenderTarget;
    case TextureAccess::UnorderedAccess:
        return nvrhi::ResourceStates::UnorderedAccess;
    case TextureAccess::CopySource:
        return nvrhi::ResourceStates::CopySource;
    case TextureAccess::CopyDest:
        return nvrhi::ResourceStates::CopyDest;
    default:
        return nvrhi::ResourceStates::Common;
    }
}

nvrhi::ResourceStates GraphBuilder::accessToState(BufferAccess access)
{
    switch (access)
    {
    case BufferAccess::ShaderResource:
        return nvrhi::ResourceStates::ShaderResource;
    case BufferAccess::UnorderedAccess:
        return nvrhi::ResourceStates::UnorderedAccess;
    case BufferAccess::ConstantBuffer:
        return nvrhi::ResourceStates::ConstantBuffer;
    case BufferAccess::CopySource:
        return nvrhi::ResourceStates::CopySource;
    case BufferAccess::CopyDest:
        return nvrhi::ResourceStates::CopyDest;
    case BufferAccess::VertexBuffer:
        return nvrhi::ResourceStates::VertexBuffer;
    case BufferAccess::IndexBuffer:
        return nvrhi::ResourceStates::IndexBuffer;
    case BufferAccess::IndirectArgument:
        return nvrhi::ResourceStates::IndirectArgument;
    case BufferAccess::AccelStructBuildInput:
        return nvrhi::ResourceStates::AccelStructBuildInput;
    default:
        return nvrhi::ResourceStates::Common;
    }
}

void GraphBuilder::setDevice(nvrhi::IDevice* device)
{
    m_device = device;
}

TextureHandle GraphBuilder::importTexture(nvrhi::ITexture* texture, nvrhi::ResourceStates initialState)
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

TextureHandle GraphBuilder::importTexture(nvrhi::ITexture* texture, TextureAccess initialAccess)
{
    return importTexture(texture, accessToState(initialAccess));
}

BufferHandle GraphBuilder::importBuffer(nvrhi::IBuffer* buffer, nvrhi::ResourceStates initialState)
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

BufferHandle GraphBuilder::importBuffer(nvrhi::IBuffer* buffer, BufferAccess initialAccess)
{
    return importBuffer(buffer, accessToState(initialAccess));
}

TextureHandle GraphBuilder::createTexture(const TextureDesc& desc)
{
    assert(m_device);

    const FormatInfo formatInfo = getFormatInfo(desc.format);
    nvrhi::TextureDesc nativeDesc;
    nativeDesc.debugName = desc.name.empty() ? "rg_transient" : desc.name.c_str();
    nativeDesc.width = desc.width;
    nativeDesc.height = desc.height;
    nativeDesc.depth = desc.depth;
    nativeDesc.mipLevels = desc.mipLevels;
    nativeDesc.arraySize = desc.arraySize;
    nativeDesc.format = nvrhi::caustica::toNvrhiFormat(desc.format);
    nativeDesc.isRenderTarget = desc.isRenderTarget || formatInfo.isRenderTargetCompatible;
    nativeDesc.isUAV = desc.isUAV || formatInfo.isUAVCompatible;
    nativeDesc.isTypeless = desc.isTypeless;
    nativeDesc.initialState = nvrhi::ResourceStates::Common;
    nativeDesc.keepInitialState = true;

    nvrhi::TextureHandle owned = m_device->createTexture(nativeDesc);
    assert(owned);

    const TextureHandle handle{ static_cast<uint32_t>(m_textures.size()) };
    GraphTexture resource{};
    resource.texture = owned;
    resource.currentState = nvrhi::ResourceStates::Common;
    resource.lifetime = ResourceLifetime::Transient;
    resource.owned = owned;
    m_textures.push_back(resource);
    return handle;
}

BufferHandle GraphBuilder::createBuffer(const BufferDesc& desc)
{
    assert(m_device);

    nvrhi::BufferDesc nativeDesc;
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
    nativeDesc.format = nvrhi::caustica::toNvrhiFormat(desc.format);
    nativeDesc.initialState = nvrhi::ResourceStates::Common;
    nativeDesc.keepInitialState = true;

    nvrhi::BufferHandle owned = m_device->createBuffer(nativeDesc);
    assert(owned);

    const BufferHandle handle{ static_cast<uint32_t>(m_buffers.size()) };
    GraphBuffer resource{};
    resource.buffer = owned;
    resource.currentState = nvrhi::ResourceStates::Common;
    resource.lifetime = ResourceLifetime::Transient;
    resource.owned = owned;
    m_buffers.push_back(resource);
    return handle;
}

void GraphBuilder::extractTexture(TextureHandle handle, nvrhi::ResourceStates finalState)
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

void GraphBuilder::extractBuffer(BufferHandle handle, nvrhi::ResourceStates finalState)
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
    pass.name = std::string(name);
    pass.setup = std::move(setup);
    pass.execute = std::move(execute);
    pass.options = options;

    if (pass.options.enabled && pass.setup)
    {
        PassBuilder builder(*this);
        pass.setup(builder);
        pass.textureReads = builder.textureReads();
        pass.textureWrites = builder.textureWrites();
        pass.bufferReads = builder.bufferReads();
        pass.bufferWrites = builder.bufferWrites();
    }

    m_passNames.push_back(pass.name);
    m_passes.push_back(std::move(pass));
    m_compiled = false;
}

void GraphBuilder::compile()
{
    for (Pass& pass : m_passes)
        pass.active = false;
    m_compiledPassOrder.clear();

    std::vector<std::vector<uint32_t>> incoming(m_passes.size());
    std::vector<std::vector<uint32_t>> outgoing(m_passes.size());
    std::vector<bool> rootPass(m_passes.size(), false);
    std::vector<bool> referenced(m_textures.size(), false);
    std::vector<bool> referencedBuffers(m_buffers.size(), false);
    std::vector<int32_t> lastTextureWriter(m_textures.size(), -1);
    std::vector<int32_t> lastBufferWriter(m_buffers.size(), -1);

    const auto addDependency = [&](uint32_t before, uint32_t after) {
        if (before == after)
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
        }
        for (const auto& [handle, access] : pass.textureWrites)
        {
            (void)access;
            assert(isValid(handle, m_textures.size()) && "RenderGraph pass write references invalid texture handle");
            if (!isValid(handle, m_textures.size()))
                continue;

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
        }
        for (const auto& [handle, access] : pass.bufferWrites)
        {
            (void)access;
            assert(isValid(handle, m_buffers.size()) && "RenderGraph pass write references invalid buffer handle");
            if (!isValid(handle, m_buffers.size()))
                continue;

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
    for (size_t emittedCount = 0; emittedCount < m_passes.size();)
    {
        bool emittedOne = false;
        for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(m_passes.size()); ++passIndex)
        {
            if (!needed[passIndex] || emitted[passIndex] || indegree[passIndex] != 0)
                continue;

            emitted[passIndex] = true;
            emittedOne = true;
            ++emittedCount;
            m_compiledPassOrder.push_back(passIndex);

            for (const uint32_t dependent : outgoing[passIndex])
            {
                if (needed[dependent] && indegree[dependent] > 0)
                    --indegree[dependent];
            }
        }

        if (!emittedOne)
            break;
    }

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
        for (uint32_t passIndex = 0; passIndex < static_cast<uint32_t>(m_passes.size()); ++passIndex)
        {
            if (needed[passIndex])
                m_compiledPassOrder.push_back(passIndex);
        }
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

    m_compiled = true;
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
}

void GraphBuilder::transitionTexture(nvrhi::ICommandList* commandList, TextureHandle handle, TextureAccess access)
{
    transitionTexture(commandList, handle, accessToState(access));
}

void GraphBuilder::transitionTexture(nvrhi::ICommandList* commandList, TextureHandle handle, nvrhi::ResourceStates targetState)
{
    if (!isValid(handle, m_textures.size()))
        return;

    GraphTexture& resource = m_textures[handle.index];
    if (!resource.texture)
        return;

    if (resource.currentState == targetState)
        return;

    commandList->setTextureState(resource.texture, nvrhi::AllSubresources, targetState);
    resource.currentState = targetState;
}

void GraphBuilder::transitionBuffer(nvrhi::ICommandList* commandList, BufferHandle handle, BufferAccess access)
{
    transitionBuffer(commandList, handle, accessToState(access));
}

void GraphBuilder::transitionBuffer(nvrhi::ICommandList* commandList, BufferHandle handle, nvrhi::ResourceStates targetState)
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

void GraphBuilder::transitionExtractedResources(nvrhi::ICommandList* commandList)
{
    bool hasTransitions = false;

    for (size_t i = 0; i < m_textures.size(); ++i)
    {
        GraphTexture& resource = m_textures[i];
        if (!resource.finalState.has_value())
            continue;

        const nvrhi::ResourceStates before = resource.currentState;
        transitionTexture(commandList, TextureHandle{ static_cast<uint32_t>(i) }, *resource.finalState);
        hasTransitions = hasTransitions || before != resource.currentState;
    }

    for (size_t i = 0; i < m_buffers.size(); ++i)
    {
        GraphBuffer& resource = m_buffers[i];
        if (!resource.finalState.has_value())
            continue;

        const nvrhi::ResourceStates before = resource.currentState;
        transitionBuffer(commandList, BufferHandle{ static_cast<uint32_t>(i) }, *resource.finalState);
        hasTransitions = hasTransitions || before != resource.currentState;
    }

    if (hasTransitions)
        commandList->commitBarriers();
}

void GraphBuilder::execute(nvrhi::ICommandList* commandList)
{
    assert(commandList);
    if (!m_compiled)
        compile();

    for (const uint32_t passIndex : m_compiledPassOrder)
    {
        if (passIndex >= m_passes.size())
            continue;

        const Pass& pass = m_passes[passIndex];
        if (!pass.active)
            continue;

        commandList->beginMarker(pass.name.c_str());

        for (const auto& [handle, access] : pass.textureReads)
            transitionTexture(commandList, handle, access);
        for (const auto& [handle, access] : pass.textureWrites)
            transitionTexture(commandList, handle, access);
        for (const auto& [handle, access] : pass.bufferReads)
            transitionBuffer(commandList, handle, access);
        for (const auto& [handle, access] : pass.bufferWrites)
            transitionBuffer(commandList, handle, access);

        if (!pass.textureReads.empty() || !pass.textureWrites.empty() || !pass.bufferReads.empty() || !pass.bufferWrites.empty())
            commandList->commitBarriers();

        if (pass.execute)
        {
            RenderPassContext context(commandList, *this);
            pass.execute(context);
        }

        syncPassEndStates(pass);

        commandList->endMarker();
    }

    transitionExtractedResources(commandList);
}

void GraphBuilder::reset()
{
    releaseTransientResources();
    m_textures.clear();
    m_buffers.clear();
    m_passes.clear();
    m_compiledPassOrder.clear();
    m_passNames.clear();
    m_importIndexByTexture.clear();
    m_importIndexByBuffer.clear();
    m_compiled = false;
}

nvrhi::ITexture* GraphBuilder::resolveTexture(TextureHandle handle) const
{
    if (!isValid(handle, m_textures.size()))
        return nullptr;
    return m_textures[handle.index].texture;
}

nvrhi::IBuffer* GraphBuilder::resolveBuffer(BufferHandle handle) const
{
    if (!isValid(handle, m_buffers.size()))
        return nullptr;
    return m_buffers[handle.index].buffer;
}

nvrhi::ResourceStates GraphBuilder::textureState(TextureHandle handle) const
{
    if (!isValid(handle, m_textures.size()))
        return nvrhi::ResourceStates::Common;
    return m_textures[handle.index].currentState;
}

nvrhi::ResourceStates GraphBuilder::bufferState(BufferHandle handle) const
{
    if (!isValid(handle, m_buffers.size()))
        return nvrhi::ResourceStates::Common;
    return m_buffers[handle.index].currentState;
}

} // namespace caustica::rg
