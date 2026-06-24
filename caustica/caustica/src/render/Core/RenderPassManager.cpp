#include "render/Core/RenderPassManager.h"
#include "backend/GpuDevice.h"  // IRenderPass definition

namespace caustica {

void RenderPassManager::addToFront(IRenderPass* pass,
                                    uint32_t backBufferWidth,
                                    uint32_t backBufferHeight,
                                    uint32_t sampleCount)
{
    m_Passes.remove(pass);
    m_Passes.push_front(pass);

    pass->BackBufferResizing();
    pass->BackBufferResized(backBufferWidth, backBufferHeight, sampleCount);
}

void RenderPassManager::addToBack(IRenderPass* pass,
                                   uint32_t backBufferWidth,
                                   uint32_t backBufferHeight,
                                   uint32_t sampleCount)
{
    m_Passes.remove(pass);
    m_Passes.push_back(pass);

    pass->BackBufferResizing();
    pass->BackBufferResized(backBufferWidth, backBufferHeight, sampleCount);
}

void RenderPassManager::remove(IRenderPass* pass)
{
    m_Passes.remove(pass);
}

void RenderPassManager::notifyResizing()
{
    for (auto* pass : m_Passes)
        pass->BackBufferResizing();
}

void RenderPassManager::notifyResized(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    for (auto* pass : m_Passes)
        pass->BackBufferResized(width, height, sampleCount);
}

} // namespace caustica
