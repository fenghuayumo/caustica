#include <render/WorldRenderer/PathTracingFramePipeline.h>

#include <cassert>

namespace caustica::render
{

class PathTracingFramePipeline::LambdaPass final : public IPathTracingFramePass
{
public:
    LambdaPass(std::string name, std::function<void(PathTracingFrameContext&)> fn)
        : m_name(std::move(name))
        , m_fn(std::move(fn))
    {
    }

    [[nodiscard]] const char* name() const override { return m_name.c_str(); }

    void execute(PathTracingFrameContext& context) override
    {
        if (m_fn)
            m_fn(context);
    }

private:
    std::string m_name;
    std::function<void(PathTracingFrameContext&)> m_fn;
};

void PathTracingFramePipeline::registerPass(const std::string& name, IPathTracingFramePass* pass)
{
    m_passes.push_back({name, pass, nullptr});
}

void PathTracingFramePipeline::registerLambdaPass(const std::string& name,
                                                std::function<void(PathTracingFrameContext&)> fn)
{
    auto pass = std::make_unique<LambdaPass>(name, std::move(fn));
    IPathTracingFramePass* raw = pass.get();
    m_passes.push_back({name, raw, std::move(pass)});
}

void PathTracingFramePipeline::insertPassAfter(const std::string& anchorName,
                                               const std::string& name,
                                               IPathTracingFramePass* pass)
{
    for (auto it = m_passes.begin(); it != m_passes.end(); ++it)
    {
        if (it->name == anchorName)
        {
            m_passes.insert(it + 1, {name, pass, nullptr});
            return;
        }
    }
    m_passes.push_back({name, pass, nullptr});
}

void PathTracingFramePipeline::insertLambdaPassAfter(const std::string& anchorName,
                                                     const std::string& name,
                                                     std::function<void(PathTracingFrameContext&)> fn)
{
    auto pass = std::make_unique<LambdaPass>(name, std::move(fn));
    IPathTracingFramePass* raw = pass.get();
    for (auto it = m_passes.begin(); it != m_passes.end(); ++it)
    {
        if (it->name == anchorName)
        {
            m_passes.insert(it + 1, {name, raw, std::move(pass)});
            return;
        }
    }
    m_passes.push_back({name, raw, std::move(pass)});
}

void PathTracingFramePipeline::executeAll(PathTracingFrameContext& context) const
{
    for (const PassEntry& entry : m_passes)
    {
        if (context.aborted)
            break;

        if (entry.pass)
            entry.pass->execute(context);
    }
}

void PathTracingFramePipeline::clear()
{
    m_passes.clear();
}

} // namespace caustica::render
