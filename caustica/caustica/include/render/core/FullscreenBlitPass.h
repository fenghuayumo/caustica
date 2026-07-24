#pragma once

#include <math/math.h>
#include <render/core/StandardSamplers.h>
#include <rhi/rhi.h>

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
    caustica::rhi::Framebuffer* targetFramebuffer = nullptr;
    caustica::rhi::Viewport targetViewport;
    dm::box2 targetBox = dm::box2(0.f, 1.f);

    caustica::rhi::Texture* sourceTexture = nullptr;
    uint32_t sourceArraySlice = 0;
    uint32_t sourceMip = 0;
    dm::box2 sourceBox = dm::box2(0.f, 1.f);
    caustica::rhi::Format sourceFormat = caustica::rhi::Format::UNKNOWN;

    BlitSampler sampler = BlitSampler::Linear;
    caustica::rhi::BlendState::RenderTarget blendState;
    caustica::rhi::Color blendConstantColor = caustica::rhi::Color(0.f);
};

class FullscreenBlitPass
{
public:
    FullscreenBlitPass(caustica::rhi::Device* device,
        std::shared_ptr<caustica::ShaderFactory> shaderFactory,
        const StandardSamplers& samplers);

    caustica::rhi::ShaderHandle fullscreenVS() const { return m_fullscreenVS; }
    caustica::rhi::ShaderHandle fullscreenAtOneVS() const { return m_fullscreenAtOneVS; }
    caustica::rhi::ShaderHandle rectVS() const { return m_rectVS; }
    caustica::rhi::ShaderHandle blitPS() const { return m_blitPS; }
    caustica::rhi::ShaderHandle blitArrayPS() const { return m_blitArrayPS; }
    caustica::rhi::ShaderHandle sharpenPS() const { return m_sharpenPS; }
    caustica::rhi::ShaderHandle sharpenArrayPS() const { return m_sharpenArrayPS; }
    caustica::rhi::BindingLayoutHandle blitBindingLayout() const { return m_blitBindingLayout; }

    void blitTexture(caustica::rhi::CommandList* commandList, const BlitParameters& params, caustica::BindingCache* bindingCache = nullptr);
    void blitTexture(caustica::rhi::CommandList* commandList,
        caustica::rhi::Framebuffer* targetFramebuffer,
        caustica::rhi::Texture* sourceTexture,
        caustica::BindingCache* bindingCache = nullptr);

private:
    struct PsoCacheKey
    {
        caustica::rhi::FramebufferInfo fbinfo;
        caustica::rhi::Shader* shader;
        caustica::rhi::BlendState::RenderTarget blendState;

        bool operator==(const PsoCacheKey& other) const
        {
            return fbinfo == other.fbinfo && shader == other.shader && blendState == other.blendState;
        }

        struct Hash
        {
            size_t operator()(const PsoCacheKey& s) const
            {
                size_t hash = 0;
                caustica::rhi::hash_combine(hash, s.fbinfo);
                caustica::rhi::hash_combine(hash, s.shader);
                caustica::rhi::hash_combine(hash, s.blendState);
                return hash;
            }
        };
    };

    caustica::rhi::DeviceHandle m_device;
    const StandardSamplers* m_samplers = nullptr;

    caustica::rhi::ShaderHandle m_fullscreenVS;
    caustica::rhi::ShaderHandle m_fullscreenAtOneVS;
    caustica::rhi::ShaderHandle m_rectVS;
    caustica::rhi::ShaderHandle m_blitPS;
    caustica::rhi::ShaderHandle m_blitArrayPS;
    caustica::rhi::ShaderHandle m_sharpenPS;
    caustica::rhi::ShaderHandle m_sharpenArrayPS;
    caustica::rhi::BindingLayoutHandle m_blitBindingLayout;

    std::unordered_map<PsoCacheKey, caustica::rhi::GraphicsPipelineHandle, PsoCacheKey::Hash> m_blitPsoCache;
};

} // namespace caustica::render
