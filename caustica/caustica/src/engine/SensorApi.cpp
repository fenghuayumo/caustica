#include <engine/SensorApi.h>

#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/GpuSharedCaches.h>
#include <engine/RenderSessionApi.h>
#include <engine/ResolvedActiveCamera.h>
#include <engine/SceneQuery.h>
#include <engine/internal/WorldRendererAccess.h>
#include <render/WorldRenderer.h>
#include <render/core/PathTracerSettings.h>
#include <render/core/RenderDevice.h>
#include <render/core/RenderTargets.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneEcs.h>
#include <scene/SceneRenderExtract.h>
#include <scene/SceneSemanticIds.h>
#include <core/log.h>
#include <math/float.h>
#include <rhi/rhi.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace caustica
{
namespace
{

std::string NormalizeAovToken(std::string name)
{
    for (char& ch : name)
    {
        if (ch == '-' || ch == ' ')
            ch = '_';
        else
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return name;
}

void RequestSensorReset(App& app)
{
    if (PathTracerSettings* cfg = settings(app))
    {
        cfg->ResetAccumulation = true;
        cfg->ResetRealtimeCaches = true;
    }
}

RenderProductRegistry& Registry(App& app)
{
    if (auto* existing = app.tryResource<RenderProductRegistry>())
        return *existing;
    return app.emplaceResource<RenderProductRegistry>();
}

uint32_t CameraHistoryKey(const scene::ActiveCameraRenderProxy& camera)
{
    return static_cast<uint32_t>(camera.sourceEntity);
}

void PrepareSensorCameraHistory(
    App& app,
    const scene::ActiveCameraRenderProxy& currentCamera)
{
    RenderProductRegistry& registry = Registry(app);
    const auto previous = registry.previousCameras.find(CameraHistoryKey(currentCamera));
    // The first capture of a product deliberately uses its current pose as its
    // previous pose. This produces zero camera motion instead of borrowing the
    // previous view from an unrelated sensor.
    registry.pendingPreviousCamera = previous != registry.previousCameras.end()
        ? previous->second
        : currentCamera;
}

void CommitSensorCameraHistory(
    App& app,
    const scene::ActiveCameraRenderProxy& currentCamera)
{
    if (currentCamera.valid)
        Registry(app).previousCameras[CameraHistoryKey(currentCamera)] = currentCamera;
}

bool CopyTexturePacked(
    caustica::rhi::Device* device,
    render::RenderDevice* renderDevice,
    caustica::rhi::Texture* texture,
    caustica::rhi::ResourceStates textureState,
    bool convertToRgba8,
    std::vector<uint8_t>& outBytes,
    uint32_t& outWidth,
    uint32_t& outHeight,
    uint32_t& outBytesPerPixel)
{
    if (!device || !texture)
        return false;

    caustica::rhi::TextureDesc desc = texture->getDesc();
    caustica::rhi::TextureHandle tempTexture = texture;
    caustica::rhi::FramebufferHandle tempFramebuffer;

    caustica::rhi::CommandListHandle commandList = device->createCommandList();
    if (!commandList || !commandList->open())
        return false;

    if (textureState != caustica::rhi::ResourceStates::Unknown)
    {
        commandList->beginTrackingTextureState(
            texture, caustica::rhi::TextureSubresourceSet(0, 1, 0, 1), textureState);
    }

    if (convertToRgba8)
    {
        switch (desc.format)
        {
        case caustica::rhi::Format::RGBA8_UNORM:
        case caustica::rhi::Format::SRGBA8_UNORM:
            break;
        default:
            if (!renderDevice)
            {
                commandList->close();
                return false;
            }
            desc.format = caustica::rhi::Format::RGBA8_UNORM;
            desc.isRenderTarget = true;
            desc.isUAV = false;
            desc.isTypeless = false;
            desc.initialState = caustica::rhi::ResourceStates::RenderTarget;
            desc.keepInitialState = true;
            desc.debugName = "SensorRgb Convert";
            tempTexture = device->createTexture(desc);
            tempFramebuffer = device->createFramebuffer(
                caustica::rhi::FramebufferDesc().addColorAttachment(tempTexture));
            renderDevice->blit().blitTexture(commandList, tempFramebuffer, texture);
            break;
        }
    }

    caustica::rhi::TextureDesc stagingDesc = tempTexture->getDesc();
    stagingDesc.isRenderTarget = false;
    stagingDesc.isUAV = false;
    stagingDesc.isTypeless = false;
    stagingDesc.initialState = caustica::rhi::ResourceStates::CopyDest;
    stagingDesc.keepInitialState = true;
    stagingDesc.debugName = "SensorAov Staging";

    caustica::rhi::StagingTextureHandle stagingTexture =
        device->createStagingTexture(stagingDesc, caustica::rhi::CpuAccessMode::Read);
    if (!stagingTexture)
    {
        commandList->close();
        return false;
    }

    commandList->copyTexture(
        stagingTexture, caustica::rhi::TextureSlice(), tempTexture, caustica::rhi::TextureSlice());

    if (textureState != caustica::rhi::ResourceStates::Unknown)
    {
        commandList->setTextureState(
            texture, caustica::rhi::TextureSubresourceSet(0, 1, 0, 1), textureState);
        commandList->commitBarriers();
    }

    commandList->close();
    device->executeCommandList(commandList);
    if (!device->waitForIdle())
        return false;

    size_t rowPitch = 0;
    const uint8_t* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(
        stagingTexture, caustica::rhi::TextureSlice(), caustica::rhi::CpuAccessMode::Read, &rowPitch));
    if (!mapped)
        return false;

    const caustica::rhi::FormatInfo& info = caustica::rhi::getFormatInfo(stagingDesc.format);
    const uint32_t bytesPerPixel = info.bytesPerBlock;
    outWidth = stagingDesc.width;
    outHeight = stagingDesc.height;
    outBytesPerPixel = bytesPerPixel;
    outBytes.resize(size_t(outWidth) * size_t(outHeight) * size_t(bytesPerPixel));
    const size_t dstStride = size_t(outWidth) * size_t(bytesPerPixel);
    for (uint32_t row = 0; row < outHeight; ++row)
    {
        std::memcpy(
            outBytes.data() + size_t(row) * dstStride,
            mapped + size_t(row) * rowPitch,
            dstStride);
    }
    device->unmapStagingTexture(stagingTexture);
    return true;
}

bool FillRgb(
    App& app,
    caustica::rhi::Device* device,
    render::RenderDevice* renderDevice,
    SensorOutput& output)
{
    caustica::rhi::Texture* texture = ldrColorTexture(app);
    if (!texture)
        return false;

    std::vector<uint8_t> bytes;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bpp = 0;
    if (!CopyTexturePacked(
            device,
            renderDevice,
            texture,
            caustica::rhi::ResourceStates::ShaderResource,
            true,
            bytes,
            width,
            height,
            bpp))
        return false;

    if (bpp != 4)
        return false;

    output.rgb = std::move(bytes);
    if (output.width == 0 || output.height == 0)
    {
        output.width = width;
        output.height = height;
    }
    output.aovs |= uint32_t(Aov::Rgb);
    return true;
}

bool FillGeometryAovs(
    App& app,
    caustica::rhi::Device* device,
    uint32_t requested,
    SensorOutput& output)
{
    render::WorldRenderer* renderer = worldRenderer(app);
    RenderTargets* targets = renderer ? renderer->getRenderTargets() : nullptr;
    if (!targets)
        return false;

    const bool wantDepth = hasAov(requested, Aov::Depth);
    const bool wantNormal = hasAov(requested, Aov::Normal);
    const bool wantInstance = hasAov(requested, Aov::InstanceId);
    const bool wantSemantic = hasAov(requested, Aov::SemanticId);
    const bool wantMotion = hasAov(requested, Aov::MotionVector);

    if (wantDepth || wantNormal)
    {
        std::vector<uint8_t> bytes;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t bpp = 0;
        if (CopyTexturePacked(
                device,
                nullptr,
                targets->sensorNormalDepth,
                caustica::rhi::ResourceStates::UnorderedAccess,
                false,
                bytes,
                width,
                height,
                bpp)
            && bpp == 16)
        {
            const size_t pixels = size_t(width) * size_t(height);
            if (wantDepth)
                output.depth.resize(pixels, 0.f);
            if (wantNormal)
                output.normal.resize(pixels * 3u, 0.f);
            const float* src = reinterpret_cast<const float*>(bytes.data());
            for (size_t i = 0; i < pixels; ++i)
            {
                if (wantNormal)
                {
                    output.normal[i * 3u + 0] = src[i * 4u + 0];
                    output.normal[i * 3u + 1] = src[i * 4u + 1];
                    output.normal[i * 3u + 2] = src[i * 4u + 2];
                }
                if (wantDepth)
                    output.depth[i] = src[i * 4u + 3];
            }
            if (output.width == 0 || output.height == 0)
            {
                output.width = width;
                output.height = height;
            }
            if (wantDepth)
                output.aovs |= uint32_t(Aov::Depth);
            if (wantNormal)
                output.aovs |= uint32_t(Aov::Normal);
        }
    }

    if (wantInstance || wantSemantic)
    {
        std::vector<uint8_t> bytes;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t bpp = 0;
        if (CopyTexturePacked(
                device,
                nullptr,
                targets->sensorIds,
                caustica::rhi::ResourceStates::UnorderedAccess,
                false,
                bytes,
                width,
                height,
                bpp)
            && bpp == 8)
        {
            const size_t pixels = size_t(width) * size_t(height);
            if (wantInstance)
                output.instanceId.resize(pixels, 0u);
            if (wantSemantic)
                output.semanticId.resize(pixels, 0u);
            const uint32_t* src = reinterpret_cast<const uint32_t*>(bytes.data());
            for (size_t i = 0; i < pixels; ++i)
            {
                if (wantInstance)
                    output.instanceId[i] = src[i * 2u + 0];
                if (wantSemantic)
                    output.semanticId[i] = src[i * 2u + 1];
            }
            if (output.width == 0 || output.height == 0)
            {
                output.width = width;
                output.height = height;
            }
            if (wantInstance)
                output.aovs |= uint32_t(Aov::InstanceId);
            if (wantSemantic)
                output.aovs |= uint32_t(Aov::SemanticId);
        }
    }

    if (wantMotion)
    {
        std::vector<uint8_t> bytes;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t bpp = 0;
        if (CopyTexturePacked(
                device,
                nullptr,
                targets->screenMotionVectors,
                caustica::rhi::ResourceStates::UnorderedAccess,
                false,
                bytes,
                width,
                height,
                bpp)
            && bpp == 8)
        {
            const size_t pixels = size_t(width) * size_t(height);
            output.motionVector.resize(pixels * 2u, 0.f);
            const auto* src = reinterpret_cast<const math::float16_t4*>(bytes.data());
            for (size_t i = 0; i < pixels; ++i)
            {
                const math::float4 v = math::float16ToFloat32x4(src[i]);
                output.motionVector[i * 2u + 0] = v.x;
                output.motionVector[i * 2u + 1] = v.y;
            }
            if (output.width == 0 || output.height == 0)
            {
                output.width = width;
                output.height = height;
            }
            output.aovs |= uint32_t(Aov::MotionVector);
        }
    }

    return output.aovs != 0u;
}

bool ApplyProductCamera(App& app, ecs::Entity camera)
{
    auto* resolved = app.tryResource<ResolvedActiveCamera>();
    if (!resolved)
        return false;

    if (!ecs::isValid(camera))
        return true;

    scene::SceneEntityWorld* ew = entityWorld(app);
    if (!ew)
        return false;

    const auto* camComp = scene::tryGetCamera(ew->world(), camera);
    const auto* global = ew->world().tryGet<scene::GlobalTransformComponent>(camera);
    if (!camComp || !global || !scene::isPerspectiveCamera(*camComp))
        return false;

    const scene::CameraRenderProxy proxy =
        scene::makeCameraRenderProxy(camera, *camComp, *global);
    scene::fillActiveCameraFromPerspectiveProxy(
        proxy, resolved->camera.selectedCameraIndex, resolved->camera);
    return true;
}

ecs::Entity ActiveResolvedCamera(const App& app)
{
    const auto* resolved = app.tryResource<ResolvedActiveCamera>();
    if (!resolved || !resolved->camera.valid)
        return ecs::NullEntity;
    return resolved->camera.sourceEntity;
}

SensorOutput ReadCurrentView(App& app, const RenderProductDesc& product)
{
    SensorOutput output;
    output.name = product.name;
    output.camera = ecs::isValid(product.camera) ? product.camera : ActiveResolvedCamera(app);

    GpuDevice* gpu = app.getGpuDevice();
    caustica::rhi::Device* device = gpu ? gpu->getDevice() : nullptr;
    if (!device)
        return output;

    app.waitForRenderThreadIdle();
    if (!device->waitForIdle())
        return output;

    auto* caches = gpuSharedCaches(app);
    render::RenderDevice* renderDevice =
        (caches && caches->renderDevice) ? caches->renderDevice.get() : nullptr;

    if (hasAov(product.aovs, Aov::Rgb))
        FillRgb(app, device, renderDevice, output);
    FillGeometryAovs(app, device, product.aovs, output);
    return output;
}

} // namespace

uint32_t parseAovMask(const std::vector<std::string>& names)
{
    uint32_t mask = 0;
    for (const std::string& raw : names)
    {
        const std::string name = NormalizeAovToken(raw);
        if (name == "rgb" || name == "color")
            mask |= uint32_t(Aov::Rgb);
        else if (name == "depth" || name == "z")
            mask |= uint32_t(Aov::Depth);
        else if (name == "normal" || name == "normals")
            mask |= uint32_t(Aov::Normal);
        else if (name == "instance" || name == "instance_id" || name == "segmentation" || name == "seg")
            mask |= uint32_t(Aov::InstanceId);
        else if (name == "semantic" || name == "semantic_id")
            mask |= uint32_t(Aov::SemanticId);
        else if (name == "motion" || name == "motion_vector" || name == "motion_vectors")
            mask |= uint32_t(Aov::MotionVector);
        else if (name == "all")
            mask |= uint32_t(Aov::All);
    }
    return mask;
}

std::string aovName(Aov aov)
{
    switch (aov)
    {
    case Aov::Rgb: return "rgb";
    case Aov::Depth: return "depth";
    case Aov::Normal: return "normal";
    case Aov::InstanceId: return "instance_id";
    case Aov::SemanticId: return "semantic_id";
    case Aov::MotionVector: return "motion_vector";
    case Aov::None: return "none";
    default: return "all";
    }
}

bool addRenderProduct(App& app, RenderProductDesc product)
{
    if (product.name.empty())
        return false;
    if (product.aovs == 0u)
        product.aovs = uint32_t(Aov::All);

    auto& registry = Registry(app);
    for (RenderProductDesc& existing : registry.products)
    {
        if (existing.name == product.name)
        {
            existing = std::move(product);
            return true;
        }
    }
    registry.products.push_back(std::move(product));
    return true;
}

bool removeRenderProduct(App& app, std::string_view name)
{
    auto* registry = app.tryResource<RenderProductRegistry>();
    if (!registry)
        return false;
    const auto it = std::remove_if(
        registry->products.begin(),
        registry->products.end(),
        [&](const RenderProductDesc& product) { return product.name == name; });
    if (it == registry->products.end())
        return false;
    registry->products.erase(it, registry->products.end());
    return true;
}

void clearRenderProducts(App& app)
{
    if (auto* registry = app.tryResource<RenderProductRegistry>())
    {
        registry->products.clear();
        registry->previousCameras.clear();
        registry->pendingPreviousCamera.reset();
    }
}

std::vector<RenderProductDesc> renderProducts(const App& app)
{
    if (const auto* registry = app.tryResource<RenderProductRegistry>())
        return registry->products;
    return {};
}

bool setEntitySemanticLabel(
    App& app,
    ecs::Entity entity,
    uint32_t instanceId,
    uint32_t semanticId,
    std::string semanticLabel)
{
    ecs::World* world = sceneEcs(app);
    if (!world)
        return false;
    return scene::setSemanticLabel(*world, entity, instanceId, semanticId, std::move(semanticLabel));
}

uint32_t entityInstanceId(const App& app, ecs::Entity entity)
{
    const ecs::World* world = sceneEcs(app);
    if (!world)
        return 0u;
    return scene::resolveInstanceId(*world, entity);
}

uint32_t entitySemanticId(const App& app, ecs::Entity entity)
{
    const ecs::World* world = sceneEcs(app);
    if (!world)
        return 0u;
    return scene::resolveSemanticId(*world, entity);
}

std::string entitySemanticLabel(const App& app, ecs::Entity entity)
{
    const ecs::World* world = sceneEcs(app);
    if (!world)
        return {};
    return scene::resolveSemanticLabel(*world, entity);
}

std::optional<SensorOutput> readSensorOutput(App& app, uint32_t aovs)
{
    if (aovs == 0u)
        aovs = uint32_t(Aov::All);
    RenderProductDesc product;
    product.name = "active";
    product.camera = ActiveResolvedCamera(app);
    product.aovs = aovs;
    SensorOutput output = ReadCurrentView(app, product);
    if (output.width == 0 || output.height == 0)
        return std::nullopt;
    return output;
}

std::vector<SensorOutput> captureSensorOutputs(App& app)
{
    std::vector<RenderProductDesc> products = renderProducts(app);
    if (products.empty())
    {
        RenderProductDesc fallback;
        fallback.name = "active";
        fallback.camera = ActiveResolvedCamera(app);
        fallback.aovs = uint32_t(Aov::All);
        products.push_back(std::move(fallback));
    }

    auto* resolved = app.tryResource<ResolvedActiveCamera>();
    const scene::ActiveCameraRenderProxy savedCamera =
        resolved ? resolved->camera : scene::ActiveCameraRenderProxy{};
    const ecs::Entity originalCamera = savedCamera.sourceEntity;
    ecs::Entity gpuCamera = originalCamera;
    bool renderedExtra = false;

    app.waitForRenderThreadIdle();

    std::vector<SensorOutput> outputs;
    outputs.reserve(products.size());
    for (const RenderProductDesc& product : products)
    {
        const ecs::Entity wanted =
            ecs::isValid(product.camera) ? product.camera : originalCamera;
        if (wanted != gpuCamera)
        {
            if (!ApplyProductCamera(app, wanted))
            {
                warning(
                    "SensorApi: RenderProduct '%s' is not a perspective scene camera; skipped.",
                    product.name.c_str());
                continue;
            }
            RequestSensorReset(app);
            if (resolved)
                PrepareSensorCameraHistory(app, resolved->camera);
            if (!app.extractAndRenderFrozenFrame())
            {
                warning("SensorApi: frozen capture failed for RenderProduct '%s'.", product.name.c_str());
                if (auto* registry = app.tryResource<RenderProductRegistry>())
                    registry->pendingPreviousCamera.reset();
                if (resolved)
                    resolved->camera = savedCamera;
                continue;
            }
            gpuCamera = wanted;
            renderedExtra = true;
        }

        RenderProductDesc desc = product;
        desc.camera = wanted;
        outputs.push_back(ReadCurrentView(app, desc));
        if (resolved)
            CommitSensorCameraHistory(app, resolved->camera);
    }

    if (renderedExtra && resolved)
    {
        resolved->camera = savedCamera;
        if (gpuCamera != originalCamera)
        {
            RequestSensorReset(app);
            PrepareSensorCameraHistory(app, savedCamera);
            if (!app.extractAndRenderFrozenFrame())
            {
                warning("SensorApi: failed to restore the original camera after extra-view capture.");
                if (auto* registry = app.tryResource<RenderProductRegistry>())
                    registry->pendingPreviousCamera.reset();
            }
            else
                CommitSensorCameraHistory(app, savedCamera);
        }
        RequestSensorReset(app);
    }

    return outputs;
}

} // namespace caustica
