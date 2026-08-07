#pragma once

#include <scene/SceneRenderData.h>

#include <cstdint>

namespace caustica
{

// Per-frame scratch for Bevy-style Extract schedule systems (logic thread only).
struct RenderExtractScratch
{
    bool active = false;
    bool canStartStructure = false;
    uint32_t frameIndex = 0;
    scene::FrameExtractInputs frameInputs{};
};

} // namespace caustica
