#include "SceneContentEditor.h"

#include "common/LocalConfig.h"
#include <EditorUI.h>

#include <assets/AssetSystem.h>
#include <assets/RuntimeMeshLoadTypes.h>
#include <core/log.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneRayTracingResources.h>
#include <render/Core/RenderSceneTypeFactory.h>
#include <render/Core/SceneMeshEditing.h>
#include <scene/SceneGraph.h>
#include <scene/SceneManager.h>
#include <scene/SceneRuntimeMutation.h>
#include <scene/loader/RuntimeMeshLoader.h>

#include <algorithm>
#include <cctype>

namespace caustica::editor
{

namespace
{
    constexpr const char* c_InlineSceneSentinel = "__CAUSTICA_INLINE_SCENE_JSON__";

    std::filesystem::path RuntimeMeshTextureSearchDirectory(const std::filesystem::path& currentScenePath)
    {
        if (currentScenePath.empty() || currentScenePath == std::filesystem::path(c_InlineSceneSentinel))
            return {};

        return currentScenePath.parent_path();
    }

    caustica::RuntimeSceneMutationCallbacks MakeMutationCallbacks()
    {
        return caustica::RuntimeSceneMutationCallbacks{
            .PostMaterialLoad = [](caustica::Material& material) { LocalConfig::PostMaterialLoad(material); },
        };
    }

    caustica::RuntimeMeshLoadParams MakeRuntimeMeshLoadParams(SceneManager* sceneManager, TextureLoader* textureLoader)
    {
        return caustica::RuntimeMeshLoadParams{
            .TextureCache = textureLoader,
            .SceneTypes = std::make_shared<caustica::render::RenderSceneTypeFactory>(),
            .TextureSearchDirectory = sceneManager
                ? RuntimeMeshTextureSearchDirectory(sceneManager->getCurrentScenePath())
                : std::filesystem::path{},
        };
    }

    caustica::SetSceneMeshVerticesParams MakeMeshEditParams(const SceneContentEditor::Context& ctx)
    {
        return caustica::SetSceneMeshVerticesParams{
            .device = ctx.device ? ctx.device() : nullptr,
            .scene = ctx.sceneManager ? ctx.sceneManager->getScene() : nullptr,
            .frameIndex = ctx.frameIndex ? ctx.frameIndex() : 0,
            .resetAccumulation = ctx.settings ? &ctx.settings->ResetAccumulation : nullptr,
            .requestMeshAccelRebuild = [&ctx](const std::shared_ptr<caustica::MeshInfo>& dirtyMesh) {
                if (ctx.rayTracingResources)
                    ctx.rayTracingResources->requestMeshAccelRebuild(dirtyMesh);
            },
        };
    }
}

SceneContentEditor::SceneContentEditor(Context context)
    : m_ctx(std::move(context))
{
}

void SceneContentEditor::updateContext(Context context)
{
    m_ctx = std::move(context);
}

void SceneContentEditor::handleDroppedFiles(std::vector<std::string>& pendingFiles)
{
    if (pendingFiles.empty())
        return;

    auto files = std::move(pendingFiles);
    pendingFiles.clear();

    for (const auto& filePath : files)
    {
        std::filesystem::path path(filePath);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });

        if (ext == ".ply")
        {
            caustica::info("Drag-drop: loading Gaussian Splat file '%s'", filePath.c_str());
            if (m_ctx.loadGaussianSplat && m_ctx.loadGaussianSplat(path))
            {
                caustica::info("Gaussian Splat loaded successfully: %d splats across %d objects",
                    m_ctx.gaussianSplatCount ? int(m_ctx.gaussianSplatCount()) : 0,
                    m_ctx.gaussianSplatObjectCount ? int(m_ctx.gaussianSplatObjectCount()) : 0);
            }
            else
                caustica::error("Failed to load Gaussian Splat file '%s'", filePath.c_str());
        }
        else if (ext == ".gltf" || ext == ".glb" || ext == ".obj")
        {
            caustica::info("Drag-drop: loading mesh file '%s'", filePath.c_str());
            if (loadMeshFile(path))
                caustica::info("Mesh file loaded successfully: '%s'", filePath.c_str());
            else
                caustica::error("Failed to load mesh file '%s'", filePath.c_str());
        }
        else
        {
            caustica::warning("Drag-drop: unsupported file type '%s' (supported: .ply, .gltf, .glb, .obj)", ext.c_str());
        }
    }
}

bool SceneContentEditor::importMeshFile(const std::filesystem::path& filePath,
    caustica::RuntimeMeshLoadResult (*loadFile)(const caustica::RuntimeMeshLoadParams&, const std::filesystem::path&))
{
    if (!m_ctx.sceneManager || !m_ctx.textureLoader || !m_ctx.frameIndex)
        return false;

    const auto loadResult = loadFile(
        MakeRuntimeMeshLoadParams(m_ctx.sceneManager, m_ctx.textureLoader),
        filePath);
    if (!loadResult)
        return false;

    const auto importedRoot = caustica::AttachRuntimeSceneImport(
        m_ctx.sceneManager->getScene(),
        *loadResult.ImportResult,
        m_ctx.frameIndex(),
        MakeMutationCallbacks());
    if (!importedRoot)
        return false;

    finalizeRuntimeSceneMutation(nullptr);
    return true;
}

