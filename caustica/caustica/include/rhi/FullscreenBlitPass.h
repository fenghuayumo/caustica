#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>
#include <rhi/StandardSamplers.h>

#include <memory>
#include <unordered_map>

namespace caustica
{
class BindingCache;
class ShaderFactory;
}

namespace caustica::rhi
{

enum class BlitSampler
{
    Point,
    Linear,
    Sharpen
};

struct BlitParameters
{
    nvrhi::IFramebuffer* targetFramebuffer = nullptr;
    nvrhi::Viewport targetViewport;
    dm::box2 targetBox = dm::box2(0.f, 1.f);

    nvrhi::ITexture* sourceTexture = nullptr;
    uint32_t sourceArraySlice = 0;
    uint32_t sourceMip = 0;
    dm::box2 sourceBox = dm::box2(0.f, 1.f);
    nvrhi::Format sourceFormat = nvrhi::Format::UNKNOWN;

    BlitSampler sampler = BlitSampler::Linear;
    nvrhi::BlendState::RenderTarget blendState;
    nvrhi::Color blendConstantColor = nvrhi::Color(0.f);
};

class FullscreenBlitPass
{
public:
    FullscreenBlitPass(nvrhi::IDevice* device,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory,
        const StandardSamplers& samplers);

    nvrhi::ShaderHandle fullscreenVS() const { return m_FullscreenVS; }
    nvrhi::ShaderHandle fullscreenAtOneVS() const { return m_FullscreenAtOneVS; }
    nvrhi::ShaderHandle rectVS() const { return m_RectVS; }
    nvrhi::ShaderHandle blitPS() const { return m_BlitPS; }
    nvrhi::ShaderHandle blitArrayPS() const { return m_BlitArrayPS; }
    nvrhi::ShaderHandle sharpenPS() const { return m_SharpenPS; }
    nvrhi::ShaderHandle sharpenArrayPS() const { return m_SharpenArrayPS; }
    nvrhi::BindingLayoutHandle blitBindingLayout() const { return m_BlitBindingLayout; }

    void blitTexture(nvrhi::ICommandList* commandList, const BlitParameters& params, caustica::BindingCache* bindingCache = nullptr);
    void blitTexture(nvrhi::ICommandList* commandList,
        nvrhi::IFramebuffer* targetFramebuffer,
        nvrhi::ITexture* sourceTexture,
        caustica::BindingCache* bindingCache = nullptr);

private:
    struct PsoCacheKey
    {
        nvrhi::FramebufferInfo fbinfo;
        nvrhi::IShader* shader;
        nvrhi::BlendState::RenderTarget blendState;

        bool operator==(const PsoCacheKey& other) const
        {
            return fbinfo == other.fbinfo && shader == other.shader && blendState == other.blendState;
        }

        struct Hash
        {
            size_t operator()(const PsoCacheKey& s) const
            {
                size_t hash = 0;
                nvrhi::hash_combine(hash, s.fbinfo);
                nvrhi::hash_combine(hash, s.shader);
                nvrhi::hash_combine(hash, s.blendState);
                return hash;
            }
        };
    };

    nvrhi::DeviceHandle m_Device;
    const StandardSamplers* m_Samplers = nullptr;

    nvrhi::ShaderHandle m_FullscreenVS;
    nvrhi::ShaderHandle m_FullscreenAtOneVS;
    nvrhi::ShaderHandle m_RectVS;
    nvrhi::ShaderHandle m_BlitPS;
    nvrhi::ShaderHandle m_BlitArrayPS;
    nvrhi::ShaderHandle m_SharpenPS;
    nvrhi::ShaderHandle m_SharpenArrayPS;
    nvrhi::BindingLayoutHandle m_BlitBindingLayout;

    std::unordered_map<PsoCacheKey, nvrhi::GraphicsPipelineHandle, PsoCacheKey::Hash> m_BlitPsoCache;
};

} // namespace caustica::rhi
