#pragma once

#include <memory>
#include <string_view>

namespace caustica::rg
{

struct PassOptions;
class PassBuilder;
class RenderPassContext;
class GraphBuilder;

class IRenderPass
{
public:
    virtual ~IRenderPass() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual PassOptions options() const;
    virtual void setup(PassBuilder& builder) = 0;
    virtual void execute(RenderPassContext& ctx) = 0;
};

void addRenderPass(GraphBuilder& graph, std::unique_ptr<IRenderPass> pass);

} // namespace caustica::rg
