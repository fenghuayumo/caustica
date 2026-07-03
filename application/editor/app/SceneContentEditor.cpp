#include "SceneContentEditor.h"

#include "SceneEditor.h"
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
            .device = sceneEditor.GetDevice(),
            .scene = sceneEditor.GetScene(),
            .frameIndex = sceneEditor.GetFrameIndex(),
            .resetAccumulation = &sceneEditor.GetPathTracerSettings().ResetAccumulation,
            .requestMeshAccelRebuild = [&sceneEditor](const std::shared_ptr<caustica::MeshInfo>& dirtyMesh) {
                sceneEditor.GetRayTracingResources().requestMeshAccelRebuild(dirtyMesh);
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
            if (m_sceneEditor.LoadGaussianSplatFile(path))
            {
                caustica::info("Gaussian Splat loaded successfully: %d splats across %d objects",
                    int(m_sceneEditor.GetGaussianSplatCount()),
                    int(m_sceneEditor.GetGaussianSplatObjectCount()));
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
    auto* sceneManager = m_sceneEditor.GetSceneManager();
    auto textureLoader = m_sceneEditor.GetTextureLoader();
    if (!sceneManager || !textureLoader)
        return false;

    const auto loadResult = loadFile(
        MakeRuntimeMeshLoadParams(sceneManager, textureLoader.get()),
        filePath);
    if (!loadResult)
        return false;

    const auto importedRoot = caustica::AttachRuntimeSceneImport(
        sceneManager->getScene(),
        *loadResult.ImportResult,
        m_sceneEditor.GetFrameIndex(),
        MakeMutationCallbacks());
    if (importedRoot == caustica::ecs::NullEntity)
        return false;

    finalizeRuntimeSceneMutation(caustica::ecs::NullEntity);
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

void SceneContentEditor::finalizeRuntimeSceneMutation(caustica::ecs::Entity importedRoot)
{
    auto* sceneManager = m_sceneEditor.GetSceneManager();
    if (!sceneManager)
        return;

    if (importedRoot != caustica::ecs::NullEntity)
    {
        caustica::FinalizeRuntimeSceneMutation(
            sceneManager->getScene(),
            importedRoot,
            m_sceneEditor.GetFrameIndex(),
            MakeMutationCallbacks());
    }

    if (auto scene = sceneManager->getScene())
        m_sceneEditor.GetLightingPasses().notifySceneReloaded(*scene);

    requestFullRebuild();
}

bool SceneContentEditor::deleteSceneNode(caustica::ecs::Entity entity)
{
    auto* sceneManager = m_sceneEditor.GetSceneManager();
    if (!sceneManager)
        return false;

    auto scene = sceneManager->getScene();
    auto* ew = scene ? scene->GetEntityWorld() : nullptr;

    if (!caustica::DeleteRuntimeSceneNode(caustica::DeleteRuntimeSceneNodeParams{
            .SceneInstance = scene,
            .Entity = entity,
            .Device = m_sceneEditor.GetDevice(),
            .FrameIndex = m_sceneEditor.GetFrameIndex(),
            .BeforeDetach = [&sceneEditor = m_sceneEditor](caustica::ecs::Entity deletedEntity) {
                sceneEditor.GetGaussianSplatPasses().removeObjectsUnderEntity(deletedEntity);
            },
        }))
    {
        return false;
    }

    auto& lightingPasses = m_sceneEditor.GetLightingPasses();
    lightingPasses.resyncLightsFromScene(*scene);

    auto& editor = m_sceneEditor.GetEditorUIState();
    if (editor.TogglableNodes != nullptr && ew)
    {
        editor.TogglableNodes->clear();
        UpdateTogglableNodes(*editor.TogglableNodes, *ew, ew->root());
    }

    editor.SelectedMaterial = nullptr;
    editor.SelectedEntity = caustica::ecs::NullEntity;
    editor.InspectorRotationEntity = caustica::ecs::NullEntity;
    editor.InspectorRotationEulerValid = false;
    editor.SelectedGaussianSplat = false;

    if (scene)
        lightingPasses.notifySceneReloaded(*scene);

    requestFullRebuild();
    return true;
}

void SceneContentEditor::requestFullRebuild()
{
    m_sceneEditor.GetRayTracingResources().requestFullRebuild();
}

std::vector<caustica::math::float3> SceneContentEditor::getMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh) const
{
    return caustica::GetMeshVertices(mesh);
}

std::vector<caustica::math::float3> SceneContentEditor::getMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh) const
{
    return caustica::GetMeshVerticesWorld(m_sceneEditor.GetScene(), mesh, m_sceneEditor.GetFrameIndex());
}

std::vector<caustica::math::float3> SceneContentEditor::getMeshVerticesWorld(caustica::ecs::Entity entity) const
{
    return caustica::GetMeshVerticesWorld(m_sceneEditor.GetScene(), entity, m_sceneEditor.GetFrameIndex());
}

void SceneContentEditor::setMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh,
    const std::vector<caustica::math::float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    auto params = MakeMeshEditParams(m_sceneEditor);
    params.recomputeNormals = recomputeNormals;
    params.rebuildAccelerationStructure = rebuildAccelerationStructure;
    caustica::SetMeshVerticesWorld(mesh, vertices, params);
}

void SceneContentEditor::setMeshVerticesWorld(caustica::ecs::Entity entity,
    const std::vector<caustica::math::float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    auto params = MakeMeshEditParams(m_sceneEditor);
    params.recomputeNormals = recomputeNormals;
    params.rebuildAccelerationStructure = rebuildAccelerationStructure;
    caustica::SetMeshVerticesWorld(entity, vertices, params);
}

void SceneContentEditor::setMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh,
    const std::vector<caustica::math::float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    auto params = MakeMeshEditParams(m_sceneEditor);
    params.recomputeNormals = recomputeNormals;
    params.rebuildAccelerationStructure = rebuildAccelerationStructure;
    caustica::SetMeshVertices(mesh, vertices, params);
}

} // namespace caustica::editor
