#include <render/graph/GraphBuilder.h>

#include <cassert>

namespace caustica::rg
{

namespace
{
    bool isValid(TextureHandle handle, size_t textureCount)
    {
        return handle.valid() && handle.index < textureCount;
    }
}

void PassBuilder::read(TextureHandle texture, TextureAccess access)
{
    m_reads.emplace_back(texture, access);
}

void PassBuilder::write(TextureHandle texture, TextureAccess access)
{
    m_writes.emplace_back(texture, access);
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

TextureHandle GraphBuilder::importTexture(nvrhi::ITexture* texture, nvrhi::ResourceStates initialState)
{
    assert(texture);

    if (const auto existing = m_importIndexByTexture.find(texture); existing != m_importIndexByTexture.end())
        return TextureHandle{ existing->second };

    const TextureHandle handle{ static_cast<uint32_t>(m_textures.size()) };
    ImportedTexture imported{};
    imported.texture = texture;
    imported.currentState = initialState;
    m_textures.push_back(imported);
    m_importIndexByTexture.emplace(texture, handle.index);
    return handle;
}

TextureHandle GraphBuilder::importTexture(nvrhi::ITexture* texture, TextureAccess initialAccess)
{
    return importTexture(texture, accessToState(initialAccess));
}

void GraphBuilder::addPass(std::string_view name, SetupFn setup, ExecuteFn execute, bool enabled)
{
    Pass pass;
    pass.name = std::string(name);
    pass.setup = std::move(setup);
    pass.execute = std::move(execute);
    pass.enabled = enabled;

    if (pass.setup)
    {
        PassBuilder builder;
        pass.setup(builder);
        pass.reads = builder.reads();
        pass.writes = builder.writes();
    }

    m_passNames.push_back(pass.name);
    m_passes.push_back(std::move(pass));
}

void GraphBuilder::transitionTexture(nvrhi::ICommandList* commandList, TextureHandle handle, TextureAccess access)
{
    if (!isValid(handle, m_textures.size()))
        return;

    ImportedTexture& imported = m_textures[handle.index];
    const nvrhi::ResourceStates targetState = accessToState(access);
    if (imported.currentState == targetState)
        return;

    commandList->setTextureState(imported.texture, nvrhi::AllSubresources, targetState);
    imported.currentState = targetState;
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
    m_textures.clear();
    m_passes.clear();
    m_passNames.clear();
    m_importIndexByTexture.clear();
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
