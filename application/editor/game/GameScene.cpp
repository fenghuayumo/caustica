#include "ui/ui_macros.h"
#include "GameScene.h"
#include "common/EditorTheme.h"

#include <core/log.h>
#include <core/json.h>
#include <core/format.h>
#include <math/math.h>
#include <scene/camera/Camera.h>
#include <cmath>

#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <ecs/Entity.h>

#include <core/file_utils.h>
#include <core/path_utils.h>
#include <core/format.h>
#include "SceneEditor.h"

#include <engine/App.h>
#include <backend/GpuDevice.h>
#include <render/passes/debug/Korgi.h>
#include <json/json.h>

using namespace caustica::math;
using namespace caustica;
using namespace caustica;
using namespace caustica;
using namespace caustica::render;

#include <fstream>
#include <iostream>
#include <thread>

namespace
{
    std::filesystem::path ResolveGameDataRoot(const std::filesystem::path& sceneFilePath, const std::filesystem::path& mediaPath)
    {
        const std::filesystem::path sceneDirGamePath = sceneFilePath.parent_path() / c_GameDataSubFolder;
        if (std::filesystem::exists(sceneDirGamePath))
            return sceneDirGamePath;

        return mediaPath / std::string(c_GameDataSubFolder);
    }

    std::filesystem::path ResolveGameStoragePath(const std::filesystem::path& mediaGamePath, const std::filesystem::path& sceneFilePath)
    {
        const std::filesystem::path sceneStem = sceneFilePath.filename().stem();
        std::vector<std::filesystem::path> candidates;
        candidates.push_back(mediaGamePath / sceneStem);
        if (sceneStem.extension() == ".scene")
            candidates.push_back(mediaGamePath / sceneStem.stem());

        for (const auto& candidate : candidates)
        {
            if (std::filesystem::exists(candidate))
                return candidate;
        }

        return candidates.front();
    }
}


GameScene::GameScene(caustica::editor::SceneEditor& sample, const CommandLineOptions& cmdLine)
    : m_sample(sample), m_cmdLine(cmdLine) // NOTE: at this point, SceneEditor is being constructed - beware of accessing incompletely constructed object
{
}

GLFWwindow* GameScene::GetGLFWWindow() const
{
    return m_sample.app() && m_sample.app()->getGpuDevice()
        ? m_sample.app()->getGpuDevice()->getWindow()
        : nullptr;
}

void GameScene::Deinitialize()
{
    m_scene = nullptr;
    m_props.clear();
    m_modelTypes.clear();
    m_gameStoragePath = std::filesystem::path();
    m_gameTime = 0.0;
    m_timeLoopEnable = false;
    m_timeLoopFrom = 0.0f;
    m_timeLoopTo = 0.0f;
    m_lastTickGlobalAnimationEnabled = false;
    m_camRecEnabled = false;
    m_recordedCameraPoses.clear();
    m_wasGameCameraActive = false;
    m_selectedProp.reset();
    m_playSpeed = 3;
}

void GameScene::ResetGame()
{
    m_camRecEnabled = false;
    for( auto & prop : m_props )
        prop->reset();
}

// void GameScene::SetActive(bool active)
// {
//     if (m_active == active)
//         return;
// 
//     m_active = active;
//     if (m_active == false)
//     {
//         m_gameTime = 0.0;
//     }
// }

std::shared_ptr<game::ModelType> GameScene::FindModelType(const std::string& modelTypeName)
{
    if (modelTypeName == "")
        return nullptr;
    auto it = std::find_if(m_modelTypes.begin(), m_modelTypes.end(), [ &modelTypeName ](const std::shared_ptr<game::ModelType>& pt) { return pt->getModelName() == modelTypeName; });
    if (it == m_modelTypes.end())
        return nullptr;
    return *it;
}

std::shared_ptr<game::PropBase> GameScene::CreatePropFromFile(const std::string& name, const std::filesystem::path& storagePath, const Json::Value& jsonRoot)
{
    std::string propType;
    jsonRoot["propType"] >> propType;

    std::shared_ptr<game::PropBase> prop = nullptr;
    if (propType == "SimpleProp")
        prop = std::make_shared<game::SimpleProp>(*this, name);
    if (prop == nullptr)
        { assert( false ); return nullptr; }
    prop->SetStoragePath(storagePath);
    prop->load(jsonRoot);
    prop->PostLoadSetup();
    prop->reset();
    return prop;
}

