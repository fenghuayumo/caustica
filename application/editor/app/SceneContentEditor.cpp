#include "SceneContentEditor.h"

#include "SceneEditor.h"
#include "EditorAccess.h"
#include "common/LocalConfig.h"
#include <EditorUI.h>

#include <core/log.h>
#include <engine/App.h>
#include <engine/SceneQuery.h>
#include <engine/SceneSpawn.h>
#include <engine/RenderSessionApi.h>
#include <render/core/SceneMeshEditing.h>
#include <scene/SceneEcs.h>

#include <algorithm>
#include <cctype>

namespace caustica::editor
{

namespace
{
    caustica::SceneApplyCallbacks makeApplyCallbacks()
    {
        return caustica::SceneApplyCallbacks{
            .postMaterialLoad = [](caustica::Material& material) { LocalConfig::postMaterialLoad(material); },
        };
    }

    caustica::SetSceneMeshVerticesParams makeMeshEditParams(SceneEditor& sceneEditor)
    {
        return caustica::SetSceneMeshVerticesParams{
            .device = sceneEditor.app()->getGpuDevice()->getDevice(),
            .scene = caustica::activeScene(*sceneEditor.app()),
            .frameIndex = sceneEditor.app()->getGpuDevice()->getFrameIndex(),
            .resetAccumulation = &sceneEditor.pathTracerSettings().ResetAccumulation,
            .requestMeshAccelRebuild = [&sceneEditor](const std::shared_ptr<caustica::MeshInfo>& dirtyMesh) {
                caustica::editor::editorGpu(sceneEditor)->rayTracingResources().requestMeshAccelRebuild(dirtyMesh);
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

    if (caustica::isSceneStructureBusy(editorApp(m_sceneEditor)))
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
            if (caustica::loadGaussianSplatFile(*m_sceneEditor.app(), path))
            {
                caustica::info("Gaussian Splat loaded successfully: %d splats across %d objects",
                    int(caustica::gaussianSplatCount(*m_sceneEditor.app())),
                    int(caustica::gaussianSplatObjectCount(*m_sceneEditor.app())));
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

bool SceneContentEditor::importMeshFile(const std::filesystem::path& filePath)
{
    auto* app = m_sceneEditor.app();
    if (!app)
        return false;

    // assets.load + spawn ??one path for editor and future apps.
    const auto root = caustica::spawnFromFile(*app, filePath, makeApplyCallbacks());
    return root != caustica::ecs::NullEntity;
}

bool SceneContentEditor::loadMeshFile(const std::filesystem::path& filePath)
{
    return importMeshFile(filePath);
}

bool SceneContentEditor::loadGltfMeshFile(const std::filesystem::path& filePath)
{
    return importMeshFile(filePath);
}

bool SceneContentEditor::loadObjMeshFile(const std::filesystem::path& filePath)
{
    return importMeshFile(filePath);
}

bool SceneContentEditor::deleteSceneNode(caustica::ecs::Entity entity)
{
    auto* app = m_sceneEditor.app();
    if (!app)
        return false;

    if (!caustica::despawn(*app, entity))
        return false;

    auto* ew = caustica::entityWorld(*app);
    auto& editor = m_sceneEditor.editorUIState();
    if (ew && editor.TogglableNodes != nullptr)
    {
        editor.TogglableNodes->clear();
        UpdateTogglableNodes(*editor.TogglableNodes, *ew, ew->root());
    }

    return true;
}

void SceneContentEditor::requestFullRebuild()
{
    if (auto* gpuRender = caustica::editor::editorGpu(m_sceneEditor))
        gpuRender->rayTracingResources().requestFullRebuild();
}

std::vector<caustica::math::float3> SceneContentEditor::getMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh) const
{
    return caustica::getMeshVertices(mesh);
}

std::vector<caustica::math::float3> SceneContentEditor::getMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh) const
{
    return caustica::getMeshVerticesWorld(caustica::activeScene(*m_sceneEditor.app()), mesh, m_sceneEditor.app()->getGpuDevice()->getFrameIndex());
}

std::vector<caustica::math::float3> SceneContentEditor::getMeshVerticesWorld(caustica::ecs::Entity entity) const
{
    return caustica::getMeshVerticesWorld(caustica::activeScene(*m_sceneEditor.app()), entity, m_sceneEditor.app()->getGpuDevice()->getFrameIndex());
}

void SceneContentEditor::setMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh,
    const std::vector<caustica::math::float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    auto params = makeMeshEditParams(m_sceneEditor);
    params.recomputeNormals = recomputeNormals;
    params.rebuildAccelerationStructure = rebuildAccelerationStructure;
    caustica::setMeshVerticesWorld(mesh, vertices, params);
}

void SceneContentEditor::setMeshVerticesWorld(caustica::ecs::Entity entity,
    const std::vector<caustica::math::float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    auto params = makeMeshEditParams(m_sceneEditor);
    params.recomputeNormals = recomputeNormals;
    params.rebuildAccelerationStructure = rebuildAccelerationStructure;
    caustica::setMeshVerticesWorld(entity, vertices, params);
}

void SceneContentEditor::setMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh,
    const std::vector<caustica::math::float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    auto params = makeMeshEditParams(m_sceneEditor);
    params.recomputeNormals = recomputeNormals;
    params.rebuildAccelerationStructure = rebuildAccelerationStructure;
    caustica::setMeshVertices(mesh, vertices, params);
}

} // namespace caustica::editor
