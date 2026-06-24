#include <render/Core/HdrPostProcess.h>
#include <render/Core/RenderCore.h>
#include <render/Core/RenderTargets.h>
#include <render/Core/View.h>
#include <render/Passes/Geometry/BloomPass.h>

namespace caustica
{

void RenderCore::hdrPostProcess(HdrPostProcessParams& params)
{
    nvrhi::ICommandList* commandList = params.commandList;
    if (commandList == nullptr || params.renderTargets == nullptr || params.bloomPass == nullptr)
        return;

    if (!params.settings.EnableBloom || params.settings.BloomIntensity <= 0.f
        || params.settings.BloomRadius <= 0.f)
    {
        return;
    }

    PlanarView fullscreenView = *m_camera.view();
    nvrhi::Viewport windowViewport(float(params.displaySize.x), float(params.displaySize.y));
    fullscreenView.SetViewport(windowViewport);
    fullscreenView.UpdateCache();

    params.bloomPass->Render(
        commandList,
        params.renderTargets->ProcessedOutputFramebuffer,
        fullscreenView,
        params.renderTargets->ProcessedOutputColor,
        params.settings.BloomRadius,
        params.settings.BloomIntensity);
}

} // namespace caustica