void GameScene::sceneLoaded(const std::shared_ptr<caustica::Scene>& scene, const std::filesystem::path& sceneFilePath, const std::filesystem::path & mediaPath)
{
    Deinitialize();

    const GameSettings* gameSettings = scene->getGameSettings();
    if (gameSettings == nullptr)
        return;

    const std::filesystem::path mediaGamePath = ResolveGameDataRoot(sceneFilePath, mediaPath);
    if (!ensureDirectoryExists(mediaGamePath))
        { assert(false); return; }

    m_gameStoragePath = ResolveGameStoragePath(mediaGamePath, sceneFilePath);
    if (!ensureDirectoryExists(m_gameStoragePath))
        { assert(false); return; }

    ensureDirectoryExists(m_gameStoragePath / "models");
    ensureDirectoryExists(m_gameStoragePath / "props");

    Json::Value node;
    bool parsingSuccessful = caustica::json::fromString(gameSettings->getJsonData(), node);
    if (!parsingSuccessful) 
    {
        caustica::warning( "Unable to load game settings" );
        assert( false );
        return;
    }

    m_scene = scene;

    auto modelFiles = enumerateFilesWithWildcard(m_gameStoragePath / "models", "*.model.json");

    for (auto modelPath : modelFiles)
    {
        auto fileNoExt = modelPath.filename();
        fileNoExt.replace_extension();
        fileNoExt.replace_extension();

        Json::Value modelRoot;
        if (!caustica::json::loadFromFile(modelPath, modelRoot) || modelRoot.empty() || !modelRoot.isObject())
            continue;
        m_modelTypes.push_back( std::make_shared<game::ModelType>(*m_scene, fileNoExt.string(), modelRoot) );
    }

    auto propFiles = enumerateFilesWithWildcard(m_gameStoragePath / "props", "*.prop.json");

    for( auto propPath : propFiles )
    {
        auto fileNoExt = propPath.filename();
        fileNoExt.replace_extension();
        fileNoExt.replace_extension();

        Json::Value propRoot;
        if (!caustica::json::loadFromFile(propPath, propRoot) || propRoot.empty() || !propRoot.isObject() )
            continue;
        
        std::shared_ptr<game::PropBase> newProp = CreatePropFromFile(fileNoExt.string(), propPath, propRoot);
        if (newProp != nullptr)
            m_props.push_back( newProp );
    }

    if( m_cmdLine.PropCameraAttach != "" )
    {
        auto it = std::find_if(m_props.begin(), m_props.end(), [this]( const std::shared_ptr<game::PropBase> & prop ) { return caustica::equalsIgnoreCase(prop->getName(), m_cmdLine.PropCameraAttach); } );
        if (it != m_props.end())
            AttachCamera(*it);
    }

    if (m_props.empty())
    {
        if (!m_modelTypes.empty())
            caustica::warning("GameSettings found model definitions but no props under '%s'", m_gameStoragePath.string().c_str());
        else
            caustica::warning("GameSettings present but no game data found under '%s'", m_gameStoragePath.string().c_str());
        m_modelTypes.clear();
        m_gameStoragePath = std::filesystem::path();
        return;
    }


    // std::srand(0);
    // for (int i = 0; i < (int)m_modelInstances.size(); i++)
    // {
    //     ModelInstance& vehicle = *m_modelInstances[i];
    //     std::string fileName = vehicle.getName() + ".vehicle.json";
    //     vehicle.SetStoragePath(m_gameStoragePath/fileName);
    //     vehicle.load();
    //     //vehicle.SetAnimOffset(std::rand() / (float)RAND_MAX * 110.0f);
    // }

}

void GameScene::sceneUnloading()
{
    Deinitialize();
}

static float GetPlaySpeedK(int playSpeed)
{
    float playSpeedK = 0.0f;
    switch (playSpeed) {
    case(1): playSpeedK = 0.1f; break;
    case(2): playSpeedK = 0.5f; break;
    case(3): playSpeedK = 1.0f; break;
    case(4): playSpeedK = 2.0f; break;
    case(5): playSpeedK =10.0f; break;
    }
    return playSpeedK;
}

void GameScene::AttachCamera(const std::shared_ptr<game::PropBase> & prop)
{
    if (prop == nullptr)
        m_gameCameraAttached.reset();
    else
    {
        auto [pos, dir, up] = prop->GetDefaultCameraPose().getPosDirUp();
        m_gameCamera.lookTo(pos, dir, up);
        m_gameCameraAttached = prop;
    }
}

