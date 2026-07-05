#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>
#include <memory>

namespace caustica
{
    class FramebufferFactory;
}

namespace caustica::render
{
    class GBufferRenderTargets
    {
    protected:
        dm::uint2 m_size = dm::uint2::zero();
        dm::uint m_sampleCount = 0;
        bool m_useReverseProjection = false;

    public:
        nvrhi::TextureHandle depth;
        nvrhi::TextureHandle gBufferDiffuse;
        nvrhi::TextureHandle gBufferSpecular;
        nvrhi::TextureHandle gBufferNormals;
        nvrhi::TextureHandle gBufferEmissive;

        nvrhi::TextureHandle motionVectors;

        std::shared_ptr<caustica::FramebufferFactory> gBufferFramebuffer;

        virtual ~GBufferRenderTargets() = default;

        virtual void init(
            nvrhi::IDevice* device,
            dm::uint2 size, 
            dm::uint sampleCount,
            bool enableMotionVectors,
            bool useReverseProjection);

        virtual void clear(nvrhi::ICommandList* commandList);

        [[nodiscard]] dm::uint2 getSize() const { return m_size; }
        [[nodiscard]] dm::uint getSampleCount() const { return m_sampleCount; }
        [[nodiscard]] bool getUseReverseProjection() const { return m_useReverseProjection; }
    };
}