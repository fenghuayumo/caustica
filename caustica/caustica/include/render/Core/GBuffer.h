#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>
#include <memory>

namespace caustica
{
    class CommonRenderPasses;
    class FramebufferFactory;
}

namespace caustica::render
{
    class GBufferRenderTargets
    {
    protected:
        dm::uint2 m_Size = dm::uint2::zero();
        dm::uint m_SampleCount = 0;
        bool m_UseReverseProjection = false;

    public:
        nvrhi::TextureHandle Depth;
        nvrhi::TextureHandle GBufferDiffuse;
        nvrhi::TextureHandle GBufferSpecular;
        nvrhi::TextureHandle GBufferNormals;
        nvrhi::TextureHandle GBufferEmissive;

        nvrhi::TextureHandle MotionVectors;

        std::shared_ptr<caustica::FramebufferFactory> GBufferFramebuffer;

        virtual ~GBufferRenderTargets() = default;

        virtual void Init(
            nvrhi::IDevice* device,
            dm::uint2 size, 
            dm::uint sampleCount,
            bool enableMotionVectors,
            bool useReverseProjection);

        virtual void Clear(nvrhi::ICommandList* commandList);

        [[nodiscard]] dm::uint2 GetSize() const { return m_Size; }
        [[nodiscard]] dm::uint GetSampleCount() const { return m_SampleCount; }
        [[nodiscard]] bool GetUseReverseProjection() const { return m_UseReverseProjection; }
    };
}