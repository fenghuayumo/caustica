#pragma once

#include <rhi/nvrhi.h>
#include <vector>
#include <memory>

namespace caustica
{
    class IBlob;
}

namespace caustica
{
    struct TextureData;

    // Initialized the TextureInfo from the 'data' array, which must be populated with DDS data
    bool LoadDDSTextureFromMemory(TextureData& textureInfo);

    // Creates a texture based on DDS data in memory
    nvrhi::TextureHandle CreateDDSTextureFromMemory(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, std::shared_ptr<caustica::IBlob> data, const char* debugName = nullptr, bool forceSRGB = false);

    std::shared_ptr<caustica::IBlob> SaveStagingTextureAsDDS(nvrhi::IDevice* device, nvrhi::IStagingTexture* stagingTexture);
}