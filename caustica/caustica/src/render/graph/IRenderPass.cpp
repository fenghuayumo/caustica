#include <render/graph/IRenderPass.h>

#include <render/graph/GraphBuilder.h>

#include <cassert>

namespace caustica::rg
{

PassOptions IRenderPass::options() const
{
    return {};
}

void addRenderPass(GraphBuilder& graph, std::unique_ptr<IRenderPass> pass)
{
    assert(pass);
    IRenderPass* rawPass = pass.get();
    graph.addPass(
        rawPass->name(),
        [rawPass](PassBuilder& builder) { rawPass->setup(builder); },
        [rawPass](RenderPassContext& ctx) { rawPass->execute(ctx); },
        rawPass->options());
    graph.retainRenderPass(std::move(pass));
}

} // namespace caustica::rg
