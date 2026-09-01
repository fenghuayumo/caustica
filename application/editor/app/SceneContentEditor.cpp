#include "SceneContentEditor.h"

#include "SceneEditor.h"
#include "EditorAccess.h"
#include "common/LocalConfig.h"
#include <EditorUI.h>

#include <core/log.h>
#include <core/path_utils.h>
#include <engine/App.h>
#include <engine/CameraApi.h>
#include <engine/SceneQuery.h>
#include <engine/SceneSpawn.h>
#include <engine/MeshDeformApi.h>
#include <engine/RenderSessionApi.h>
#include <scene/SceneEcs.h>
#include <scene/SceneSerializer.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <string>

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

    bool WouldRemoveLastEnvironmentLight(
        caustica::scene::SceneEntityWorld& entityWorld, caustica::ecs::Entity subtree)
    {
        size_t environmentLightCount = 0;
        size_t removedEnvironmentLightCount = 0;
        entityWorld.world().each<caustica::scene::EnvironmentLightComponent>(
            [&](caustica::ecs::Entity light, caustica::scene::EnvironmentLightComponent&) {
                ++environmentLightCount;
                if (entityWorld.entitySubtreeContains(subtree, light))
                    ++removedEnvironmentLightCount;
            });
        return environmentLightCount > 0 && removedEnvironmentLightCount == environmentLightCount;
    }

    const char* BuiltinSource(BuiltinPrimitiveKind kind)
    {
        switch (kind)
        {
        case BuiltinPrimitiveKind::Plane: return "builtin:plane";
        case BuiltinPrimitiveKind::Cube: return "builtin:cube";
        case BuiltinPrimitiveKind::Sphere: return "builtin:sphere";
        case BuiltinPrimitiveKind::Cylinder: return "builtin:cylinder";
        }
        return "builtin:cube";
    }

    const char* BuiltinDisplayName(BuiltinPrimitiveKind kind)
    {
        switch (kind)
        {
        case BuiltinPrimitiveKind::Plane: return "Plane";
        case BuiltinPrimitiveKind::Cube: return "Cube";
        case BuiltinPrimitiveKind::Sphere: return "Sphere";
        case BuiltinPrimitiveKind::Cylinder: return "Cylinder";
        }
        return "Mesh";
    }

    const char* LightDisplayName(EditorLightKind kind)
    {
        switch (kind)
        {
        case EditorLightKind::Directional: return "DirectionalLight";
        case EditorLightKind::Point: return "PointLight";
        case EditorLightKind::Spot: return "SpotLight";
        case EditorLightKind::Rect: return "RectLight";
        case EditorLightKind::Environment: return "EnvironmentLight";
        }
        return "Light";
    }

    dm::dquat DefaultSunRotation()
    {
        return dm::dquat::fromXYZW(dm::double4(-0.23053891, -0.15879166, -0.68904659, 0.66846975));
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
            || ext == ".usd" || ext == ".usda" || ext == ".usdc")
        {
            caustica::info("Drag-drop: loading mesh file '%s'", filePath.c_str());
            if (loadMeshFile(path))
                caustica::info("Mesh file loaded successfully: '%s'", filePath.c_str());
            else
                caustica::error("Failed to load mesh file '%s'", filePath.c_str());
        }
        else
        {
            caustica::warning("Drag-drop: unsupported file type '%s' (supported: .ply, .gltf, .glb, .obj, .urdf, .usd/.usda/.usdc)", ext.c_str());
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

bool SceneContentEditor::deleteEntity(caustica::ecs::Entity entity)
{
    auto* app = m_sceneEditor.app();
    if (!app)
        return false;

    auto* ew = caustica::entityWorld(*app);
    if (!ew || WouldRemoveLastEnvironmentLight(*ew, entity))
        return false;

    std::string authoringId;
    if (const auto* authoring = ew->world().tryGet<caustica::scene::SceneAuthoringIdComponent>(entity))
        authoringId = authoring->id;

    if (!caustica::despawn(*app, entity))
        return false;

    if (!authoringId.empty())
        caustica::scene::removeAuthoredEntityNode(m_sceneEditor.editorState().sceneDocument, authoringId);

    auto& editor = m_sceneEditor.editorUIState();
    if (ew && editor.TogglableNodes != nullptr)
    {
        editor.TogglableNodes->clear();
        UpdateTogglableNodes(*editor.TogglableNodes, *ew, ew->root());
    }

    return true;
}

std::string SceneContentEditor::makeUniqueAuthoringId(const std::string& baseName) const
{
    auto* app = m_sceneEditor.app();
    auto* ew = app ? caustica::entityWorld(*app) : nullptr;
    const Json::Value* document = m_sceneEditor.editorState().sceneDocumentValid
        ? &m_sceneEditor.editorState().sceneDocument
        : nullptr;

    auto taken = [&](const std::string& id) {
        if (!ew)
            return false;
        bool used = false;
        ew->world().each<caustica::scene::SceneAuthoringIdComponent>(
            [&](caustica::ecs::Entity, const caustica::scene::SceneAuthoringIdComponent& authoring) {
                if (authoring.id == id)
                    used = true;
            });
        ew->world().each<caustica::scene::NameComponent>(
            [&](caustica::ecs::Entity entity, const caustica::scene::NameComponent& name) {
                if (entity != ew->root() && name.value == id)
                    used = true;
            });
        if (document && (*document)["entities"].isArray())
        {
            for (const Json::Value& node : (*document)["entities"])
            {
                if (node["id"].isString() && node["id"].asString() == id)
                    used = true;
                if (node["name"].isString() && node["name"].asString() == id)
                    used = true;
            }
        }
        return used;
    };

    if (!taken(baseName))
        return baseName;
    for (int i = 2; i < 10000; ++i)
    {
        const std::string candidate = baseName + "_" + std::to_string(i);
        if (!taken(candidate))
            return candidate;
    }
    return baseName + "_x";
}

void SceneContentEditor::registerAuthoredEntity(caustica::ecs::Entity entity)
{
    auto* app = m_sceneEditor.app();
    auto* ew = app ? caustica::entityWorld(*app) : nullptr;
    if (!ew || !ecs::isValid(entity))
        return;

    auto& editorState = m_sceneEditor.editorState();
    if (!editorState.sceneDocumentValid)
    {
        editorState.sceneDocument = Json::Value(Json::objectValue);
        editorState.sceneDocument["format"] = "caustica.scene";
        editorState.sceneDocument["version"] = 2;
        const std::string sceneName = editorState.loadedSceneName.empty()
            ? "untitled"
            : editorState.loadedSceneName;
        editorState.sceneDocument["name"] = sceneName;
        editorState.sceneDocument["entities"] = Json::Value(Json::arrayValue);
        editorState.sceneDocumentValid = true;
    }

    caustica::scene::upsertAuthoredEntityNode(editorState.sceneDocument, *ew, entity);
}

void SceneContentEditor::placeInFrontOfCamera(caustica::ecs::Entity entity, bool snapToGround)
{
    auto* app = m_sceneEditor.app();
    auto* ew = app ? caustica::entityWorld(*app) : nullptr;
    if (!app || !ew || !ecs::isValid(entity))
        return;

    dm::double3 translation(0.0, snapToGround ? 0.0 : 1.2, 0.0);
    const caustica::FirstPersonCamera& camera = caustica::currentCamera(*app);
    const dm::float3 pos = camera.getPosition() + camera.getDir() * 4.0f;
    translation = dm::double3(double(pos.x), double(pos.y), double(pos.z));
    if (snapToGround)
        translation.y = 0.0;

    ew->setTranslation(entity, translation);
    ew->refreshHierarchy(caustica::scene::PreviousTransformPolicy::CaptureCurrent);
}

void SceneContentEditor::selectCreatedEntity(caustica::ecs::Entity entity, bool isLight)
{
    auto& editor = m_sceneEditor.editorUIState();
    editor.SelectedEntity = entity;
    editor.SelectedGaussianSplat = false;
    editor.SelectedMaterial = nullptr;
    editor.InspectorRotationEntity = caustica::ecs::NullEntity;
    editor.InspectorRotationEulerValid = false;
    editor.ShowInspector = true;
    editor.RequestFocusInspector = true;

    auto& settings = m_sceneEditor.pathTracerSettings();
    settings.ResetAccumulation = true;
    if (isLight)
        settings.ResetRealtimeCaches = true;
}

caustica::ecs::Entity SceneContentEditor::createBuiltinMesh(BuiltinPrimitiveKind kind)
{
    auto* app = m_sceneEditor.app();
    if (!app || !caustica::isSceneLoaded(*app) || caustica::isSceneStructureBusy(*app))
        return caustica::ecs::NullEntity;

    const std::string id = makeUniqueAuthoringId(BuiltinDisplayName(kind));
    const ecs::Entity root = caustica::spawnFromSource(*app, BuiltinSource(kind), makeApplyCallbacks());
    if (!ecs::isValid(root))
    {
        caustica::error("Failed to create builtin mesh '%s'", id.c_str());
        return caustica::ecs::NullEntity;
    }

    auto* ew = caustica::entityWorld(*app);
    if (!ew)
        return caustica::ecs::NullEntity;

    ew->world().emplace<caustica::scene::NameComponent>(root, caustica::scene::NameComponent{ id });
    ew->world().emplace<caustica::scene::SceneAuthoringIdComponent>(
        root, caustica::scene::SceneAuthoringIdComponent{ id });
    ew->world().emplace<caustica::scene::PrefabInstanceComponent>(
        root, caustica::scene::PrefabInstanceComponent{ BuiltinSource(kind), {} });
    ew->rebuildPathsFromRoot();
    placeInFrontOfCamera(root, true);
    registerAuthoredEntity(root);
    selectCreatedEntity(root, false);
    caustica::info("Created %s", id.c_str());
    return root;
}

caustica::ecs::Entity SceneContentEditor::createLight(EditorLightKind kind)
{
    auto* app = m_sceneEditor.app();
    auto* ew = app ? caustica::entityWorld(*app) : nullptr;
    if (!app || !ew || !caustica::isSceneLoaded(*app))
        return caustica::ecs::NullEntity;
    if (kind == EditorLightKind::Rect && caustica::isSceneStructureBusy(*app))
        return caustica::ecs::NullEntity;

    const std::string id = makeUniqueAuthoringId(LightDisplayName(kind));
    ecs::Entity entity = caustica::ecs::NullEntity;

    switch (kind)
    {
    case EditorLightKind::Directional:
    {
        caustica::scene::DirectionalLightComponent light;
        light.color = dm::float3(1.0f, 0.96f, 0.9f);
        light.irradiance = 4.0f;
        light.angularSize = 1.5f;
        entity = caustica::spawnDirectionalLight(*app, std::move(light), id);
        if (ecs::isValid(entity))
            ew->setRotation(entity, DefaultSunRotation());
        break;
    }
    case EditorLightKind::Point:
    {
        caustica::scene::PointLightComponent light;
        light.color = dm::float3(1.0f, 0.92f, 0.78f);
        light.intensity = 40.0f;
        light.radius = 0.05f;
        light.range = 12.0f;
        entity = caustica::spawnPointLight(*app, std::move(light), id);
        break;
    }
    case EditorLightKind::Spot:
    {
        caustica::scene::SpotLightComponent light;
        light.color = dm::float3(1.0f, 0.94f, 0.82f);
        light.intensity = 55.0f;
        light.radius = 0.04f;
        light.range = 14.0f;
        light.innerAngle = 18.0f;
        light.outerAngle = 32.0f;
        entity = caustica::spawnSpotLight(*app, std::move(light), id);
        break;
    }
    case EditorLightKind::Rect:
    {
        caustica::scene::RectLightComponent light;
        light.color = dm::float3(0.95f, 0.97f, 1.0f);
        light.intensity = 20.0f;
        light.width = 0.6f;
        light.height = 0.4f;
        entity = caustica::spawnRectLight(*app, std::move(light), id);
        break;
    }
    case EditorLightKind::Environment:
    {
        caustica::scene::EnvironmentLightComponent light;
        light.radianceScale = dm::float3(1.0f);
        light.path = c_EnvMapProcSky;
        entity = caustica::spawnEnvironmentLight(*app, std::move(light), id);
        break;
    }
    }

    if (!ecs::isValid(entity))
    {
        caustica::error("Failed to create light '%s'", id.c_str());
        return caustica::ecs::NullEntity;
    }

    ew->world().emplace<caustica::scene::SceneAuthoringIdComponent>(
        entity, caustica::scene::SceneAuthoringIdComponent{ id });
    if (kind != EditorLightKind::Directional && kind != EditorLightKind::Environment)
        placeInFrontOfCamera(entity, false);
    else
        ew->refreshHierarchy(caustica::scene::PreviousTransformPolicy::CaptureCurrent);

    if (kind == EditorLightKind::Rect)
    {
        if (auto* local = ew->world().tryGet<caustica::scene::LocalTransformComponent>(entity))
        {
            dm::double3 translation = local->translation;
            translation.y = std::max(translation.y, 1.6);
            ew->setTranslation(entity, translation);
        }
        caustica::syncRectLightVisual(*app, entity);
        ew->refreshHierarchy(caustica::scene::PreviousTransformPolicy::CaptureCurrent);
    }
    ew->discardStructureDirtyIfGeometryUnchanged();

    registerAuthoredEntity(entity);
    selectCreatedEntity(entity, true);
    caustica::info("Created %s", id.c_str());
    return entity;
}

void SceneContentEditor::requestFullRebuild()
{
    if (m_sceneEditor.app())
        caustica::requestFullAccelRebuild(*m_sceneEditor.app());
}

std::vector<caustica::math::float3> SceneContentEditor::getMeshVertices(caustica::ecs::Entity entity) const
{
    return caustica::getMeshVertices(*m_sceneEditor.app(), entity);
}

std::vector<caustica::math::float3> SceneContentEditor::getMeshVerticesWorld(caustica::ecs::Entity entity) const
{
    return caustica::getMeshVerticesWorld(*m_sceneEditor.app(), entity);
}

void SceneContentEditor::setMeshVerticesWorld(caustica::ecs::Entity entity,
    const std::vector<caustica::math::float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    caustica::setMeshVerticesWorld(
        *m_sceneEditor.app(),
        entity,
        vertices,
        { .recomputeNormals = recomputeNormals, .rebuildAccelerationStructure = rebuildAccelerationStructure });
}

void SceneContentEditor::setMeshVertices(caustica::ecs::Entity entity,
    const std::vector<caustica::math::float3>& vertices,
    bool recomputeNormals,
    bool rebuildAccelerationStructure)
{
    caustica::setMeshVertices(
        *m_sceneEditor.app(),
        entity,
        vertices,
        { .recomputeNormals = recomputeNormals, .rebuildAccelerationStructure = rebuildAccelerationStructure });
}

} // namespace caustica::editor










