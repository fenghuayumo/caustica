#pragma once

#include <render/worldRenderer/PathTracingFramePipeline.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace caustica::ecs
{
class Schedule;
}

namespace caustica::rg
{
class GraphBuilder;
}

namespace caustica::render
{

class WorldRenderer;

enum class FramePassInsertPoint
{
    // Matches WorldRenderer::ensureRenderScheduleBuilt() ordering.
    BeforeFrameSetup,
    AfterFrameSetup,
    AfterRenderTargets,
    AfterRendererInit,
    AfterShaderUpdate,
    AfterBeginCommandList,
    AfterSceneUpdate,
    AfterPathTracePrepare,
    AfterPathTrace,
    AfterDenoiseAndAA,
    AfterToneMapping,
    AfterComposite,
    AfterFinalize,
};

// Declarative hook for Render Graph and render-schedule extension passes.
class FramePassRegistry
{
public:
    using PassFactory = std::function<std::unique_ptr<IPathTracingFramePass>()>;

    struct PassRegistration
    {
        std::string           name;
        FramePassInsertPoint  insertAfter = FramePassInsertPoint::AfterFinalize;
        PassFactory           factory;
    };

    // Register a pass to be inserted after `insertAfter`.
    template<typename T, typename... Args>
    void registerPass(std::string name, FramePassInsertPoint insertAfter, Args&&... args)
    {
        registerPassFactory(std::move(name), insertAfter, [args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            return std::apply([](auto&&... a) {
                return std::make_unique<T>(std::forward<decltype(a)>(a)...);
            }, std::move(args));
        });
    }

    void registerPassFactory(std::string name, FramePassInsertPoint insertAfter, PassFactory factory);

    void registerLambdaPass(std::string name,
        FramePassInsertPoint insertAfter,
        std::function<void(PathTracingFrameContext&)> fn);

    using GraphPassFn = std::function<void(rg::GraphBuilder&, PathTracingFrameContext&)>;

    struct GraphPassRegistration
    {
        std::string          name;
        FramePassInsertPoint insertAfter = FramePassInsertPoint::AfterFinalize;
        GraphPassFn          fn;
    };

    void registerGraphPass(std::string name, FramePassInsertPoint insertAfter, GraphPassFn fn);

    void applyGraphPasses(FramePassInsertPoint insertPoint,
        rg::GraphBuilder& graph,
        PathTracingFrameContext& context) const;

    // Insert registered CPU passes into the render-thread ecs::Schedule.
    void applyToRenderSchedule(ecs::Schedule& schedule, WorldRenderer& renderer) const;

    void clear();

    [[nodiscard]] size_t registrationCount() const { return m_registrations.size(); }
    [[nodiscard]] size_t graphPassCount() const { return m_graphPasses.size(); }

private:
    std::vector<PassRegistration> m_registrations;
    std::vector<GraphPassRegistration> m_graphPasses;
};

} // namespace caustica::render
