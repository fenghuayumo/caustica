#pragma once

#include <render/Core/PathTracerSettings.h>
#include <rhi/nvrhi.h>
#include <math/math.h>

class RenderTargets;

namespace caustica::render
{
class BloomPass;
}

namespace caustica
{

struct HdrPostProcessParams
{
    PathTracerSettings&              settings;
    nvrhi::ICommandList*             commandList = nullptr;
    RenderTargets*                   renderTargets = nullptr;
    dm::uint2                        displaySize{};

    render::BloomPass*               bloomPass = nullptr;
};

} // namespace caustica