bool GameScene::debugGUI(float indent)
{
    if (!m_lastTickGlobalAnimationEnabled)
        ImGui::Text("Note: global animations disabled, game world not updating!");

    {
        UI_SCOPED_DISABLE(!m_lastTickGlobalAnimationEnabled);

        float playSpeedK = GetPlaySpeedK(m_playSpeed);
        if (ImGui::Button("[-2s]"))
            m_gameTime = std::max(0.0, m_gameTime-2.0);
        ImGui::SameLine();
        if (ImGui::Button("<slower<"))
            m_playSpeed--;
        ImGui::SameLine();
        if (m_playSpeed == 0 && ImGui::Button("[PLAY]"))
            m_playSpeed = 3;
        else if (m_playSpeed != 0 && ImGui::Button("[PAUSE]"))
            m_playSpeed = 0;
        ImGui::SameLine();
        if (ImGui::Button(">faster>"))
            m_playSpeed++;
        ImGui::SameLine();
        if (ImGui::Button("[+2s]"))
            m_gameTime = m_gameTime + 2.0;

        ImGui::Text("Time %05.2f, play speed %.2fx", m_gameTime, playSpeedK);
        ImGui::SameLine();
        if (ImGui::Button("reset##Timer"))
            m_gameTime = 0.0f;

        ImGui::Checkbox("Loop", &m_timeLoopEnable);
        if (m_timeLoopEnable)
        {
            ImGui::SameLine();
            float2 fromTo(m_timeLoopFrom, m_timeLoopTo); ImGui::InputFloat2("from<->to", &fromTo.x, "%.2f"); m_timeLoopFrom = fromTo.x; m_timeLoopTo = fromTo.y;
        }
    }

    {
        auto cameraAttached = m_gameCameraAttached.lock();
        if (cameraAttached == nullptr)
            ImGui::Text("Game camera not active");
        else
        {
            ImGui::Text("Game camera attached to %s", cameraAttached->getName().c_str());
            if (ImGui::Button("Detach"))
                m_gameCameraAttached.reset();
        }
    }

    if (ImGui::CollapsingHeader("Props", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RAII_SCOPE(ImGui::Indent(indent); , ImGui::Unindent(indent); );

        // ImGui::Text("Props:");
        // ImGui::Separator();

        int itemDisplaySize = 6;
        ImGui::BeginChild("ItemList", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * itemDisplaySize), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);

        for (int i = 0; i < (int)m_props.size(); i++)
        {
            const std::shared_ptr<game::PropBase> & prop = m_props[i];

            bool selected = m_selectedProp.lock() == prop;
            if (ImGui::Selectable(prop->getName().c_str(), &selected, ImGuiSelectableFlags_None))
                m_selectedProp = (selected)?(prop):(nullptr);
        }
        ImGui::EndChild();

        ImGui::Separator();
        //ImGui::Text("Selected:");
        ImGui::BeginChild("Properties", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * itemDisplaySize), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        
        std::shared_ptr<game::PropBase> selectedProp = m_selectedProp.lock();
        if (selectedProp)
        {
            bool cameraAttached = m_gameCameraAttached.lock() == selectedProp;
            selectedProp->GUI(indent, cameraAttached, m_gameCamera);
            if (cameraAttached && m_gameCameraAttached.lock() != selectedProp)
                AttachCamera(selectedProp);
            if (!cameraAttached && m_gameCameraAttached.lock() == selectedProp)
                AttachCamera(nullptr); 
        }
        else
        {
            ImGui::Text("No selected prop");
        }

        ImGui::EndChild();
    }

#ifdef SAMPLE_GAME_DEVELOPER_SETTINGS
    ImGui::Separator();

    if(!m_camRecEnabled)
    {
        if (ImGui::Button("start camera rec (1 sec delay)"))
        {
            m_camRecEnabled = true;
            m_camRecTimeToNextKeyframe = 1;
            m_gameTime = -1.0f;
        }
    }
    if (m_camRecEnabled)
    {
        if (ImGui::Button("stop recording"))
            m_camRecEnabled = false;
    }
    ImGui::Text("Recorded poses: %d", m_recordedCameraPoses.size());
    if (!m_camRecEnabled && m_recordedCameraPoses.size() > 0)
    {
        if (ImGui::Button("Copy first to last for looping"))
        {
            game::Pose term = m_recordedCameraPoses[0];
            term.KeyTime = m_recordedCameraPoses.back().KeyTime+m_camRecKeyframeStep;
            m_recordedCameraPoses.push_back(term);
        }

        if (ImGui::Button("SAVE REC TO EXPORT_POSES.json"))
        {
            Json::Value animsJ;
            for (auto& pose : m_recordedCameraPoses)
                animsJ.append(pose.write());
            Json::Value rootJ;
            rootJ["animation"] = animsJ;

            caustica::json::saveToFile(m_gameStoragePath / "EXPORT_POSES.json", rootJ);
            m_recordedCameraPoses.clear();
        }
    }
#endif

    return false;
}

