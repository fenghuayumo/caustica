#pragma once

#include <render/graph/GraphBuilder.h>

#include <vector>

namespace caustica::rg
{

class TransientResourceAllocator
{
public:
    void allocate(
        GraphBuilder& graph,
        const std::vector<bool>& referencedTextures,
        const std::vector<bool>& referencedBuffers,
        const std::vector<GraphBuilder::TransientLifetime>& textureLifetimes,
        const std::vector<GraphBuilder::TransientLifetime>& bufferLifetimes);
};

} // namespace caustica::rg
