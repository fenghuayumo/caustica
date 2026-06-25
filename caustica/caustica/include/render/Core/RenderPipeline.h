#pragma once

#include <rhi/nvrhi.h>
#include <math/math.h>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{
class BindingCache;
class CommonRenderPasses;
class DescriptorTableManager;
class ShaderFactory;
struct ExecuteContext;
class IRenderPipelinePass;
} // namespace caustica

class RenderTargets;

namespace caustica
{
class IRenderPipelinePass
{
public:
    virtual ~IRenderPipelinePass() = default;
    virtual void Render(nvrhi::IFramebuffer* framebuffer) = 0;
    virtual void BackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) {}
};
} // namespace caustica

// =============================================================================
// RenderPipeline — owns shared rendering resources and manages the ordered
// execution of render passes.
//
// Shared resources (render targets, binding infrastructure, command list,
// common passes) are exposed as public members so that passes can access
// them directly without going through getter chains.
//
// Pass registration and ordered execution provide a lightweight alternative
// to the hardcoded 473-line Render() sequence in PathTracerApp.
// =============================================================================
class RenderPipeline
{
public:
    // ========================================================================
    // Construction / Destruction
    // ========================================================================
    RenderPipeline(nvrhi::IDevice* device,
                   std::shared_ptr<caustica::ShaderFactory> shaderFactory);
    ~RenderPipeline();

    // Non-copyable, movable
    RenderPipeline(const RenderPipeline&) = delete;
    RenderPipeline& operator=(const RenderPipeline&) = delete;
    RenderPipeline(RenderPipeline&&) = default;
    RenderPipeline& operator=(RenderPipeline&&) = default;

    // ========================================================================
    // Shared resources (public for direct access by passes)
    // ========================================================================

    std::unique_ptr<RenderTargets>                           renderTargets;
    std::shared_ptr<caustica::BindingCache>                   bindingCache;
    std::shared_ptr<caustica::CommonRenderPasses>             commonPasses;
    std::shared_ptr<caustica::DescriptorTableManager>         descriptorTable;

    nvrhi::BufferHandle                                      constantBuffer;

    nvrhi::BindingLayoutHandle                               bindingLayout;
    nvrhi::BindingLayoutHandle                               bindlessLayout;
    nvrhi::BindingSetHandle                                  bindingSet;

    // ========================================================================
    // Device / command list
    // ========================================================================

    [[nodiscard]] nvrhi::IDevice*        GetDevice() const { return m_device; }

    // The shared per-frame command list.
    [[nodiscard]] nvrhi::CommandListHandle GetCommandList() const { return m_commandList; }

    // Set the shared command list handle.
    void SetCommandList(nvrhi::CommandListHandle cl) { m_commandList = cl; }

    // Open the command list for a new frame.
    void OpenCommandList();

    // Close the command list and submit it to the device for execution.
    void CloseAndSubmitCommandList();

    // ========================================================================
    // Render target lifecycle
    // ========================================================================

    // Returns true if render targets need to be (re)created due to a size
    // or sample-count change.
    [[nodiscard]] bool IsRenderTargetUpdateRequired(dm::uint2 renderSize,
                                                    dm::uint2 displaySize,
                                                    dm::uint  sampleCount = 1) const;

    // Create (or recreate) all render targets. Destroys any existing targets
    // and calls BackBufferResized on every registered pass.
    void CreateRenderTargets(dm::uint2 renderSize,
                             dm::uint2 displaySize,
                             bool      enableMotionVectors,
                             bool      useReverseProjection,
                             int       backbufferCount);

    // Destroy render targets and notify registered passes.
    void DestroyRenderTargets();

    // ========================================================================
    // Binding set management
    // ========================================================================

    // Recreate the global binding set from the given descriptor.
    void RecreateBindingSet(const nvrhi::BindingSetDesc& desc,
                            nvrhi::IBindingLayout*        layout);

    // Mark the current binding set as invalid, forcing recreation next frame.
    void InvalidateBindingSet() { bindingSet = nullptr; }

    [[nodiscard]] bool IsBindingSetValid() const { return bindingSet != nullptr; }

    // ========================================================================
    // Pass registration
    // ========================================================================

    // Register a non-owning pass for ordered execution.
    void RegisterPass(const std::string& name, caustica::IRenderPipelinePass* pass);

    // Create and register a pass, transferring ownership to the pipeline.
    // The pass type T must be constructable from (nvrhi::IDevice*, Args&&...).
    template<typename T, typename... Args>
    T& EmplacePass(const std::string& name, Args&&... args)
    {
        auto owned = std::make_unique<T>(m_device, std::forward<Args>(args)...);
        T* raw = owned.get();
        m_passes.push_back({name, raw, std::move(owned)});
        return *raw;
    }

    // Remove a previously registered pass.
    void UnregisterPass(const std::string& name);

    // Find a registered pass by name and type. Returns nullptr if not found.
    template<typename T = caustica::IRenderPipelinePass>
    T* FindPass(const std::string& name) const
    {
        for (auto& entry : m_passes)
        {
            if (entry.name == name)
                return dynamic_cast<T*>(entry.pass);
        }
        return nullptr;
    }

    // ========================================================================
    // Execution
    // ========================================================================

    // Open the command list for a new frame and reset frame state.
    void BeginFrame();

    // Execute all registered passes in registration order.
    void ExecuteAll(nvrhi::IFramebuffer* framebuffer);

    // Close the command list and submit it for execution, then advance
    // the frame index.
    void EndFrame();

    // ========================================================================
    // Frame / render state
    // ========================================================================

    [[nodiscard]] uint64_t GetFrameIndex() const { return m_frameIndex; }
    [[nodiscard]] uint32_t GetSampleIndex() const { return m_sampleIndex; }
    void SetSampleIndex(uint32_t index) { m_sampleIndex = index; }

    void AdvanceFrame();

    dm::uint2  GetRenderSize()         const { return m_renderSize; }
    dm::uint2  GetDisplaySize()        const { return m_displaySize; }
    float      GetDisplayAspectRatio() const { return m_displayAspectRatio; }

    void SetRenderSize(dm::uint2 size)          { m_renderSize = size; }
    void SetDisplaySize(dm::uint2 size)         { m_displaySize = size; }
    void SetDisplayAspectRatio(float ratio)     { m_displayAspectRatio = ratio; }

private:
    nvrhi::IDevice*                         m_device = nullptr;
    nvrhi::CommandListHandle                m_commandList;

    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;

    dm::uint2                               m_renderSize  = {};
    dm::uint2                               m_displaySize = {};
    float                                   m_displayAspectRatio = 1.0f;
    uint64_t                                m_frameIndex  = 0;
    uint32_t                                m_sampleIndex = 0;

    struct PassEntry
    {
        std::string                       name;
        caustica::IRenderPipelinePass*            pass;
        std::unique_ptr<caustica::IRenderPipelinePass> ownedPtr;
    };
    std::vector<PassEntry> m_passes;

    // Cached render-target creation parameters for BackBufferResized
    // notifications.
    uint32_t m_lastRTWidth  = 0;
    uint32_t m_lastRTHeight = 0;
    bool     m_hasRenderTargets = false;
};