bool GameScene::keyboardUpdate(int key, int scancode, int action, int mods)
{
    if (CameraActive())
        m_gameCamera.keyboardUpdate(key, scancode, action, mods);

    //if (key == GLFW_KEY_SPACE && action == GLFW_PRESS && mods == GLFW_MOD_CONTROL)
    //    m_recordCamera = true;

    if (!isActive())
        return false;

    return false;
}
void GameScene::mousePosUpdate(double xpos, double ypos)
{
    if (CameraActive())
        m_gameCamera.mousePosUpdate(xpos, ypos);
}
void GameScene::mouseButtonUpdate(int button, int action, int mods)
{
    if (CameraActive())
        m_gameCamera.mouseButtonUpdate(button, action, mods);
}

void GameScene::Tick(float deltaTime, bool globalAnimationEnabled)
{
    deltaTime = min(deltaTime, 0.5f);

    if (m_timeLoopEnable && m_gameTime < m_timeLoopFrom)
        m_gameTime = m_timeLoopFrom;

    m_playSpeed = dm::clamp(m_playSpeed, 0, 5);
    float playSpeedK = GetPlaySpeedK(m_playSpeed);

    m_lastTickGlobalAnimationEnabled = globalAnimationEnabled;
    if (isActive() && globalAnimationEnabled)
    {
        m_gameTime += deltaTime * playSpeedK;

        if (m_timeLoopEnable)
        {
            double loopSpan = (double)m_timeLoopTo-(double)m_timeLoopFrom;
            if (loopSpan > 0 )
            {
                double p = (m_gameTime-m_timeLoopFrom)/loopSpan;
                m_gameTime = (p-floor(p))*loopSpan+m_timeLoopFrom;
            }
            else
                m_gameTime = m_timeLoopFrom;
        }

    }

    for (auto& prop : m_props)
        prop->Tick(m_gameTime, deltaTime);
}

void GameScene::TickCamera(float deltaTime, caustica::FirstPersonCamera & renderCamera)
{
    // in case we're switching from scene camera (renderCamera) to game camera and back, save/restore scene camera
    if (!m_wasGameCameraActive && CameraActive())
    {
        m_sceneCameraLastPos = renderCamera.getPosition();
        m_sceneCameraLastDir = renderCamera.getDir();
        m_sceneCameraLastUp  = renderCamera.getUp();
    }
    if (m_wasGameCameraActive && !CameraActive())
    {
        renderCamera.lookTo(m_sceneCameraLastPos, m_sceneCameraLastDir, m_sceneCameraLastUp);
    }
    m_wasGameCameraActive = CameraActive();

    if (CameraActive())
    {
        // Allow game camera to move in its own reference frame - this should be optional as some props might like to have control of it
        m_gameCamera.animate(deltaTime);

        // Move game camera into it's parent (attached) prop's reference frame and apply to global renderCamera
        {
            auto attachedProp = m_gameCameraAttached.lock();
            affine3 transform = affine3::identity();
            if (attachedProp != nullptr)
            {
                // transform = attachedObject->GetRootNode()->GetLocalToWorldTransformFloat(); // we can't do this because these have not yet been updated
                auto* ew = attachedProp->EntityWorld();
                caustica::ecs::Entity propEntity = attachedProp->GetEntity();
                dm::daffine3 transformD = dm::daffine3::identity();
                if (ew && propEntity != caustica::ecs::NullEntity)
                {
                    auto* ltc = ew->world().tryGet<caustica::scene::LocalTransformComponent>(propEntity);
                    if (ltc)
                    {
                        transformD = dm::scaling(ltc->scaling);
                        transformD *= ltc->rotation.toAffine();
                        transformD *= dm::translation(ltc->translation);
                    }
                }
                transform = dm::affine3(transformD);
            }

            renderCamera.lookTo( transform.transformPoint(m_gameCamera.getPosition()), transform.transformVector(m_gameCamera.getDir()), transform.transformVector(m_gameCamera.getUp()));
        }
    }

    m_lastRenderCameraPose.setTransformFromCamera(renderCamera.getPosition(), renderCamera.getDir(), renderCamera.getUp());

    //if (!m_active)
    //    return;
    
    if (m_camRecEnabled && isActive())
    {
        m_camRecTimeToNextKeyframe = dm::clamp(m_camRecTimeToNextKeyframe-deltaTime, -m_camRecKeyframeStep, m_camRecKeyframeStep);

        if (m_camRecTimeToNextKeyframe <= 0)
        {
            game::Pose pose;
            pose.setTransformFromCamera(renderCamera.getPosition(), renderCamera.getDir(), renderCamera.getUp());
            pose.Scaling = { 1,1,1 };
            pose.KeyTime = m_gameTime;

            m_recordedCameraPoses.push_back(pose);

            m_camRecTimeToNextKeyframe += m_camRecKeyframeStep;
        }
    }
}

