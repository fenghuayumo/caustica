#include <render/core/HdrPostProcess.h>
#include <render/core/CameraController.h>
#include <render/core/RenderTargets.h>
#include <scene/View.h>
#include <render/passes/geometry/BloomPass.h>

namespace caustica
{

void hdrPostProcess(CameraController& camera, HdrPostProcessParams& params)
{
    nvrhi::ICommandList* commandList = params.commandList;
    if (commandList == nullptr || params.renderTargets == nullptr || params.bloomPass == nullptr)
        return;

    if (!params.settings.EnableBloom || params.settings.BloomIntensity <= 0.f
        || params.settings.BloomRadius <= 0.f)
    {
        return;
    }

    PlanarView fullscreenView = *camera.view();
    ViewportDesc windowViewport(float(params.displaySize.x), float(params.displaySize.y));
    fullscreenView.setViewport(windowViewport);
    fullscreenView.updateCache();

    params.bloomPass->Render(
        commandList,
        params.renderTargets->processedOutputFramebuffer,
        fullscreenView,
        params.renderTargets->processedOutputColor,
        params.settings.BloomRadius,
        params.settings.BloomIntensity);
}

} // namespace caustica
