#include <render/graph/GraphBuilder.h>

#include <render/graph/GpuTypes.h>
#include <rhi/nvrhi.h>

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
}

PassBuilder::PassBuilder(GraphBuilder& graph)
    : m_graph(&graph)
{
}

void PassBuilder::read(TextureHandle texture, TextureAccess access)
{
    m_reads.emplace_back(texture, access);
}

void PassBuilder::write(TextureHandle texture, TextureAccess access)
{
    m_writes.emplace_back(texture, access);
}

TextureHandle PassBuilder::createTexture(const TextureDesc& desc)
{
    assert(m_graph);
    return m_graph->createTexture(desc);
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

void GraphBuilder::addPass(std::string_view name, SetupFn setup, ExecuteFn execute, bool enabled)
{
    Pass pass;
    pass.name = std::string(name);
    pass.setup = std::move(setup);
    pass.execute = std::move(execute);
    pass.enabled = enabled;

    if (pass.enabled && pass.setup)
    {
        PassBuilder builder(*this);
        pass.setup(builder);
        pass.reads = builder.reads();
        pass.writes = builder.writes();
    }

    m_passNames.push_back(pass.name);
    m_passes.push_back(std::move(pass));
    m_compiled = false;
}

void GraphBuilder::compile()
{
    std::vector<bool> referenced(m_textures.size(), false);

    for (const Pass& pass : m_passes)
    {
        if (!pass.enabled)
            continue;

        for (const auto& [handle, access] : pass.reads)
        {
            (void)access;
            assert(isValid(handle, m_textures.size()) && "RenderGraph pass read references invalid texture handle");
            referenced[handle.index] = true;
        }
        for (const auto& [handle, access] : pass.writes)
        {
            (void)access;
            assert(isValid(handle, m_textures.size()) && "RenderGraph pass write references invalid texture handle");
            referenced[handle.index] = true;
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
}

void GraphBuilder::transitionTexture(nvrhi::ICommandList* commandList, TextureHandle handle, TextureAccess access)
{
    if (!isValid(handle, m_textures.size()))
        return;

    GraphTexture& resource = m_textures[handle.index];
    const nvrhi::ResourceStates targetState = accessToState(access);
    if (resource.currentState == targetState)
        return;

    commandList->setTextureState(resource.texture, nvrhi::AllSubresources, targetState);
    resource.currentState = targetState;
}

void GraphBuilder::syncPassEndStates(const Pass& pass)
{
    for (const auto& [handle, access] : pass.writes)
    {
        if (isValid(handle, m_textures.size()))
            m_textures[handle.index].currentState = accessToState(access);
    }

    for (const auto& [handle, access] : pass.reads)
    {
        if (!isValid(handle, m_textures.size()) || passUsesTextureAsWrite(pass, handle))
            continue;
        m_textures[handle.index].currentState = accessToState(access);
    }
}

bool GraphBuilder::passUsesTextureAsWrite(const Pass& pass, TextureHandle handle)
{
    for (const auto& [writeHandle, access] : pass.writes)
    {
        if (writeHandle.index == handle.index)
            return true;
        (void)access;
    }
    return false;
}

void GraphBuilder::execute(nvrhi::ICommandList* commandList)
{
    assert(commandList);
    if (!m_compiled)
        compile();

    for (const Pass& pass : m_passes)
    {
        if (!pass.enabled)
            continue;

        commandList->beginMarker(pass.name.c_str());

        for (const auto& [handle, access] : pass.reads)
            transitionTexture(commandList, handle, access);
        for (const auto& [handle, access] : pass.writes)
            transitionTexture(commandList, handle, access);

        if (!pass.reads.empty() || !pass.writes.empty())
            commandList->commitBarriers();

        if (pass.execute)
        {
            RenderPassContext context(commandList, *this);
            pass.execute(context);
        }

        syncPassEndStates(pass);

        commandList->endMarker();
    }
}

void GraphBuilder::reset()
{
    releaseTransientResources();
    m_textures.clear();
    m_passes.clear();
    m_passNames.clear();
    m_importIndexByTexture.clear();
    m_compiled = false;
}

nvrhi::ITexture* GraphBuilder::resolveTexture(TextureHandle handle) const
{
    if (!isValid(handle, m_textures.size()))
        return nullptr;
    return m_textures[handle.index].texture;
}

nvrhi::ResourceStates GraphBuilder::textureState(TextureHandle handle) const
{
    if (!isValid(handle, m_textures.size()))
        return nvrhi::ResourceStates::Common;
    return m_textures[handle.index].currentState;
}

} // namespace caustica::rg
