#pragma once

#include <math/math.h>
#include <render/Core/StandardSamplers.h>
#include <rhi/nvrhi.h>

#include <memory>
#include <unordered_map>

namespace caustica
{
class BindingCache;
class ShaderFactory;
}

namespace caustica::render
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

    nvrhi::ShaderHandle fullscreenVS() const { return m_fullscreenVS; }
    nvrhi::ShaderHandle fullscreenAtOneVS() const { return m_fullscreenAtOneVS; }
    nvrhi::ShaderHandle rectVS() const { return m_rectVS; }
    nvrhi::ShaderHandle blitPS() const { return m_blitPS; }
    nvrhi::ShaderHandle blitArrayPS() const { return m_blitArrayPS; }
    nvrhi::ShaderHandle sharpenPS() const { return m_sharpenPS; }
    nvrhi::ShaderHandle sharpenArrayPS() const { return m_sharpenArrayPS; }
    nvrhi::BindingLayoutHandle blitBindingLayout() const { return m_blitBindingLayout; }

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

    nvrhi::DeviceHandle m_device;
    const StandardSamplers* m_samplers = nullptr;

    nvrhi::ShaderHandle m_fullscreenVS;
    nvrhi::ShaderHandle m_fullscreenAtOneVS;
    nvrhi::ShaderHandle m_rectVS;
    nvrhi::ShaderHandle m_blitPS;
    nvrhi::ShaderHandle m_blitArrayPS;
    nvrhi::ShaderHandle m_sharpenPS;
    nvrhi::ShaderHandle m_sharpenArrayPS;
    nvrhi::BindingLayoutHandle m_blitBindingLayout;

    std::unordered_map<PsoCacheKey, nvrhi::GraphicsPipelineHandle, PsoCacheKey::Hash> m_blitPsoCache;
};

} // namespace caustica::render
