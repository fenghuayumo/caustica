#pragma once

#include <render/core/PathTracerSettings.h>
#include <rhi/rhi.h>
#include <math/math.h>

class RenderTargets;

namespace caustica::render
{
class BloomPass;
}

namespace caustica
{
class CameraController;

struct HdrPostProcessParams
{
    PathTracerSettings&              settings;
    caustica::rhi::ICommandList*             commandList = nullptr;
    RenderTargets*                   renderTargets = nullptr;
    dm::uint2                        displaySize{};

    render::BloomPass*               bloomPass = nullptr;
};

void hdrPostProcess(CameraController& camera, HdrPostProcessParams& params);

} // namespace caustica
