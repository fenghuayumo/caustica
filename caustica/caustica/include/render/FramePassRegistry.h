#pragma once

#include <render/WorldRenderer/PathTracingFramePipeline.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace caustica::render
{

enum class FramePassInsertPoint
{
    // Matches WorldRenderer::ensureFramePipelineBuilt() ordering.
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

// Declarative hook for future Render Graph work. Phase 0: records pass factories
// and applies them to PathTracingFramePipeline when WorldRenderer builds the frame list.
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

    // Merge pending registrations into `pipeline` (called from WorldRenderer once).
    void applyTo(PathTracingFramePipeline& pipeline) const;

    void clear();

    [[nodiscard]] size_t registrationCount() const { return m_registrations.size(); }

private:
    std::vector<PassRegistration> m_registrations;
};

} // namespace caustica::render
