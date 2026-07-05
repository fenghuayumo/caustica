#include <render/ecs/RenderScheduleSetup.h>
#include <render/ecs/RenderSystems.h>
#include <render/ecs/RenderWorldResources.h>
#include <render/FramePassRegistry.h>
#include <render/worldRenderer/WorldRenderer.h>

namespace caustica::render
{

namespace
{
    void addRendererSystem(
        ecs::Schedule& schedule,
        const char* setName,
        const char* name,
        RenderPrepareSystemFn fn)
    {
        schedule.addSystem(setName, name,
            [fn](ecs::World& world, const ecs::ScheduleContext& /*ctx*/) {
                RenderFrameResource* frame = world.getResource<RenderFrameResource>();
                if (!frame || !frame->renderer || !frame->context || frame->context->frame.aborted)
                    return;
                fn(*frame->renderer, *frame->context);
            });
    }

    void addGraphSystem(
        ecs::Schedule& schedule,
        const char* name,
        RenderGraphSystemFn fn)
    {
        schedule.addSystem("BuildGraph", name,
            [fn](ecs::World& world, const ecs::ScheduleContext& /*ctx*/) {
                RenderFrameResource* frame = world.getResource<RenderFrameResource>();
                if (!frame || !frame->renderer || !frame->context || frame->context->frame.aborted)
                    return;
                fn(*frame->renderer, *frame->context, world);
                frame->context->graphBuilt = true;
            });
    }
}

void buildDefaultRenderSchedule(
    ecs::Schedule& schedule,
    WorldRenderer& renderer,
    FramePassRegistry* framePassRegistry)
{
    schedule
        .addSet("Extract")
        .addSet("Prepare")
        .addSet("Queue")
        .addSet("BuildGraph")
        .addSet("ExecuteGraph")
        .addSet("Cleanup");

    addRendererSystem(schedule, "Extract", "FrameSetup", FrameSetupSystem);
    schedule.addSystem("Extract", "ExtractFrameView", ExtractFrameViewSystem);
    addRendererSystem(schedule, "Prepare", "EnsureRenderTargets", EnsureRenderTargetsSystem);
    addRendererSystem(schedule, "Prepare", "RendererInit", RendererInitSystem);
    addRendererSystem(schedule, "Prepare", "ShaderUpdate", ShaderUpdateSystem);
    addRendererSystem(schedule, "Prepare", "BeginCommandList", BeginCommandListSystem);
    addRendererSystem(schedule, "Prepare", "SceneUpdate", SceneUpdateSystem);
    addRendererSystem(schedule, "Prepare", "PathTracePrepare", PathTracePrepareSystem);
    addRendererSystem(schedule, "Prepare", "PathTrace", PathTraceSystem);
    addRendererSystem(schedule, "Prepare", "DenoiseAndAA", DenoiseAndAASystem);

    addGraphSystem(schedule, "BuildFrameGraph", BuildFrameGraphSystem);

    schedule.addSystem("ExecuteGraph", "ExecuteRenderGraph",
        [](ecs::World& world, const ecs::ScheduleContext& /*ctx*/) {
            RenderFrameResource* frame = world.getResource<RenderFrameResource>();
            if (!frame || !frame->renderer || !frame->context || frame->context->frame.aborted)
                return;
            ExecuteRenderGraphSystem(*frame->renderer, *frame->context);
        });

    addRendererSystem(schedule, "Cleanup", "DebugLines", DebugLinesSystem);
    addRendererSystem(schedule, "Cleanup", "Finalize", FinalizeSystem);

    schedule
        .before("FrameSetup", "EnsureRenderTargets")
        .before("FrameSetup", "ExtractFrameView")
        .before("ExtractFrameView", "EnsureRenderTargets")
        .before("EnsureRenderTargets", "RendererInit")
        .before("RendererInit", "ShaderUpdate")
        .before("ShaderUpdate", "BeginCommandList")
        .before("BeginCommandList", "SceneUpdate")
        .before("SceneUpdate", "PathTracePrepare")
        .before("PathTracePrepare", "PathTrace")
        .before("PathTrace", "DenoiseAndAA")
        .before("DenoiseAndAA", "BuildFrameGraph")
        .before("BuildFrameGraph", "ExecuteRenderGraph")
        .before("ExecuteRenderGraph", "DebugLines")
        .before("DebugLines", "Finalize");

    if (framePassRegistry)
        framePassRegistry->applyToRenderSchedule(schedule, renderer);
}

} // namespace caustica::render