void GameScene::StandaloneGUI(const std::shared_ptr<caustica::PlanarView> & view, const float2 & displaySize)
{
    // collect toggles
    struct BigButton
    {
        std::string                 Name;
        std::optional<std::string>  HoverText;

        std::function<bool(void)>   IsSelected;
        std::function<void(void)>   OnClick;

        bool                        enabled;

        BigButton(const std::string& name, const std::string& hoverText, std::function<void(void)> onClick, std::function<bool(void)> isSelected) : Name(name), HoverText(hoverText), OnClick(onClick), IsSelected(isSelected), enabled(true) { }
        std::string                 GetText() const { return Name; }

    };
    std::vector<BigButton> buttons;

    auto gameCameraAttached = m_gameCameraAttached.lock();
    if (gameCameraAttached!=nullptr)
        buttons.push_back( BigButton("Exit prop camera", stringFormat( "Camera attached to %s", gameCameraAttached->getName().c_str() ), [&]() { AttachCamera(nullptr); }, [ ]() { return true; } ) );

    if (buttons.size() > 0)
    {
        auto& io = ImGui::GetIO();
        float scaledWidth = io.DisplaySize.x;
        float scaledHeight = io.DisplaySize.y;

        // show & 
        ImVec2 texSizeA = ImGui::CalcTextSize("A");
        float buttonWidth = texSizeA.x * 16;
        float windowHeight = texSizeA.y * 3.0f;
        float windowWidth = buttonWidth * buttons.size() + ImGui::GetStyle().ItemSpacing.x * (buttons.size() + 1);
        ImGui::SetNextWindowPos(ImVec2(0.5f * (scaledWidth - windowWidth), scaledHeight - 10.0f - windowHeight ), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.0f);
        if (ImGui::Begin("GameUI", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav))
        {
            for (int i = 0; i < buttons.size(); i++)
            {
                if (i > 0)
                    ImGui::SameLine();

                UI_SCOPED_DISABLE(!buttons[i].enabled);

                bool selected = buttons[i].IsSelected();

                ImGui::PushID(i);
                caustica::editor::PushToolbarButtonColors(selected);
                if (ImGui::Button(buttons[i].GetText().c_str(), ImVec2(buttonWidth, texSizeA.y * 2)))
                {
                    buttons[i].OnClick();
                }
                caustica::editor::PopToolbarButtonColors();
                ImGui::PopID();

                if (buttons[i].HoverText.has_value())
                {
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip(buttons[i].HoverText.value().c_str());
                }
            }
        }
        ImGui::End();
    }

    auto currentlyAttachedProp = m_gameCameraAttached.lock();
    game::ScreenGUISel selArea{}; std::shared_ptr<game::PropBase> selProp;
    float2 mousePos = { ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y };
    for (auto& prop : m_props)
    {
        if (prop == currentlyAttachedProp)
            continue;

        game::ScreenGUISel selC = prop->StandaloneGUI(view, mousePos, displaySize);
        if (selC.Selected && selC.RangeToCamera < selArea.RangeToCamera)
        {
            selArea = selC;
            selProp = prop;
        }
    }
    
    if (selArea.Selected && selProp != nullptr)
    {
        ImDrawList* draw_list = ImGui::GetForegroundDrawList();
        draw_list->AddCircle(ImVec2(selArea.ScreenPos.x, selArea.ScreenPos.y), selArea.ScreenRadius, IM_COL32(0, 0, 255, 255), 32);
        std::string info = stringFormat("Press 'F' to lock camera to prop '%s'", selProp->getName().c_str());
        draw_list->AddText(ImVec2(selArea.ScreenPos.x+1, selArea.ScreenPos.y+1), IM_COL32(0, 0, 0, 192), info.c_str() );
        draw_list->AddText(ImVec2(selArea.ScreenPos.x, selArea.ScreenPos.y), IM_COL32(255, 255, 255, 255), info.c_str());

        if (ImGui::IsKeyDown(ImGuiKey::ImGuiKey_F))
            AttachCamera(selProp);
    }
      
}

