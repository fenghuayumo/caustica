#pragma once

#include <rhi/rhi.h>
#include <vector>
#include <memory>

namespace caustica
{
class IBlob;
struct ImageAsset;

bool loadDDSTextureFromMemory(ImageAsset& textureInfo);

caustica::rhi::TextureHandle createDDSTextureFromMemory(
    caustica::rhi::Device* device,
    caustica::rhi::CommandList* commandList,
    std::shared_ptr<IBlob> data,
    const char* debugName = nullptr,
    bool forceSRGB = false);

std::shared_ptr<IBlob> saveStagingTextureAsDDS(caustica::rhi::Device* device, caustica::rhi::StagingTexture* stagingTexture);

} // namespace caustica
