#pragma once

#include <backend/IDescriptorTableManager.h>
#include <rhi/rhi.h>
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
    caustica::rhi::TextureHandle texture;
    int textureIndex;
};

class IesProfileLoader
{
    caustica::rhi::DeviceHandle m_Device;
    caustica::rhi::ShaderHandle m_ComputeShader;
    caustica::rhi::ComputePipelineHandle m_ComputePipeline;
    caustica::rhi::BindingLayoutHandle m_BindingLayout;

    std::shared_ptr<ShaderFactory> m_ShaderFactory;
    std::shared_ptr<IDescriptorTableManager> m_DescriptorTableManager;

public:
    IesProfileLoader(
        caustica::rhi::Device* device,
        std::shared_ptr<ShaderFactory> shaderFactory,
        std::shared_ptr<IDescriptorTableManager> descriptorTableManager);

    std::shared_ptr<IesProfile> loadIesProfile(IFileSystem& fs, const std::filesystem::path& path);
    void bakeIesProfile(IesProfile& profile, caustica::rhi::CommandList* commandList);
};

} // namespace caustica
