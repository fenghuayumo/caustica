#pragma once

#include <render/WorldRenderer/PathTracingFrameContext.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace caustica::render
{

// =============================================================================
// PathTracingFramePipeline — ordered pass execution for PathTracingWorldRenderer.
//
// Mirrors the RenderPipeline RegisterPass / ExecuteAll pattern so the two can
// be unified in a later phase. Each pass receives a shared PathTracingFrameContext.
// =============================================================================

class IPathTracingFramePass
{
public:
    virtual ~IPathTracingFramePass() = default;

    [[nodiscard]] virtual const char* name() const = 0;
    virtual void execute(PathTracingFrameContext& context) = 0;
};

class PathTracingFramePipeline
{
public:
    void registerPass(const std::string& name, IPathTracingFramePass* pass);

    template<typename T, typename... Args>
    T& emplacePass(const std::string& name, Args&&... args)
    {
        auto owned = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = owned.get();
        m_passes.push_back({name, raw, std::move(owned)});
        return *raw;
    }

    void registerLambdaPass(const std::string& name,
                            std::function<void(PathTracingFrameContext&)> fn);

    void executeAll(PathTracingFrameContext& context) const;

    void clear();

    [[nodiscard]] size_t passCount() const { return m_passes.size(); }

private:
    class LambdaPass;

    struct PassEntry
    {
        std::string                      name;
        IPathTracingFramePass*           pass = nullptr;
        std::unique_ptr<IPathTracingFramePass> owned;
    };

    std::vector<PassEntry> m_passes;
};

} // namespace caustica::render
