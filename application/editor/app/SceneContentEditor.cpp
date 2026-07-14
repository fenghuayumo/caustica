#include "SceneContentEditor.h"

#include "SceneEditor.h"
#include "common/LocalConfig.h"
#include <EditorUI.h>

#include <assets/RuntimeMeshLoadTypes.h>
#include <core/log.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneRayTracingResources.h>
#include <render/core/RenderSceneTypeFactory.h>
#include <render/core/SceneGpuUpdater.h>
#include <render/core/SceneMeshEditing.h>
#include <scene/SceneEcs.h>
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

    caustica::RuntimeMeshLoadParams MakeRuntimeMeshLoadParams(SceneManager* sceneManager, caustica::TextureLoader* textureLoader)
    {
        return caustica::RuntimeMeshLoadParams{
            .TextureCache = textureLoader,
            .SceneTypes = std::make_shared<caustica::render::RenderSceneTypeFactory>(),
            .TextureSearchDirectory = sceneManager
                ? RuntimeMeshTextureSearchDirectory(sceneManager->getCurrentScenePath())
                : std::filesystem::path{},
        };
    }

    caustica::SetSceneMeshVerticesParams MakeMeshEditParams(SceneEditor& sceneEditor)
    {
        return caustica::SetSceneMeshVerticesParams{
            .device = sceneEditor.device(),
            .scene = sceneEditor.scene(),
            .frameIndex = sceneEditor.frameIndex(),
            .resetAccumulation = &sceneEditor.pathTracerSettings().ResetAccumulation,
            .requestMeshAccelRebuild = [&sceneEditor](const std::shared_ptr<caustica::MeshInfo>& dirtyMesh) {
                sceneEditor.gpuRender()->rayTracingResources().requestMeshAccelRebuild(dirtyMesh);
            },
        };
    }
}

SceneContentEditor::SceneContentEditor(SceneEditor& sceneEditor)
    : m_sceneEditor(sceneEditor)
{
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
            if (m_sceneEditor.loadGaussianSplatFile(path))
            {
                caustica::info("Gaussian Splat loaded successfully: %d splats across %d objects",
                    int(m_sceneEditor.gaussianSplatCount()),
                    int(m_sceneEditor.gaussianSplatObjectCount()));
            }
            else
                caustica::error("Failed to load Gaussian Splat file '%s'", filePath.c_str());
        }
        else if (ext == ".gltf" || ext == ".glb" || ext == ".obj" || ext == ".urdf"
            || ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".caususd")
        {
            caustica::info("Drag-drop: loading mesh file '%s'", filePath.c_str());
            if (loadMeshFile(path))
                caustica::info("Mesh file loaded successfully: '%s'", filePath.c_str());
            else
                caustica::error("Failed to load mesh file '%s'", filePath.c_str());
        }
        else
        {
            caustica::warning("Drag-drop: unsupported file type '%s' (supported: .ply, .gltf, .glb, .obj, .urdf, .usd/.usda/.usdc/.caususd)", ext.c_str());
        }
    }
}

bool SceneContentEditor::importMeshFile(const std::filesystem::path& filePath,
    caustica::RuntimeMeshLoadResult (*loadFile)(const caustica::RuntimeMeshLoadParams&, const std::filesystem::path&))
{
    auto* sceneManager = m_sceneEditor.sceneManager();
    auto textureLoader = m_sceneEditor.gpuRender()->textureLoader();
    if (!sceneManager || !textureLoader)
        return false;

    const auto loadResult = loadFile(
        MakeRuntimeMeshLoadParams(sceneManager, textureLoader.get()),
        filePath);
    if (!loadResult)
        return false;

    const auto importedRoot = caustica::attachRuntimeSceneImport(
        sceneManager->getScene(),
        *loadResult.ImportResult,
        m_sceneEditor.frameIndex(),
        MakeMutationCallbacks());
    if (importedRoot == caustica::ecs::NullEntity)
        return false;

    finalizeRuntimeSceneMutation(caustica::ecs::NullEntity);
    return true;
}

bool SceneContentEditor::loadMeshFile(const std::filesystem::path& filePath)
{
    return importMeshFile(filePath, caustica::loadRuntimeMeshFile);
}

bool SceneContentEditor::loadGltfMeshFile(const std::filesystem::path& filePath)
{
    return importMeshFile(filePath, caustica::loadRuntimeGltfMeshFile);
}

bool SceneContentEditor::loadObjMeshFile(const std::filesystem::path& filePath)
{
    return importMeshFile(filePath, caustica::loadRuntimeObjMeshFile);
}

