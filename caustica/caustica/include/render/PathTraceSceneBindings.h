#pragma once

#include <rhi/rhi.h>

#include <utility>

namespace caustica::render
{

// Owns the scene binding-set generation and the optional Gaussian resources it
// represents. WorldRenderer builds a generation; RT subsystems only invalidate it.
class PathTraceSceneBindings
{
public:
    void invalidate()
    {
        m_bindingSet = nullptr;
        m_gaussianSplatAS = nullptr;
        m_gaussianSplatBuffer = nullptr;
        m_gaussianSplatShBuffer = nullptr;
    }

    void publish(
        caustica::rhi::BindingSetHandle bindingSet,
        caustica::rhi::rt::AccelStruct* gaussianSplatAS,
        caustica::rhi::Buffer* gaussianSplatBuffer,
        caustica::rhi::Buffer* gaussianSplatShBuffer)
    {
        m_bindingSet = std::move(bindingSet);
        m_gaussianSplatAS = gaussianSplatAS;
        m_gaussianSplatBuffer = gaussianSplatBuffer;
        m_gaussianSplatShBuffer = gaussianSplatShBuffer;
    }

    [[nodiscard]] const caustica::rhi::BindingSetHandle& bindingSet() const { return m_bindingSet; }
    [[nodiscard]] bool ready() const { return m_bindingSet != nullptr; }

    [[nodiscard]] bool matchesGaussianResources(
        caustica::rhi::rt::AccelStruct* gaussianSplatAS,
        caustica::rhi::Buffer* gaussianSplatBuffer,
        caustica::rhi::Buffer* gaussianSplatShBuffer) const
    {
        return gaussianSplatAS == m_gaussianSplatAS
            && gaussianSplatBuffer == m_gaussianSplatBuffer
            && gaussianSplatShBuffer == m_gaussianSplatShBuffer;
    }

private:
    caustica::rhi::BindingSetHandle m_bindingSet;
    caustica::rhi::rt::AccelStruct* m_gaussianSplatAS = nullptr;
    caustica::rhi::Buffer* m_gaussianSplatBuffer = nullptr;
    caustica::rhi::Buffer* m_gaussianSplatShBuffer = nullptr;
};

} // namespace caustica::render
