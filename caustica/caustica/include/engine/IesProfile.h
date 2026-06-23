#pragma once

#include <nvrhi/nvrhi.h>
#include <memory>
#include <filesystem>

namespace caustica
{
    class IFileSystem;
}

namespace caustica
{
    class ShaderFactory;
    class DescriptorTableManager;

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

        std::shared_ptr<caustica::ShaderFactory> m_ShaderFactory;
        std::shared_ptr<caustica::DescriptorTableManager> m_DescriptorTableManager;

    public:
        IesProfileLoader(
            nvrhi::IDevice* device,
            std::shared_ptr<caustica::ShaderFactory> shaderFactory,
            std::shared_ptr<caustica::DescriptorTableManager> descriptorTableManager);

        std::shared_ptr<IesProfile> LoadIesProfile(caustica::IFileSystem& fs, const std::filesystem::path& path);

        void BakeIesProfile(IesProfile& profile, nvrhi::ICommandList* commandList);
    };

}