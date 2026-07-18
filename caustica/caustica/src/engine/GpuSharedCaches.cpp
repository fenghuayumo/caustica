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
    const std::filesystem::path engineShaderPath = appDirectory / "ShaderPrecompiled/engine" / shaderTypeName;
    const std::filesystem::path appShaderPath = appDirectory / "ShaderPrecompiled/caustica" / shaderTypeName;
    const std::filesystem::path nrdShaderPath = appDirectory / "ShaderPrecompiled/nrd" / shaderTypeName;
    const std::filesystem::path ommShaderPath = appDirectory / "ShaderPrecompiled/omm" / shaderTypeName;

    std::shared_ptr<RootFileSystem> rootFS = std::make_shared<RootFileSystem>();
    const std::filesystem::path shaderPackPath = appDirectory / (std::string("caustica.shaders.") + shaderTypeName + ".pack");
    auto shaderPackFS = std::make_shared<ShaderPackFileSystem>(shaderPackPath, "ShaderPrecompiled");
    const bool shaderPackHasCurrentLayout = shaderPackFS->isOpen()
        && shaderPackFS->fileExists("caustica/caustica/shaders/render/misc/DebugLines_main_vs.bin")
        && shaderPackFS->fileExists("engine/fullscreen_vs.bin");

    if (shaderPackFS->isOpen() && !shaderPackHasCurrentLayout)
    {
        warning("Shader pack '%s' does not match the current shader layout; falling back to ShaderPrecompiled directories",
            shaderPackPath.string().c_str());
    }

    if (shaderPackHasCurrentLayout)
    {
        rootFS->mount("/ShaderPrecompiled", shaderPackFS);
    }
    else
    {
        rootFS->mount("/ShaderPrecompiled/engine", engineShaderPath);
        rootFS->mount("/ShaderPrecompiled/caustica", appShaderPath);
        rootFS->mount("/ShaderPrecompiled/nrd", nrdShaderPath);
        rootFS->mount("/ShaderPrecompiled/omm", ommShaderPath);
    }

    return std::make_shared<ShaderFactory>(gpuDevice.getDevice(), rootFS, "/ShaderPrecompiled");
}

} // namespace

bool GpuSharedCaches::initialize(GpuDevice& gpuDevice, AssetSystem& assetSystem)
{
    shaderFactory = CreateShaderFactory(gpuDevice);

    auto* nvrhiDevice = gpuDevice.getDevice();
    bindlessLayout = render::WorldRenderer::createBindlessLayout(nvrhiDevice);

    renderDevice = std::make_unique<render::RenderDevice>(nvrhiDevice, shaderFactory);
    bindingCache = std::make_unique<BindingCache>(nvrhiDevice);
    bindlessTable = std::make_unique<BindlessTable>(nvrhiDevice, bindlessLayout);
    descriptorTable = bindlessTable->getDescriptorTableManager();

    auto nativeFS = std::make_shared<NativeFileSystem>();
    assetSystem.initialize(nvrhiDevice, nativeFS, descriptorTable);
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
