#pragma once

#include <render/worldRenderer/PathTracingFrameContext.h>

namespace caustica::render
{

class IPathTracingFramePass
{
public:
    virtual ~IPathTracingFramePass() = default;

    [[nodiscard]] virtual const char* name() const = 0;
    virtual void execute(PathTracingFrameContext& context) = 0;
};

} // namespace caustica::render
