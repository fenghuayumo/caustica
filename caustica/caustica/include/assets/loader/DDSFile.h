#pragma once

#include <rhi/nvrhi.h>
#include <vector>
#include <memory>

namespace caustica
{
class IBlob;
struct TextureData;

bool loadDDSTextureFromMemory(TextureData& textureInfo);

nvrhi::TextureHandle createDDSTextureFromMemory(
    nvrhi::IDevice* device,
    nvrhi::ICommandList* commandList,
    std::shared_ptr<IBlob> data,
    const char* debugName = nullptr,
    bool forceSRGB = false);

std::shared_ptr<IBlob> saveStagingTextureAsDDS(nvrhi::IDevice* device, nvrhi::IStagingTexture* stagingTexture);

} // namespace caustica
