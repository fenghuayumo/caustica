#include <engine/GpuSharedCaches.h>

#include <assets/AssetSystem.h>
#include <assets/loader/ShaderFactory.h>
#include <assets/loader/ShaderPackFileSystem.h>
#include <backend/GpuDevice.h>
#include <backend/ShaderUtils.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <render/core/BindingCache.h>
#include <render/core/BindlessTable.h>
#include <render/core/RenderDevice.h>
#include <render/WorldRenderer.h>

#include <cassert>

namespace caustica
{

GpuSharedCaches::GpuSharedCaches() = default;
GpuSharedCaches::~GpuSharedCaches() = default;
GpuSharedCaches::GpuSharedCaches(GpuSharedCaches&&) noexcept = default;
GpuSharedCaches& GpuSharedCaches::operator=(GpuSharedCaches&&) noexcept = default;

namespace
{

std::shared_ptr<ShaderFactory> CreateShaderFactory(GpuDevice& gpuDevice)
{
    const char* shaderTypeName = getShaderTypeName(gpuDevice.getGraphicsAPI());
    const std::filesystem::path appDirectory = getRuntimeDirectory();
    const std::filesystem::path shaderBinPath = appDirectory / "ShaderBin" / shaderTypeName;

    std::shared_ptr<RootFileSystem> rootFS = std::make_shared<RootFileSystem>();
    const std::filesystem::path shaderPackPath = appDirectory / (std::string("caustica.shaders.") + shaderTypeName + ".pack");
    auto shaderPackFS = std::make_shared<ShaderPackFileSystem>(shaderPackPath, "ShaderBin");
    const bool shaderPackHasCurrentLayout = shaderPackFS->hasShaderBinLayout();

    if (shaderPackFS->isOpen() && !shaderPackHasCurrentLayout)
    {
        warning("Shader pack '%s' does not match the current ShaderBin layout; falling back to loose binaries",
            shaderPackPath.string().c_str());
    }

    if (shaderPackHasCurrentLayout)
    {
        rootFS->mount("/ShaderBin", shaderPackFS);
    }
    else
    {
        rootFS->mount("/ShaderBin", shaderBinPath);
    }

    return std::make_shared<ShaderFactory>(gpuDevice.getDevice(), rootFS, "/ShaderBin");
}

} // namespace

bool GpuSharedCaches::initialize(GpuDevice& gpuDevice, AssetSystem& assetSystem)
{
    shaderFactory = CreateShaderFactory(gpuDevice);

    auto* rhiDevice = gpuDevice.getDevice();
    bindlessLayout = render::WorldRenderer::createBindlessLayout(rhiDevice);

    renderDevice = std::make_unique<render::RenderDevice>(rhiDevice, shaderFactory);
    bindingCache = std::make_unique<BindingCache>(rhiDevice);
    bindlessTable = std::make_unique<BindlessTable>(rhiDevice, bindlessLayout);
    descriptorTable = bindlessTable->getDescriptorTableManager();

    auto nativeFS = std::make_shared<NativeFileSystem>();
    assetSystem.initialize(rhiDevice, nativeFS, descriptorTable);
    textureLoader = assetSystem.getTextureLoader();
    return true;
}

void GpuSharedCaches::endFrame()
{
    if (bindlessTable)
        bindlessTable->flushDeferredFrees();
}

void GpuSharedCaches::shutdown()
{
    textureLoader.reset();
    descriptorTable.reset();
    bindlessTable.reset();
    bindingCache.reset();
    renderDevice.reset();
    shaderFactory.reset();
    bindlessLayout = nullptr;
}

render::RenderDevice& GpuSharedCaches::device()
{
    assert(renderDevice != nullptr);
    return *renderDevice;
}

const render::RenderDevice& GpuSharedCaches::device() const
{
    assert(renderDevice != nullptr);
    return *renderDevice;
}

} // namespace caustica
