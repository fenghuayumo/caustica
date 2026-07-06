#pragma once

#include <backend/IDescriptorTableManager.h>
#include <rhi/nvrhi.h>
#include <memory>
#include <filesystem>

namespace caustica
{
class IFileSystem;
class ShaderFactory;
class IDescriptorTableManager;

struct IesProfile
{
    std::string name;
    std::vector<float> rawData;
    nvrhi::TextureHandle texture;
    int textureIndex;
};

class IesProfileLoader
{
    nvrhi::DeviceHandle m_Device;
    nvrhi::ShaderHandle m_ComputeShader;
    nvrhi::ComputePipelineHandle m_ComputePipeline;
    nvrhi::BindingLayoutHandle m_BindingLayout;

    std::shared_ptr<ShaderFactory> m_ShaderFactory;
    std::shared_ptr<IDescriptorTableManager> m_DescriptorTableManager;

public:
    IesProfileLoader(
        nvrhi::IDevice* device,
        std::shared_ptr<ShaderFactory> shaderFactory,
        std::shared_ptr<IDescriptorTableManager> descriptorTableManager);

    std::shared_ptr<IesProfile> loadIesProfile(IFileSystem& fs, const std::filesystem::path& path);
    void bakeIesProfile(IesProfile& profile, nvrhi::ICommandList* commandList);
};

} // namespace caustica
