#include <render/SceneGaussianSplatPasses.h>

#include <render/core/RenderDevice.h>
#include <render/PathTracerScenePasses.h>
#include <render/core/RenderTargets.h>

#include <backend/GpuDevice.h>
#include <core/command_line.h>
#include <core/log.h>
#include <render/core/RenderTargets.h>
#include <scene/SceneEcs.h>
#include <scene/SceneManager.h>
#include <scene/Scene.h>

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace caustica::render
{

namespace
{
    std::string MakeUniqueChildEntityName(const scene::SceneEntityWorld& entityWorld,
        ecs::Entity parent,
        const std::string& desiredName)
    {
        const std::string baseName = desiredName.empty() ? "GaussianSplat" : desiredName;

        std::unordered_set<std::string> existingNames;
        for (ecs::Entity child : entityWorld.getEntityChildren(parent))
            existingNames.insert(entityWorld.getEntityName(child));

        if (existingNames.find(baseName) == existingNames.end())
            return baseName;

        for (uint32_t suffix = 2; ; suffix++)
        {
            std::string candidate = baseName + " (" + std::to_string(suffix) + ")";
            if (existingNames.find(candidate) == existingNames.end())
                return candidate;
        }
    }

    void ApplyGaussianSplatLocalBounds(scene::SceneEntityWorld& entityWorld, ecs::Entity entity, const GaussianSplatPass& pass)
    {
        entityWorld.world().emplace<scene::LocalBoundsComponent>(
            entity, scene::LocalBoundsComponent{ pass.getLocalBounds() });
        entityWorld.refreshHierarchy(scene::PreviousTransformPolicy::PreserveExisting);
    }
}

void SceneGaussianSplatPasses::wireSession(const ScenePassWireParams& params)
{
    m_gpuDevice = &params.gpuDevice;
    m_sceneManager = &params.sceneManager;
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

void SceneGaussianSplatPasses::sceneUnloading()
{
    m_objects.clear();
}

void SceneGaussianSplatPasses::onSceneLoaded(const CommandLineOptions& cmdLine)
{
    loadFromSceneEntities();

    if (!m_initialCmdLineSplatAttached && !cmdLine.GaussianSplatFileName.empty())
    {
        m_initialCmdLineSplatAttached = true;
        (void)attachToScene(cmdLine.GaussianSplatFileName, cmdLine.GaussianSplatConvertRdfToRub);
    }
}

bool SceneGaussianSplatPasses::loadFromFile(const std::filesystem::path& fileName, bool convertRdfToRub)
{
    return attachToScene(fileName, convertRdfToRub);
}

bool SceneGaussianSplatPasses::removeObjectsUnderEntity(ecs::Entity rootEntity)
{
    if (!ecs::isValid(rootEntity))
        return false;

    const scene::SceneEntityWorld* entityWorld = m_sceneManager->getScene()
        ? m_sceneManager->getScene()->getEntityWorld()
        : nullptr;
    if (!entityWorld)
        return false;

    bool removedGaussianSplat = false;
    auto removedBegin = std::remove_if(
        m_objects.begin(),
        m_objects.end(),
        [&](const SceneObject& object)
        {
            const bool remove = ecs::isValid(object.entity)
                && entityWorld->entitySubtreeContains(rootEntity, object.entity);
            removedGaussianSplat = removedGaussianSplat || remove;
            return remove;
        });
    if (removedBegin != m_objects.end())
        m_objects.erase(removedBegin, m_objects.end());

    if (removedGaussianSplat)
    {
        updateUIState();
        if (m_onTemporalReset)
            m_onTemporalReset();
    }

    return removedGaussianSplat;
}

std::filesystem::path SceneGaussianSplatPasses::resolveSplatPath(const caustica::GaussianSplat& splat) const
{
    if (splat.path.empty())
        return {};

    std::filesystem::path splatPath = splat.path;
    if (splatPath.is_absolute())
        return splatPath;

    const std::filesystem::path sceneFolder = m_sceneManager->getCurrentScenePath().parent_path();
    if (!sceneFolder.empty() && m_sceneManager->getCurrentScenePath() != std::filesystem::path(SceneManager::inlineSceneSentinel()))
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

void SceneGaussianSplatPasses::updateUIState()
{
    m_summary->ObjectCount = uint32_t(m_objects.size());
    m_summary->SplatCount = totalSplatCount();

    m_fileNameSummary.clear();
    if (m_objects.size() == 1 && m_objects.front().pass != nullptr)
        m_fileNameSummary = m_objects.front().pass->getSourceFileName();
    else if (!m_objects.empty())
        m_fileNameSummary = std::to_string(m_objects.size()) + " scene Gaussian Splat objects";
    m_summary->FileName = m_fileNameSummary;
}

void SceneGaussianSplatPasses::loadFromSceneEntities()
{
    // Game-thread load path: walks GaussianSplatComponent, creates GPU passes, then
    // callers publish a render snapshot. Frame rendering must not call this.
    m_objects.clear();

    if (!m_sceneManager->getScene() || !m_sceneManager->getScene()->getEntityWorld() || !m_shaderFactory)
    {
        updateUIState();
        return;
    }

    auto* entityWorld = m_sceneManager->getScene()->getEntityWorld();
    entityWorld->world().each<scene::GaussianSplatComponent>(
        [&](ecs::Entity entity, scene::GaussianSplatComponent& component)
        {
            auto splat = component.splat;
            if (!splat)
                return;

            splat->loadedSplatCount = 0;
            splat->resolvedPath.clear();

            const std::filesystem::path splatPath = resolveSplatPath(*splat);
            if (splatPath.empty())
            {
                caustica::error("Gaussian Splat entity '%s' has no path/file field.",
                    entityWorld->getEntityName(entity).c_str());
                return;
            }

            auto pass = std::make_unique<GaussianSplatPass>(m_gpuDevice->getDevice(), m_shaderFactory);
            if (pass->loadFromFile(splatPath, splat->convertRdfToRub))
            {
                splat->resolvedPath = splatPath.string();
                splat->loadedSplatCount = pass->getSplatCount();
                onPassLoaded(*pass);
                ApplyGaussianSplatLocalBounds(*entityWorld, entity, *pass);

                SceneObject object;
                object.splat = splat;
                object.entity = entity;
                object.pass = std::move(pass);
                m_objects.push_back(std::move(object));
            }
            else
            {
                caustica::error("Failed to load Gaussian Splat entity '%s' from '%s'.",
                    entityWorld->getEntityName(entity).c_str(), splatPath.string().c_str());
            }
        });

    updateUIState();
    if (m_onTemporalReset)
        m_onTemporalReset();
}

bool SceneGaussianSplatPasses::attachToScene(const std::filesystem::path& fileName, bool convertRdfToRub)
{
    auto scene = m_sceneManager->getScene();
    auto* entityWorld = scene ? scene->getEntityWorld() : nullptr;
    if (!scene || !entityWorld || !ecs::isValid(entityWorld->root()))
    {
        caustica::error("Cannot load Gaussian splats before a scene is loaded.");
        return false;
    }
    if (!m_shaderFactory)
    {
        caustica::error("Cannot load Gaussian splats before the shader factory is initialized.");
        return false;
    }

    std::filesystem::path splatPath = fileName;
    if (!splatPath.is_absolute())
        splatPath = std::filesystem::absolute(splatPath);

    if (!std::filesystem::exists(splatPath))
    {
        caustica::error("Gaussian Splat file does not exist: '%s'", splatPath.string().c_str());
        return false;
    }

    auto pass = std::make_unique<GaussianSplatPass>(m_gpuDevice->getDevice(), m_shaderFactory);
    if (!pass->loadFromFile(splatPath, convertRdfToRub))
    {
        caustica::error("Failed to load Gaussian Splat file '%s'.", splatPath.string().c_str());
        return false;
    }
    if (pass->getSplatCount() == 0)
    {
        caustica::error("Gaussian Splat file '%s' contains no splats.", splatPath.string().c_str());
        return false;
    }

    auto splat = std::make_shared<GaussianSplat>();
    splat->path = splatPath.string();
    splat->resolvedPath = splatPath.string();
    splat->convertRdfToRub = convertRdfToRub;
    splat->enabled = true;
    splat->loadedSplatCount = pass->getSplatCount();

    const ecs::Entity parent = entityWorld->root();
    const std::string entityName = MakeUniqueChildEntityName(*entityWorld, parent, splatPath.filename().string());
    ecs::Entity entity = entityWorld->createEntity(entityName, parent);

    constexpr double deg2rad = 3.14159265358979323846 / 180.0;
    entityWorld->setTranslation(entity, dm::double3(
        double(m_settings->GaussianSplatTranslation.x),
        double(m_settings->GaussianSplatTranslation.y),
        double(m_settings->GaussianSplatTranslation.z)));
    entityWorld->setRotation(entity, dm::rotationQuat(dm::double3(
        double(m_settings->GaussianSplatRotationEulerDeg.x) * deg2rad,
        double(m_settings->GaussianSplatRotationEulerDeg.y) * deg2rad,
        double(m_settings->GaussianSplatRotationEulerDeg.z) * deg2rad)));
    entityWorld->setScaling(entity, dm::double3(
        double(m_settings->GaussianSplatObjectScale.x),
        double(m_settings->GaussianSplatObjectScale.y),
        double(m_settings->GaussianSplatObjectScale.z)));
    entityWorld->setGaussianSplat(entity, splat);
    scene->extractAndPublishRenderSnapshot(m_gpuDevice->getFrameIndex());

    onPassLoaded(*pass);
    ApplyGaussianSplatLocalBounds(*entityWorld, entity, *pass);

    SceneObject object;
    object.splat = splat;
    object.entity = entity;
    object.pass = std::move(pass);
    m_objects.push_back(std::move(object));

    m_settings->EnableGaussianSplats = true;
    updateUIState();
    if (m_onTemporalReset)
        m_onTemporalReset();
    if (m_onRequestFullRebuild)
        m_onRequestFullRebuild();
    return true;
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
