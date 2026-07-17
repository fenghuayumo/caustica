#include <render/SceneGaussianSplatPasses.h>

#include <render/core/RenderDevice.h>
#include <render/PathTracerScenePasses.h>
#include <render/core/RenderTargets.h>

#include <backend/GpuDevice.h>
#include <core/command_line.h>
#include <core/log.h>
#include <render/core/RenderTargets.h>
#include <scene/scene_utils.h>
#include <scene/SceneObjects.h>

#include <algorithm>
#include <limits>

namespace caustica::render
{

void SceneGaussianSplatPasses::wireSession(const ScenePassWireParams& params)
{
    m_gpuDevice = &params.gpuDevice;
    m_settings = &params.settings;
    m_summary = &params.gaussianSplatsSummary;
    m_shaderFactory = params.shaderFactory;
    m_renderDevice = &params.renderDevice;
    m_onTemporalReset = params.onGaussianSplatTemporalReset;
    m_getRenderTargets = params.getRenderTargets;
    m_getShaderDebug = params.getShaderDebug;
}

void SceneGaussianSplatPasses::setOnRequestFullRebuild(std::function<void()> callback)
{
    m_onRequestFullRebuild = std::move(callback);
}

void SceneGaussianSplatPasses::bindSession(caustica::Scene* scene, std::filesystem::path scenePath)
{
    m_sessionScene = scene;
    m_sessionScenePath = std::move(scenePath);
}

void SceneGaussianSplatPasses::clearSession()
{
    m_sessionScene = nullptr;
    m_sessionScenePath.clear();
}

void SceneGaussianSplatPasses::sceneUnloading()
{
    m_objects.clear();
    m_passByEntity.clear();
}

std::filesystem::path SceneGaussianSplatPasses::resolveSplatPath(const caustica::GaussianSplat& splat) const
{
    if (splat.path.empty())
        return {};

    std::filesystem::path splatPath = splat.path;
    if (splatPath.is_absolute())
        return splatPath;

    const std::filesystem::path sceneFolder = m_sessionScenePath.parent_path();
    if (!sceneFolder.empty() && !isInlineScenePath(m_sessionScenePath))
        return sceneFolder / splatPath;

    return std::filesystem::absolute(splatPath);
}

void SceneGaussianSplatPasses::onPassLoaded(GaussianSplatPass& pass)
{
    if (!m_getRenderTargets)
        return;

    if (RenderTargets* renderTargets = m_getRenderTargets())
        pass.createPipeline(*renderTargets);
}

uint32_t SceneGaussianSplatPasses::totalSplatCount() const
{
    uint64_t total = 0;
    for (const auto& object : m_objects)
    {
        if (object.pass != nullptr)
            total += object.pass->getSplatCount();
    }
    return uint32_t(std::min<uint64_t>(total, std::numeric_limits<uint32_t>::max()));
}

GaussianSplatPass* SceneGaussianSplatPasses::findPass(ecs::Entity entity)
{
    return const_cast<GaussianSplatPass*>(
        static_cast<const SceneGaussianSplatPasses&>(*this).findPass(entity));
}

const GaussianSplatPass* SceneGaussianSplatPasses::findPass(ecs::Entity entity) const
{
    const auto it = m_passByEntity.find(uint32_t(entity));
    return it != m_passByEntity.end() ? it->second : nullptr;
}

void SceneGaussianSplatPasses::updateUIState()
{
    m_passByEntity.clear();
    m_passByEntity.reserve(m_objects.size());
    for (const SceneObject& object : m_objects)
    {
        if (ecs::isValid(object.entity) && object.pass)
            m_passByEntity[uint32_t(object.entity)] = object.pass.get();
    }

    m_summary->ObjectCount = uint32_t(m_objects.size());
    m_summary->SplatCount = totalSplatCount();

    m_fileNameSummary.clear();
    if (m_objects.size() == 1 && m_objects.front().pass != nullptr)
        m_fileNameSummary = m_objects.front().pass->getSourceFileName();
    else if (!m_objects.empty())
        m_fileNameSummary = std::to_string(m_objects.size()) + " scene Gaussian Splat objects";
    m_summary->FileName = m_fileNameSummary;
}

uint32_t SceneGaussianSplatPasses::splatCount() const
{
    return totalSplatCount();
}

uint32_t SceneGaussianSplatPasses::objectCount() const
{
    return uint32_t(m_objects.size());
}

} // namespace caustica::render
