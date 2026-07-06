#pragma once



#include <core/command_line.h>

#include <engine/ISubsystem.h>

#include <engine/SceneViewState.h>

#include <render/RenderSessionState.h>

#include <render/SessionDiagnostics.h>



#include <string>



namespace caustica

{



struct SceneSessionConfig

{

    SceneViewState& viewState;

    render::SessionDiagnostics& diagnostics;

    std::string preferredScene;



    render::RenderSessionState* sessionState = nullptr;

    const CommandLineOptions* cmdLine = nullptr;

    bool refreshEnvMapMediaList = true;

    bool applyCmdLineToSessionState = true;

};



// Wires scene view state and GpuRenderSubsystem during engine initialization.

class SceneSessionSubsystem : public ISubsystem

{

public:

    explicit SceneSessionSubsystem(SceneSessionConfig config);



    [[nodiscard]] int priority() const override { return 200; }



    void initialize(EngineInitContext& context) override;



protected:

    virtual void onInitializePost(EngineInitContext& context);



    SceneSessionConfig m_config;

};



void initializeSceneSession(EngineInitContext& context, const SceneSessionConfig& config);



} // namespace caustica

