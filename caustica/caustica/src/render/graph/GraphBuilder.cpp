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
    default:
        return nvrhi::ResourceStates::Common;
    }
}

TextureHandle GraphBuilder::importTexture(nvrhi::ITexture* texture, TextureAccess initialAccess)
{
    assert(texture);

    TextureHandle handle{ static_cast<uint32_t>(m_textures.size()) };
    ImportedTexture imported{};
    imported.texture = texture;
    imported.currentState = accessToState(initialAccess);
    m_textures.push_back(imported);
    return handle;
}

void GraphBuilder::addPass(std::string_view name, SetupFn setup, ExecuteFn execute)
{
    Pass pass;
    pass.name = std::string(name);
    pass.setup = std::move(setup);
    pass.execute = std::move(execute);

    if (pass.setup)
    {
        PassBuilder builder;
        pass.setup(builder);
        pass.reads = builder.reads();
        pass.writes = builder.writes();
    }

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

void GraphBuilder::execute(nvrhi::ICommandList* commandList)
{
    assert(commandList);

    for (const Pass& pass : m_passes)
    {
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

        commandList->endMarker();
    }
}

nvrhi::ITexture* GraphBuilder::resolveTexture(TextureHandle handle) const
{
    if (!isValid(handle, m_textures.size()))
        return nullptr;
    return m_textures[handle.index].texture;
}

} // namespace caustica::rg
