#pragma once



#include <engine/SceneSessionSubsystem.h>



namespace caustica::editor

{



class SceneEditor;



struct EditorSceneSubsystemConfig

{

    caustica::SceneSessionConfig session;

    SceneEditor* sceneEditor = nullptr;

    bool postAppInit = true;

};



// Editor scene driver: extends SceneSessionSubsystem with capture scripts and local config.

class EditorSceneSubsystem : public caustica::SceneSessionSubsystem

{

public:

    explicit EditorSceneSubsystem(EditorSceneSubsystemConfig config);



    void initialize(caustica::EngineInitContext& context) override;



protected:

    void onInitializePost(caustica::EngineInitContext& context) override;



    SceneEditor* m_sceneEditor = nullptr;

    bool m_postAppInit = true;

};



} // namespace caustica::editor

