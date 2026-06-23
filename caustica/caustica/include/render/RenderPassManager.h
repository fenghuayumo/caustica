#pragma once

#include <list>

namespace caustica {

class IRenderPass;

// =============================================================================
// RenderPassManager — Renderer layer: ordered container of render passes.
//
// Extracted from DeviceManager. Manages pass registration, removal,
// and back-buffer resize notifications. Does NOT own the passes.
// =============================================================================
class RenderPassManager
{
public:
    RenderPassManager() = default;
    ~RenderPassManager() = default;

    // Add to front (renders first). Notifies pass of current back buffer size.
    void addToFront(IRenderPass* pass, uint32_t backBufferWidth, uint32_t backBufferHeight, uint32_t sampleCount);

    // Add to back (renders last). Notifies pass of current back buffer size.
    void addToBack(IRenderPass* pass, uint32_t backBufferWidth, uint32_t backBufferHeight, uint32_t sampleCount);

    // Remove a pass.
    void remove(IRenderPass* pass);

    // Notify all passes that the back buffer is about to resize.
    void notifyResizing();

    // Notify all passes that the back buffer has resized.
    void notifyResized(uint32_t width, uint32_t height, uint32_t sampleCount);

    // --- Iteration (for rendering and animation) ---
    const std::list<IRenderPass*>& passes() const { return m_Passes; }

    // --- Forward iteration (for BackBufferResizing/Resized) ---
    auto begin()       { return m_Passes.begin(); }
    auto end()         { return m_Passes.end(); }

    // --- Reverse iteration (for input dispatch) ---
    auto rbegin()      { return m_Passes.rbegin(); }
    auto rend()        { return m_Passes.rend(); }
    auto crbegin() const { return m_Passes.crbegin(); }
    auto crend() const   { return m_Passes.crend(); }

    size_t count() const { return m_Passes.size(); }
    bool   empty() const { return m_Passes.empty(); }

private:
    std::list<IRenderPass*> m_Passes;
};

} // namespace caustica
