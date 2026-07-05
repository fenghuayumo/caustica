#pragma once

#include <render/ecs/RenderFrameContext.h>

namespace caustica::ecs
{
class World;
struct ScheduleContext;
}

namespace caustica::render
{

class WorldRenderer;

using RenderPrepareSystemFn = void (*)(WorldRenderer& renderer, RenderFrameContext& ctx);
using RenderGraphSystemFn = void (*)(WorldRenderer& renderer, RenderFrameContext& ctx, ecs::World& world);

void FrameSetupSystem(WorldRenderer& renderer, RenderFrameContext& ctx);
void ExtractFrameViewSystem(ecs::World& world, const ecs::ScheduleContext& scheduleContext);
void EnsureRenderTargetsSystem(WorldRenderer& renderer, RenderFrameContext& ctx);
void RendererInitSystem(WorldRenderer& renderer, RenderFrameContext& ctx);
void ShaderUpdateSystem(WorldRenderer& renderer, RenderFrameContext& ctx);
void BeginCommandListSystem(WorldRenderer& renderer, RenderFrameContext& ctx);
void SceneUpdateSystem(WorldRenderer& renderer, RenderFrameContext& ctx);
void PathTracePrepareSystem(WorldRenderer& renderer, RenderFrameContext& ctx);
void PathTraceSystem(WorldRenderer& renderer, RenderFrameContext& ctx);
void DenoiseAndAASystem(WorldRenderer& renderer, RenderFrameContext& ctx);

void BuildPostProcessGraphSystem(WorldRenderer& renderer, RenderFrameContext& ctx, ecs::World& world);
void BuildCompositeGraphSystem(WorldRenderer& renderer, RenderFrameContext& ctx, ecs::World& world);
void ExecuteRenderGraphSystem(WorldRenderer& renderer, RenderFrameContext& ctx);

void DebugLinesSystem(WorldRenderer& renderer, RenderFrameContext& ctx);
void FinalizeSystem(WorldRenderer& renderer, RenderFrameContext& ctx);

} // namespace caustica::render