bool SceneContentEditor::loadMeshFile(const std::filesystem::path& filePath)
{
    return importMeshFile(filePath, caustica::LoadRuntimeMeshFile);
}

bool SceneContentEditor::loadGltfMeshFile(const std::filesystem::path& filePath)
{
    return importMeshFile(filePath, caustica::LoadRuntimeGltfMeshFile);
}

bool SceneContentEditor::loadObjMeshFile(const std::filesystem::path& filePath)
{
    return importMeshFile(filePath, caustica::LoadRuntimeObjMeshFile);
}

void SceneContentEditor::finalizeRuntimeSceneMutation(const std::shared_ptr<caustica::SceneGraphNode>& importedRoot)
{
    if (!m_ctx.sceneManager || !m_ctx.lightingPasses)
        return;

    if (importedRoot)
    {
        caustica::FinalizeRuntimeSceneMutation(
            m_ctx.sceneManager->getScene(),
            importedRoot,
            m_ctx.frameIndex ? m_ctx.frameIndex() : 0,
            MakeMutationCallbacks());
    }

    if (auto scene = m_ctx.sceneManager->getScene())
        m_ctx.lightingPasses->notifyBakersSceneReloaded(*scene);

    requestFullRebuild();
}

bool SceneContentEditor::deleteSceneNode(const std::shared_ptr<caustica::SceneGraphNode>& node)
{
    if (!m_ctx.sceneManager || !m_ctx.lightingPasses || !m_ctx.device || !m_ctx.frameIndex)
        return false;

    auto scene = m_ctx.sceneManager->getScene();
    auto sceneGraph = scene ? scene->GetSceneGraph() : nullptr;
    auto rootNode = sceneGraph ? sceneGraph->GetRootNode() : nullptr;

    if (!caustica::DeleteRuntimeSceneNode(caustica::DeleteRuntimeSceneNodeParams{
            .SceneInstance = scene,
            .Node = node,
            .Device = m_ctx.device(),
            .FrameIndex = m_ctx.frameIndex(),
            .BeforeDetach = [this](const std::shared_ptr<caustica::SceneGraphNode>& deletedNode) {
                if (m_ctx.gaussianSplatPasses)
                    m_ctx.gaussianSplatPasses->removeObjectsUnderNode(deletedNode);
            },
        }))
    {
        return false;
    }

    m_ctx.lightingPasses->resyncLightsFromSceneGraph(*sceneGraph);

    if (m_ctx.editor && m_ctx.editor->TogglableNodes != nullptr)
    {
        m_ctx.editor->TogglableNodes->clear();
        UpdateTogglableNodes(*m_ctx.editor->TogglableNodes, rootNode.get());
    }

    if (m_ctx.editor)
    {
        m_ctx.editor->SelectedMaterial = nullptr;
        m_ctx.editor->SelectedNode = nullptr;
        m_ctx.editor->InspectorRotationNode.reset();
        m_ctx.editor->InspectorRotationEulerValid = false;
        m_ctx.editor->SelectedGaussianSplat = false;
    }

    if (scene)
        m_ctx.lightingPasses->notifyBakersSceneReloaded(*scene);

    requestFullRebuild();
    return true;
}

void SceneContentEditor::requestFullRebuild()
{
    if (m_ctx.rayTracingResources)
        m_ctx.rayTracingResources->requestFullRebuild();
}

std::vector<caustica::math::float3> SceneContentEditor::getMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh) const
{
    return caustica::GetMeshVertices(mesh);
}

std::vector<caustica::math::float3> SceneContentEditor::getMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh) const
{
    if (!m_ctx.sceneManager)
        return {};
    return caustica::GetMeshVerticesWorld(m_ctx.sceneManager->getScene(), mesh, m_ctx.frameIndex());
}

std::vector<caustica::math::float3> SceneContentEditor::getMeshVerticesWorld(const std::shared_ptr<caustica::SceneGraphNode>& node) const
{
    if (!m_ctx.sceneManager)
        return {};
    return caustica::GetMeshVerticesWorld(m_ctx.sceneManager->getScene(), node, m_ctx.frameIndex());
}

void SceneContentEditor::setMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh,
    const std::vector<caustica::math::float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    auto params = MakeMeshEditParams(m_ctx);
    params.recomputeNormals = recomputeNormals;
    params.rebuildAccelerationStructure = rebuildAccelerationStructure;
    caustica::SetMeshVerticesWorld(mesh, vertices, params);
}

void SceneContentEditor::setMeshVerticesWorld(const std::shared_ptr<caustica::SceneGraphNode>& node,
    const std::vector<caustica::math::float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    auto params = MakeMeshEditParams(m_ctx);
    params.recomputeNormals = recomputeNormals;
    params.rebuildAccelerationStructure = rebuildAccelerationStructure;
    caustica::SetMeshVerticesWorld(node, vertices, params);
}

void SceneContentEditor::setMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh,
    const std::vector<caustica::math::float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    auto params = MakeMeshEditParams(m_ctx);
    params.recomputeNormals = recomputeNormals;
    params.rebuildAccelerationStructure = rebuildAccelerationStructure;
    caustica::SetMeshVertices(mesh, vertices, params);
}

} // namespace caustica::editor
