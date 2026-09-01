#pragma once

#include <shaders/PathTracer/Config.h>
//#include <EditorUI.h>

#include <scene/camera/Camera.h>
#include <scene/Scene.h>

#include "EditorCommandLine.h"
#include "GameModel.h"
#include "GameProps.h"

#ifdef _DEBUG
#define GAME_DEVELOPER_SETTINGS
#endif

namespace caustica::editor { class SceneEditor; }

// DEMO-ONLY editor game stage loader (not an embedding API).
// Prefer EngineApp + EntityWorld / SceneSpawn for new hosts (thin_client).
class GameScene
{
public:
    GameScene(caustica::editor::SceneEditor& editor, const caustica::editor::EditorCommandLine& cmdLine);

    void                    sceneLoaded( const std::shared_ptr<caustica::Scene> & scene, const std::filesystem::path& sceneFilePath, const std::filesystem::path & mediaPath );
    void                    sceneUnloading( );
    bool                    debugGUI(float indent);
    void                    StandaloneGUI(const std::shared_ptr<caustica::PlanarView> & view, const float2 & displaySize);

    bool                    IsInitialized() const           { return m_scene != nullptr && !m_props.empty(); }
    bool                    CameraActive() const            { return m_gameCameraAttached.lock() != nullptr; }
    const caustica::FirstPersonCamera &
                            GetCamera() const               { return m_gameCamera; }
    std::shared_ptr<demo::PropBase>
                            GetCameraAttached() const       { return m_gameCameraAttached.lock(); }
    void                    AttachCamera(const std::shared_ptr<demo::PropBase> & prop);
    const demo::Pose &      GetLastRenderCameraPose() const { return m_lastRenderCameraPose; }

    // active means animating / physics is enabled
    //void                    SetActive(bool active);
    bool                    isActive()                      { return m_playSpeed != 0; }

    bool                    keyboardUpdate(int key, int scancode, int action, int mods);
    void                    mousePosUpdate(double xpos, double ypos);
    void                    mouseButtonUpdate(int button, int action, int mods);
    void                    Tick(float deltaTime, bool globalAnimationEnabled); // globalAnimationEnabled will be false if not in reference mode or global scene animations not enabled
    void                    TickCamera(float deltaTime, caustica::FirstPersonCamera & renderCamera);

    const std::shared_ptr<caustica::Scene> &
                            scene() const { return m_scene; }

    double                  gameTime() const             { return m_gameTime; }
    void                    SetGameTime(double t)           { m_gameTime = t; }

    std::shared_ptr<demo::ModelType>
                            FindModelType(const std::string & name);

    std::shared_ptr<demo::PropBase>
                            GetSelectedProp() const { return m_selectedProp.lock(); }

    GLFWwindow *            GetGLFWWindow() const;

    const std::vector<demo::Pose> & GetCamRecAnimation() const { return m_recordedCameraPoses; }

    const caustica::editor::EditorCommandLine& GetCmdLine() const { return m_cmdLine; }

private:
    std::shared_ptr<demo::PropBase> CreatePropFromFile(const std::string& name, const std::filesystem::path& storagePath, const Json::Value& jsonRoot);
    void                    Deinitialize( );
    void                    ResetGame( );

private:
    caustica::editor::SceneEditor &          m_editor;
    std::shared_ptr<caustica::Scene>
                            m_scene = nullptr;
    int                     m_playSpeed = 3;   // speed: 0 - paused, 1 - 0.1x, 2 - 0.5x, 3 - 1.0x, 4 - 2.0x, 5 - 10.0x

    std::vector<std::shared_ptr<demo::ModelType>> m_modelTypes;

    std::vector<std::shared_ptr<demo::PropBase>>
                            m_props;
    std::weak_ptr<demo::PropBase>
                            m_selectedProp;

    double                  m_gameTime = 0.0;

    bool                    m_timeLoopEnable    = false;
    float                   m_timeLoopFrom      = 0.0f;
    float                   m_timeLoopTo        = 0.0f;

    bool                    m_lastTickGlobalAnimationEnabled = false;

    std::filesystem::path   m_gameStoragePath;

    bool                    m_camRecEnabled = false;
    float                   m_camRecKeyframeStep = 1.0f;
    float                   m_camRecTimeToNextKeyframe = 0.0f;
    std::vector<demo::Pose> m_recordedCameraPoses;
    demo::Pose              m_lastRenderCameraPose; // useful for stuff like "set prop to camera pose"

    caustica::FirstPersonCamera   m_gameCamera;
    std::weak_ptr<demo::PropBase>   m_gameCameraAttached;

    bool                            m_wasGameCameraActive = false;
    float3                          m_sceneCameraLastPos;
    float3                          m_sceneCameraLastDir;
    float3                          m_sceneCameraLastUp;

    const caustica::editor::EditorCommandLine& m_cmdLine;
};

