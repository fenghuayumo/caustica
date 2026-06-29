#pragma once

#include <cstdint>

namespace nvrhi
{
class ICommandList;
}

namespace caustica
{
class Scene;
}

namespace caustica::render
{

class SceneGpuUpdater
{
public:
    static void Refresh(Scene& scene, nvrhi::ICommandList* commandList, uint32_t frameIndex);
    static void RefreshAfterLoad(Scene& scene, uint32_t frameIndex);
};

} // namespace caustica::render
