#pragma once

#include <math/math.h>
#include <rhi/rhi.h>

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
    caustica::rhi::IDevice* device,
    const std::shared_ptr<ShaderDebug>& shaderDebug,
    caustica::rhi::BindingLayoutHandle bindlessLayout,
    caustica::rhi::CommandListHandle initializeCommandList,
    dm::uint2 screenResolution);

void preUpdateLightingFrame(
    PathTracingContext& context,
    caustica::rhi::CommandListHandle commandList,
    bool& needNewBindings);

void updateEnvMapFrame(
    PathTracingContext& context,
    caustica::rhi::CommandListHandle commandList,
    uint64_t frameIndex);

void updateLightSamplingBeginFrame(
    PathTracingContext& context,
    caustica::rhi::CommandListHandle commandList,
    uint64_t frameIndex,
    const std::vector<GaussianSplatEmissionProxy>* gaussianSplatEmissionProxies);

void updateLightingFrame(
    PathTracingContext& context,
    caustica::rhi::CommandListHandle commandList,
    uint64_t frameIndex,
    const std::vector<GaussianSplatEmissionProxy>* gaussianSplatEmissionProxies);

} // namespace caustica::render
