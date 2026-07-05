#pragma once

#include <render/ecs/RenderFrameContext.h>

#include <ecs/World.h>
#include <scene/View.h>

#include <utility>

namespace caustica::render
{

class WorldRenderer;

struct RenderFrameResource
{
    WorldRenderer* renderer = nullptr;
    RenderFrameContext* context = nullptr;
};

struct ExtractedFrameView
{
    dm::uint2 displaySize{};
    dm::uint2 renderSize{};
    float displayAspectRatio = 1.0f;
    caustica::PlanarView postProcessView;
};

template<typename T>
T& setRenderWorldResource(ecs::World& world, T value)
{
    if (T* existing = world.getResource<T>())
    {
        *existing = std::move(value);
        return *existing;
    }

    return world.insertResource<T>(std::move(value));
}

} // namespace caustica::render
