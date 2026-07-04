#include <render/FramePassRegistry.h>
#include <render/graph/GraphBuilder.h>

namespace caustica::render
{

namespace
{
    const char* insertPointAnchorName(FramePassInsertPoint point)
    {
        switch (point)
        {
        case FramePassInsertPoint::BeforeFrameSetup:      return "FrameSetup";
        case FramePassInsertPoint::AfterFrameSetup:       return "FrameSetup";
        case FramePassInsertPoint::AfterRenderTargets:    return "RenderTargets";
        case FramePassInsertPoint::AfterRendererInit:     return "RendererInit";
        case FramePassInsertPoint::AfterShaderUpdate:     return "ShaderUpdate";
        case FramePassInsertPoint::AfterBeginCommandList: return "BeginCommandList";
        case FramePassInsertPoint::AfterSceneUpdate:      return "SceneUpdate";
        case FramePassInsertPoint::AfterPathTracePrepare: return "PathTracePrepare";
        case FramePassInsertPoint::AfterPathTrace:        return "PathTrace";
        case FramePassInsertPoint::AfterDenoiseAndAA:     return "DenoiseAndAA";
        case FramePassInsertPoint::AfterToneMapping:      return "ToneMapping";
        case FramePassInsertPoint::AfterComposite:        return "Composite";
        case FramePassInsertPoint::AfterFinalize:         return "Finalize";
        }
        return "Finalize";
    }
}

void FramePassRegistry::registerPassFactory(std::string name,
    FramePassInsertPoint insertAfter,
    PassFactory factory)
{
    m_registrations.push_back(PassRegistration{
        .name = std::move(name),
        .insertAfter = insertAfter,
        .factory = std::move(factory),
    });
}

void FramePassRegistry::registerLambdaPass(std::string name,
    FramePassInsertPoint insertAfter,
    std::function<void(PathTracingFrameContext&)> fn)
{
    registerPassFactory(std::move(name), insertAfter, [passName = std::move(name), fn = std::move(fn)]() mutable {
        struct LambdaPass final : IPathTracingFramePass
        {
            explicit LambdaPass(std::string n, std::function<void(PathTracingFrameContext&)> f)
                : m_name(std::move(n))
                , m_fn(std::move(f))
            {
            }

            [[nodiscard]] const char* name() const override { return m_name.c_str(); }
            void execute(PathTracingFrameContext& context) override
            {
                if (m_fn)
                    m_fn(context);
            }

            std::string m_name;
            std::function<void(PathTracingFrameContext&)> m_fn;
        };
        return std::make_unique<LambdaPass>(std::move(passName), std::move(fn));
    });
}

void FramePassRegistry::registerGraphPass(std::string name, FramePassInsertPoint insertAfter, GraphPassFn fn)
{
    m_graphPasses.push_back(GraphPassRegistration{
        .name = std::move(name),
        .insertAfter = insertAfter,
        .fn = std::move(fn),
    });
}

void FramePassRegistry::applyGraphPasses(FramePassInsertPoint insertPoint,
    rg::GraphBuilder& graph,
    PathTracingFrameContext& context) const
{
    for (const GraphPassRegistration& reg : m_graphPasses)
    {
        if (reg.insertAfter != insertPoint || !reg.fn)
            continue;
        reg.fn(graph, context);
    }
}

void FramePassRegistry::applyTo(PathTracingFramePipeline& pipeline) const
{
    for (const PassRegistration& reg : m_registrations)
    {
        if (!reg.factory)
            continue;

        auto pass = reg.factory();
        if (!pass)
            continue;

        const char* anchorName = insertPointAnchorName(reg.insertAfter);
        pipeline.insertPassAfter(anchorName, reg.name, pass.release());
    }
}

void FramePassRegistry::clear()
{
    m_registrations.clear();
    m_graphPasses.clear();
}

} // namespace caustica::render
