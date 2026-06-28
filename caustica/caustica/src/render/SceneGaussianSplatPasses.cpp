#include <render/SceneGaussianSplatPasses.h>

#include <render/WorldRenderer/PathTracingWorldRenderer.h>
#include <render/Passes/Gaussian/GaussianSplatPass.h>
#include <render/GPUSort/GPUSort.h>

#include <backend/GpuDevice.h>
#include <core/command_line.h>
#include <core/log.h>
#include <render/Core/RenderCore.h>
#include <scene/SceneGraph.h>
#include <scene/SceneManager.h>
#include <scene/Scene.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace caustica::editor
{

namespace
{
    uint32_t ResolveGaussianSplatShadowMode(const PathTracerSettings& settings)
    {
        if (!settings.GaussianSplatShadows && settings.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED)
            return GAUSSIAN_SPLAT_SHADOWS_DISABLED;

        const int requestedMode = settings.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED
            ? GAUSSIAN_SPLAT_SHADOWS_HARD
            : settings.GaussianSplatShadowsMode;
        return uint32_t(std::clamp(requestedMode, GAUSSIAN_SPLAT_SHADOWS_HARD, GAUSSIAN_SPLAT_SHADOWS_SOFT));
    }

    uint32_t ClampGaussianSplatEmissionProxyCount(int proxyCount)
    {
        return uint32_t(std::clamp(proxyCount, 0, 262144));
    }

    std::string MakeUniqueChildNodeName(const SceneGraphNode& parent, const std::string& desiredName)
    {
        const std::string baseName = desiredName.empty() ? "GaussianSplat" : desiredName;

        std::unordered_set<std::string> existingNames;
        for (size_t childIndex = 0; childIndex < parent.GetNumChildren(); childIndex++)
            existingNames.insert(parent.GetChild(childIndex)->GetName());

        if (existingNames.find(baseName) == existingNames.end())
            return baseName;

        for (uint32_t suffix = 2; ; suffix++)
        {
            std::string candidate = baseName + " (" + std::to_string(suffix) + ")";
            if (existingNames.find(candidate) == existingNames.end())
                return candidate;
        }
    }

    bool NodeSubtreeContains(SceneGraphNode* root, SceneGraphNode* candidate)
    {
        if (root == nullptr || candidate == nullptr)
            return false;

        SceneGraphWalker walker(root);
        while (walker)
        {
            if (walker.Get() == candidate)
                return true;
            walker.Next(true);
        }

        return false;
    }
}

void SceneGaussianSplatPasses::attach(caustica::GpuDevice& gpuDevice,
    SceneManager& sceneManager,
    caustica::RenderCore& renderCore,
    caustica::render::PathTracingWorldRenderer& worldRenderer,
    PathTracerSettings& settings,
    caustica::render::GaussianSplatSceneSummary& summary,
    const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
    const std::shared_ptr<caustica::CommonRenderPasses>& commonPasses)
{
    m_gpuDevice = &gpuDevice;
    m_sceneManager = &sceneManager;
    m_renderCore = &renderCore;
    m_worldRenderer = &worldRenderer;
    m_settings = &settings;
    m_summary = &summary;
    m_shaderFactory = shaderFactory;
    m_commonPasses = commonPasses;
}

void SceneGaussianSplatPasses::setOnRequestFullRebuild(std::function<void()> callback)
{
    m_onRequestFullRebuild = std::move(callback);
}

void SceneGaussianSplatPasses::sceneUnloading()
{
    m_objects.clear();
    m_emissionProxies.clear();
    m_gpuSort = nullptr;
    updateUIState();
}

void SceneGaussianSplatPasses::onSceneLoaded(const CommandLineOptions& cmdLine)
{
    loadFromSceneGraph();

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

bool SceneGaussianSplatPasses::removeObjectsUnderNode(const std::shared_ptr<caustica::SceneGraphNode>& node)
{
    bool removedGaussianSplat = false;
    auto removedBegin = std::remove_if(
        m_objects.begin(),
        m_objects.end(),
        [&node, &removedGaussianSplat](const SceneObject& object)
        {
            auto objectNode = object.node.lock();
            const bool remove = objectNode != nullptr && NodeSubtreeContains(node.get(), objectNode.get());
            removedGaussianSplat = removedGaussianSplat || remove;
            return remove;
        });
    if (removedBegin != m_objects.end())
        m_objects.erase(removedBegin, m_objects.end());

    if (removedGaussianSplat)
    {
        updateUIState();
        m_emissionProxies.clear();
        m_worldRenderer->setGaussianSplatTemporalReset(true);
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

void SceneGaussianSplatPasses::preparePass(GaussianSplatPass& pass)
{
    auto* renderTargets = m_worldRenderer->getRenderTargets();
    auto shaderDebug = m_worldRenderer->getShaderDebug();
    if (renderTargets == nullptr || shaderDebug == nullptr)
        return;

    if (m_gpuSort == nullptr)
        m_gpuSort = std::make_shared<GPUSort>(m_gpuDevice->GetDevice(), m_shaderFactory);
    m_gpuSort->CreateRenderPasses(m_commonPasses, shaderDebug);
    pass.SetGpuSort(m_gpuSort);
    pass.CreatePipeline(*renderTargets);
}

uint32_t SceneGaussianSplatPasses::totalSplatCount() const
{
    uint64_t total = 0;
    for (const auto& object : m_objects)
    {
        if (object.pass != nullptr)
            total += object.pass->GetSplatCount();
    }
    return uint32_t(std::min<uint64_t>(total, std::numeric_limits<uint32_t>::max()));
}

void SceneGaussianSplatPasses::updateUIState()
{
    m_summary->ObjectCount = uint32_t(m_objects.size());
    m_summary->SplatCount = totalSplatCount();

    m_fileNameSummary.clear();
    if (m_objects.size() == 1 && m_objects.front().pass != nullptr)
        m_fileNameSummary = m_objects.front().pass->GetSourceFileName();
    else if (!m_objects.empty())
        m_fileNameSummary = std::to_string(m_objects.size()) + " scene Gaussian Splat objects";
    m_summary->FileName = m_fileNameSummary;
}

void SceneGaussianSplatPasses::loadFromSceneGraph()
{
    m_objects.clear();
    m_emissionProxies.clear();

    if (!m_sceneManager->getScene() || !m_sceneManager->getScene()->GetSceneGraph() || !m_shaderFactory)
    {
        updateUIState();
        return;
    }

    SceneGraphWalker walker(m_sceneManager->getScene()->GetSceneGraph()->GetRootNode().get());
    while (walker)
    {
        auto splat = std::dynamic_pointer_cast<GaussianSplat>(walker->GetLeaf());
        if (splat != nullptr)
        {
            splat->loadedSplatCount = 0;
            splat->resolvedPath.clear();

            const std::filesystem::path splatPath = resolveSplatPath(*splat);
            if (splatPath.empty())
            {
                caustica::error("Gaussian Splat node '%s' has no path/file field.", walker->GetName().c_str());
            }
            else
            {
                auto pass = std::make_unique<GaussianSplatPass>(m_gpuDevice->GetDevice(), m_shaderFactory);
                if (pass->LoadFromFile(splatPath, splat->convertRdfToRub))
                {
                    splat->resolvedPath = splatPath.string();
                    splat->loadedSplatCount = pass->GetSplatCount();
                    preparePass(*pass);

                    SceneObject object;
                    object.splat = splat;
                    object.node = walker->shared_from_this();
                    object.pass = std::move(pass);
                    m_objects.push_back(std::move(object));
                }
                else
                {
                    caustica::error("Failed to load Gaussian Splat node '%s' from '%s'.",
                        walker->GetName().c_str(), splatPath.string().c_str());
                }
            }
        }

        walker.Next(true);
    }

    updateUIState();
    m_worldRenderer->setGaussianSplatTemporalReset(true);
}

bool SceneGaussianSplatPasses::attachToScene(const std::filesystem::path& fileName, bool convertRdfToRub)
{
    if (!m_sceneManager->getScene() || !m_sceneManager->getScene()->GetSceneGraph() || !m_sceneManager->getScene()->GetSceneGraph()->GetRootNode())
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

    auto pass = std::make_unique<GaussianSplatPass>(m_gpuDevice->GetDevice(), m_shaderFactory);
    if (!pass->LoadFromFile(splatPath, convertRdfToRub))
    {
        caustica::error("Failed to load Gaussian Splat file '%s'.", splatPath.string().c_str());
        return false;
    }
    if (pass->GetSplatCount() == 0)
    {
        caustica::error("Gaussian Splat file '%s' contains no splats.", splatPath.string().c_str());
        return false;
    }

    auto splat = std::make_shared<GaussianSplat>();
    splat->path = splatPath.string();
    splat->resolvedPath = splatPath.string();
    splat->convertRdfToRub = convertRdfToRub;
    splat->enabled = true;
    splat->loadedSplatCount = pass->GetSplatCount();

    auto node = std::make_shared<SceneGraphNode>();
    auto sceneGraph = m_sceneManager->getScene()->GetSceneGraph();
    auto rootNode = sceneGraph->GetRootNode();
    node->SetName(MakeUniqueChildNodeName(*rootNode, splatPath.filename().string()));
    node->SetLeaf(splat);

    constexpr double deg2rad = 3.14159265358979323846 / 180.0;
    node->SetTranslation(dm::double3(
        double(m_settings->GaussianSplatTranslation.x),
        double(m_settings->GaussianSplatTranslation.y),
        double(m_settings->GaussianSplatTranslation.z)));
    node->SetRotation(dm::rotationQuat(dm::double3(
        double(m_settings->GaussianSplatRotationEulerDeg.x) * deg2rad,
        double(m_settings->GaussianSplatRotationEulerDeg.y) * deg2rad,
        double(m_settings->GaussianSplatRotationEulerDeg.z) * deg2rad)));
    node->SetScaling(dm::double3(
        double(m_settings->GaussianSplatObjectScale.x),
        double(m_settings->GaussianSplatObjectScale.y),
        double(m_settings->GaussianSplatObjectScale.z)));

    auto attachedNode = sceneGraph->Attach(rootNode, node);
    m_sceneManager->getScene()->RefreshSceneGraph(m_gpuDevice->GetFrameIndex());

    preparePass(*pass);

    SceneObject object;
    object.splat = splat;
    object.node = attachedNode;
    object.pass = std::move(pass);
    m_objects.push_back(std::move(object));

    m_settings->EnableGaussianSplats = true;
    updateUIState();
    m_worldRenderer->setGaussianSplatTemporalReset(true);
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

SceneGaussianSplatPasses::SceneObject* SceneGaussianSplatPasses::primaryObject()
{
    for (auto& object : m_objects)
    {
        if (object.splat != nullptr && object.splat->enabled && object.pass != nullptr && object.pass->HasSplats())
            return &object;
    }
    return nullptr;
}

const SceneGaussianSplatPasses::SceneObject* SceneGaussianSplatPasses::primaryObject() const
{
    for (const auto& object : m_objects)
    {
        if (object.splat != nullptr && object.splat->enabled && object.pass != nullptr && object.pass->HasSplats())
            return &object;
    }
    return nullptr;
}

dm::float4x4 SceneGaussianSplatPasses::objectToWorld(const SceneObject& object) const
{
    auto node = object.node.lock();
    if (node == nullptr)
        return dm::float4x4::identity();

    return dm::affineToHomogeneous(node->GetLocalToWorldTransformFloat());
}

void SceneGaussianSplatPasses::preparePasses()
{
    for (auto& object : m_objects)
    {
        if (object.pass != nullptr && object.pass->HasSplats())
            preparePass(*object.pass);
    }
}

void SceneGaussianSplatPasses::buildEmissionProxyList()
{
    m_emissionProxies.clear();

    if (!isEmissionEnabled())
        return;

    const uint32_t maxProxyCount = ClampGaussianSplatEmissionProxyCount(m_settings->GaussianSplatEmissionMaxProxyCount);
    for (auto& object : m_objects)
    {
        if (object.splat == nullptr || !object.splat->enabled || object.pass == nullptr || !object.pass->HasSplats())
            continue;

        const uint32_t remainingProxyCount = maxProxyCount > m_emissionProxies.size()
            ? maxProxyCount - uint32_t(m_emissionProxies.size())
            : 0u;
        if (remainingProxyCount == 0)
            break;

        object.pass->BuildEmissionProxies(
            remainingProxyCount,
            m_settings->GaussianSplatScale,
            uint32_t(std::clamp(m_settings->GaussianSplatRtxKernelDegree, 0, 5)),
            m_settings->GaussianSplatRtxAdaptiveClamp,
            m_settings->GaussianSplatTintColor,
            m_settings->GaussianSplatAlphaCullThreshold);

        auto node = object.node.lock();
        const dm::affine3 objectToWorldTransform = node != nullptr
            ? node->GetLocalToWorldTransformFloat()
            : dm::affine3::identity();

        const float radiusScale = std::max({
            length(objectToWorldTransform.transformVector(float3(1.0f, 0.0f, 0.0f))),
            length(objectToWorldTransform.transformVector(float3(0.0f, 1.0f, 0.0f))),
            length(objectToWorldTransform.transformVector(float3(0.0f, 0.0f, 1.0f))) });

        const auto& proxies = object.pass->GetEmissionProxies();
        m_emissionProxies.reserve(m_emissionProxies.size() + proxies.size());
        for (const GaussianSplatEmissionProxy& proxy : proxies)
        {
            GaussianSplatEmissionProxy transformed = proxy;
            transformed.center = objectToWorldTransform.transformPoint(proxy.center);
            transformed.radius = proxy.radius * radiusScale;
            m_emissionProxies.push_back(transformed);
        }
    }
}

bool SceneGaussianSplatPasses::isEmissionEnabled() const
{
    return m_settings->EnableGaussianSplats
        && m_settings->GaussianSplatAsEmitter
        && m_settings->GaussianSplatEmissionIntensity > 0.0f
        && m_settings->GaussianSplatEmissionMaxProxyCount > 0;
}

bool SceneGaussianSplatPasses::objectsEmpty() const
{
    return m_objects.empty();
}

caustica::render::GaussianSplatBinding SceneGaussianSplatPasses::getPrimaryBinding() const
{
    caustica::render::GaussianSplatBinding binding;
    if (const auto* object = primaryObject())
    {
        binding.pass = object->pass.get();
        binding.objectToWorld = objectToWorld(*object);
    }
    return binding;
}

void SceneGaussianSplatPasses::renderSceneGaussianSplats(nvrhi::ICommandList* commandList,
    const caustica::PlanarView& splatView,
    RenderTargets& renderTargets,
    const GaussianSplatRenderSettings& settings,
    bool& renderedAny)
{
    for (auto& object : m_objects)
    {
        if (object.splat == nullptr || !object.splat->enabled || object.pass == nullptr || !object.pass->HasSplats())
            continue;
        GaussianSplatRenderSettings objectSettings = settings;
        objectSettings.objectToWorld = objectToWorld(object);
        object.pass->Render(commandList, splatView, m_renderCore->accelStructs().getTopLevelAS().Get(), renderTargets, objectSettings);
        renderedAny = true;
    }
}

void SceneGaussianSplatPasses::buildAccelStructs(nvrhi::ICommandList* commandList)
{
    for (auto& object : m_objects)
    {
        if (object.splat == nullptr || !object.splat->enabled || object.pass == nullptr || !object.pass->HasSplats())
            continue;

        if (ResolveGaussianSplatShadowMode(*m_settings) != GAUSSIAN_SPLAT_SHADOWS_DISABLED)
            object.pass->BuildAccelerationStructures(
                commandList,
                m_settings->GaussianSplatUseAABBs,
                m_settings->GaussianSplatUseTLASInstances,
                m_settings->GaussianSplatBlasCompaction,
                m_settings->GaussianSplatScale,
                uint32_t(std::clamp(m_settings->GaussianSplatRtxKernelDegree, 0, 5)),
                m_settings->GaussianSplatRtxAdaptiveClamp);
        else
            object.pass->ReleaseAccelerationStructures();
    }
}

} // namespace caustica::editor
