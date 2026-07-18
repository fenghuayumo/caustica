#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <vector>

class ShaderDebug;
struct GaussianSplatEmissionProxy;

namespace caustica::render
{

class PathTracingContext;

// Lighting create / per-frame update helpers (no WorldRenderer symbols).
void createLightingRenderPasses(
    PathTracingContext& context,
    nvrhi::IDevice* device,
    const std::shared_ptr<ShaderDebug>& shaderDebug,
    nvrhi::BindingLayoutHandle bindlessLayout,
    nvrhi::CommandListHandle initializeCommandList,
    dm::uint2 screenResolution);

void preUpdateLightingFrame(
    PathTracingContext& context,
    nvrhi::CommandListHandle commandList,
    bool& needNewBindings);

void updateLightingFrame(
    PathTracingContext& context,
    nvrhi::CommandListHandle commandList,
    uint64_t frameIndex,
    const std::vector<GaussianSplatEmissionProxy>* gaussianSplatEmissionProxies);

} // namespace caustica::render
