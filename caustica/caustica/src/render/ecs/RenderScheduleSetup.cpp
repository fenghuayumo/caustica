#include <render/ecs/RenderScheduleSetup.h>
#include <render/ecs/RenderSystems.h>
#include <render/FramePassRegistry.h>
#include <render/WorldRenderer/WorldRenderer.h>

namespace caustica::render
{

namespace
{
    void addPrepareSystem(
        ecs::Schedule& schedule,
        WorldRenderer& renderer,
        const char* name,
        RenderPrepareSystemFn fn)
    {
        schedule.addSystem("Prepare", name,
            [&renderer, fn](ecs::World& /*world*/, const ecs::ScheduleContext& /*ctx*/) {
                RenderFrameContext* frameCtx = renderer.activeRenderFrameContext();
                if (!frameCtx || frameCtx->frame.aborted)
                    return;
                fn(renderer, *frameCtx);
            });
    }

    void addGraphSystem(
        ecs::Schedule& schedule,
        WorldRenderer& renderer,
        const char* name,
        RenderGraphSystemFn fn)
    {
        schedule.addSystem("BuildGraph", name,
            [&renderer, fn](ecs::World& /*world*/, const ecs::ScheduleContext& /*ctx*/) {
                RenderFrameContext* frameCtx = renderer.activeRenderFrameContext();
                if (!frameCtx || frameCtx->frame.aborted)
                    return;
                fn(renderer, *frameCtx);
                frameCtx->graphBuilt = true;
            });
    }
}

void buildDefaultRenderSchedule(
    ecs::Schedule& schedule,
    WorldRenderer& renderer,
    FramePassRegistry* framePassRegistry)
{
    schedule
        .addSet("Prepare")
        .addSet("BuildGraph")
        .addSet("ExecuteGraph")
        .addSet("Cleanup");

    addPrepareSystem(schedule, renderer, "FrameSetup", FrameSetupSystem);
    addPrepareSystem(schedule, renderer, "EnsureRenderTargets", EnsureRenderTargetsSystem);
    addPrepareSystem(schedule, renderer, "RendererInit", RendererInitSystem);
    addPrepareSystem(schedule, renderer, "ShaderUpdate", ShaderUpdateSystem);
    addPrepareSystem(schedule, renderer, "BeginCommandList", BeginCommandListSystem);
    addPrepareSystem(schedule, renderer, "SceneUpdate", SceneUpdateSystem);
    addPrepareSystem(schedule, renderer, "PathTracePrepare", PathTracePrepareSystem);
    addPrepareSystem(schedule, renderer, "PathTrace", PathTraceSystem);
    addPrepareSystem(schedule, renderer, "DenoiseAndAA", DenoiseAndAASystem);

    addGraphSystem(schedule, renderer, "BuildPostProcessGraph", BuildPostProcessGraphSystem);
    addGraphSystem(schedule, renderer, "BuildCompositeGraph", BuildCompositeGraphSystem);

    schedule.addSystem("ExecuteGraph", "ExecuteRenderGraph",
        [&renderer](ecs::World& /*world*/, const ecs::ScheduleContext& /*ctx*/) {
            RenderFrameContext* frameCtx = renderer.activeRenderFrameContext();
            if (!frameCtx || frameCtx->frame.aborted)
                return;
            ExecuteRenderGraphSystem(renderer, *frameCtx);
        });

    addPrepareSystem(schedule, renderer, "DebugLines", DebugLinesSystem);
    addPrepareSystem(schedule, renderer, "Finalize", FinalizeSystem);

    schedule
        .before("FrameSetup", "EnsureRenderTargets")
        .before("EnsureRenderTargets", "RendererInit")
        .before("RendererInit", "ShaderUpdate")
        .before("ShaderUpdate", "BeginCommandList")
        .before("BeginCommandList", "SceneUpdate")
        .before("SceneUpdate", "PathTracePrepare")
        .before("PathTracePrepare", "PathTrace")
        .before("PathTrace", "DenoiseAndAA")
        .before("DenoiseAndAA", "BuildPostProcessGraph")
        .before("BuildPostProcessGraph", "BuildCompositeGraph")
        .before("BuildCompositeGraph", "ExecuteRenderGraph")
        .before("ExecuteRenderGraph", "DebugLines")
        .before("DebugLines", "Finalize");

    if (framePassRegistry)
        framePassRegistry->applyToRenderSchedule(schedule, renderer);
}

} // namespace caustica::render