void SceneContentEditor::finalizeRuntimeSceneMutation(caustica::ecs::Entity importedRoot)
{
    auto* sceneManager = m_sceneEditor.sceneManager();
    if (!sceneManager)
        return;

    if (importedRoot != caustica::ecs::NullEntity)
    {
        caustica::finalizeRuntimeSceneMutation(
            sceneManager->getScene(),
            importedRoot,
            m_sceneEditor.frameIndex(),
            MakeMutationCallbacks());
    }

    // Match delete / scene-load teardown: drain render thread before uploading
    // new mesh GPU buffers or requesting AS rebuild while the previous frame
    // may still be reading the old snapshot / acceleration structures.
    m_sceneEditor.gpuDevice().waitForRenderThreadIdle();
    if (nvrhi::IDevice* device = m_sceneEditor.device())
        device->waitForIdle();

    if (auto scene = sceneManager->getScene())
    {
        // Runtime import must not wipe existing PT materials (that leaves a
        // window where SubInstanceData points at cleared material slots, and
        // with 3DGS present the instance-index remap can stick on wrong colors).
        m_sceneEditor.lightingPasses().ensureMaterialsFromScene(scene);

        // recreateAccelStructs runs before updateSceneGeometry in the render frame.
        // Without uploading vertex/index buffers first, createBlases skips new meshes
        // (empty BLAS desc) and buildTlas drops null BLAS — imported geometry never
        // appears until a later accidental rebuild. Same precondition as full scene load.
        caustica::render::SceneGpuUpdater::refreshAfterLoad(*scene, m_sceneEditor.frameIndex());
    }

    // AS rebuild only — requestFullRebuild also forces shader reload +
    // createRenderPassesAndLoadMaterials, which rebuilds hit groups from a
    // pre-sync snapshot and permanently mis-maps materials when 3DGS is present.
    if (auto* gpuRender = m_sceneEditor.gpuRender())
        gpuRender->rayTracingResources().requestAccelerationStructureRebuild();
}

bool SceneContentEditor::deleteSceneNode(caustica::ecs::Entity entity)
{
    auto* sceneManager = m_sceneEditor.sceneManager();
    auto* gpuRender = m_sceneEditor.gpuRender();
    if (!sceneManager || !gpuRender)
        return false;

    auto scene = sceneManager->getScene();
    auto* ew = scene ? scene->getEntityWorld() : nullptr;
    if (!ew || !ew->world().isAlive(entity))
        return false;

    if (!caustica::deleteRuntimeSceneNode(caustica::DeleteRuntimeSceneNodeParams{
            .SceneInstance = scene,
            .Entity = entity,
            .FrameIndex = m_sceneEditor.frameIndex(),
            .BeforeDetach = [gpuRender](caustica::ecs::Entity deletedEntity) {
                gpuRender->gaussianSplatPasses().removeObjectsUnderEntity(deletedEntity);
            },
        }))
    {
        return false;
    }

    m_sceneEditor.lightingPasses().resyncLightsFromScene(*scene);

    auto& editor = m_sceneEditor.GetEditorUIState();
    if (editor.TogglableNodes != nullptr)
    {
        editor.TogglableNodes->clear();
        UpdateTogglableNodes(*editor.TogglableNodes, *ew, ew->root());
    }

    // Selection / PendingDeleteEntity are cleared by the UI deferral and ProcessPending.
    gpuRender->rayTracingResources().requestAccelerationStructureRebuild();
    return true;
}

void SceneContentEditor::requestFullRebuild()
{
    if (auto* gpuRender = m_sceneEditor.gpuRender())
        gpuRender->rayTracingResources().requestFullRebuild();
}

std::vector<caustica::math::float3> SceneContentEditor::getMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh) const
{
    return caustica::getMeshVertices(mesh);
}

std::vector<caustica::math::float3> SceneContentEditor::getMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh) const
{
    return caustica::getMeshVerticesWorld(m_sceneEditor.scene(), mesh, m_sceneEditor.frameIndex());
}

std::vector<caustica::math::float3> SceneContentEditor::getMeshVerticesWorld(caustica::ecs::Entity entity) const
{
    return caustica::getMeshVerticesWorld(m_sceneEditor.scene(), entity, m_sceneEditor.frameIndex());
}

void SceneContentEditor::setMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh,
    const std::vector<caustica::math::float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    auto params = MakeMeshEditParams(m_sceneEditor);
    params.recomputeNormals = recomputeNormals;
    params.rebuildAccelerationStructure = rebuildAccelerationStructure;
    caustica::setMeshVerticesWorld(mesh, vertices, params);
}

void SceneContentEditor::setMeshVerticesWorld(caustica::ecs::Entity entity,
    const std::vector<caustica::math::float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    auto params = MakeMeshEditParams(m_sceneEditor);
    params.recomputeNormals = recomputeNormals;
    params.rebuildAccelerationStructure = rebuildAccelerationStructure;
    caustica::setMeshVerticesWorld(entity, vertices, params);
}

void SceneContentEditor::setMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh,
    const std::vector<caustica::math::float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    auto params = MakeMeshEditParams(m_sceneEditor);
    params.recomputeNormals = recomputeNormals;
    params.rebuildAccelerationStructure = rebuildAccelerationStructure;
    caustica::setMeshVertices(mesh, vertices, params);
}

} // namespace caustica::editor
