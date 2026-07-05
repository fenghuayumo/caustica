#include <render/Core/GBuffer.h>
#include <render/Core/FramebufferFactory.h>
#include <rhi/utils.h>

using namespace caustica::math;
using namespace caustica;
using namespace caustica::render;

// Resources

void GBufferRenderTargets::init(
    nvrhi::IDevice* device,
    uint2 size, 
    uint sampleCount,
    bool enableMotionVectors,
    bool useReverseProjection)
{
    nvrhi::TextureDesc desc;
    desc.width = size.x;
    desc.height = size.y;
    desc.initialState = nvrhi::ResourceStates::RenderTarget;
    desc.isRenderTarget = true;
    desc.useClearValue = true;
    desc.clearValue = nvrhi::Color(0.f);
    desc.sampleCount = sampleCount;
    desc.dimension = sampleCount > 1 ? nvrhi::TextureDimension::Texture2DMS : nvrhi::TextureDimension::Texture2D;
    desc.keepInitialState = true;
    desc.isTypeless = false;
    desc.isUAV = false;
    desc.mipLevels = 1;

    desc.format = nvrhi::Format::SRGBA8_UNORM;
    desc.debugName = "gBufferDiffuse";
    gBufferDiffuse = device->createTexture(desc);

    desc.format = nvrhi::Format::SRGBA8_UNORM;
    desc.debugName = "gBufferSpecular";
    gBufferSpecular = device->createTexture(desc);

    desc.format = nvrhi::Format::RGBA16_SNORM;
    desc.debugName = "gBufferNormals";
    gBufferNormals = device->createTexture(desc);

    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "gBufferEmissive";
    gBufferEmissive = device->createTexture(desc);

    const nvrhi::Format depthFormats[] = {
        nvrhi::Format::D24S8,
        nvrhi::Format::D32S8,
        nvrhi::Format::D32,
        nvrhi::Format::D16 };

    const nvrhi::FormatSupport depthFeatures = 
        nvrhi::FormatSupport::Texture |
        nvrhi::FormatSupport::DepthStencil |
        nvrhi::FormatSupport::ShaderLoad;

    desc.format = nvrhi::utils::ChooseFormat(device, depthFeatures, depthFormats, std::size(depthFormats));
    desc.isTypeless = true;
    desc.initialState = nvrhi::ResourceStates::DepthWrite;
    desc.clearValue = useReverseProjection ? nvrhi::Color(0.f) : nvrhi::Color(1.f);
    desc.debugName = "GBufferDepth";
    depth = device->createTexture(desc);

    desc.isTypeless = false;
    desc.format = nvrhi::Format::RG16_FLOAT;
    desc.initialState = nvrhi::ResourceStates::RenderTarget;
    desc.debugName = "GBufferMotionVectors";
    desc.clearValue = nvrhi::Color(0.f);
    if (!enableMotionVectors)
    {
        desc.width = 1;
        desc.height = 1;
    }
    motionVectors = device->createTexture(desc);

    gBufferFramebuffer = std::make_shared<FramebufferFactory>(device);
    gBufferFramebuffer->renderTargets = {
        gBufferDiffuse,
        gBufferSpecular,
        gBufferNormals,
        gBufferEmissive };

    if (enableMotionVectors)
        gBufferFramebuffer->renderTargets.push_back(motionVectors);

    gBufferFramebuffer->depthTarget = depth;

    m_size = size;
    m_sampleCount = sampleCount;
    m_useReverseProjection = useReverseProjection;
}

void GBufferRenderTargets::clear(nvrhi::ICommandList* commandList)
{
    const nvrhi::FormatInfo& depthFormatInfo = nvrhi::getFormatInfo(depth->getDesc().format);

    float depthClearValue = m_useReverseProjection ? 0.f : 1.f;
    commandList->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true, depthClearValue, depthFormatInfo.hasStencil, 0);
    commandList->clearTextureFloat(gBufferDiffuse, nvrhi::AllSubresources, nvrhi::Color(0.f));
    commandList->clearTextureFloat(gBufferSpecular, nvrhi::AllSubresources, nvrhi::Color(0.f));
    commandList->clearTextureFloat(gBufferNormals, nvrhi::AllSubresources, nvrhi::Color(0.f));
    commandList->clearTextureFloat(gBufferEmissive, nvrhi::AllSubresources, nvrhi::Color(0.f));
    commandList->clearTextureFloat(motionVectors, nvrhi::AllSubresources, nvrhi::Color(0.f));
}
