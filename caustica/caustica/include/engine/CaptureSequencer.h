#pragma once

#include <core/command_line.h>

#include <filesystem>
#include <functional>

namespace caustica
{

struct CaptureSequencerSettings
{
    std::filesystem::path ScreenshotFileName;
    bool ResetAndWarmup = false;
    int ResetAndWarmupFrames = 64;
    double SequenceBeginTime = 0.0;
    double SequenceRecordStartTime = 0.0;
    double SequenceDeltaTime = 1.0 / 60.0;
    int SequenceRecordFrames = 0;
    int Type = 0; // 0 - simple screenshot; 1 - advanced sequence
};

struct CaptureSequencerPreRenderActions
{
    bool ResetRealtimeCaches = false;
    bool ResetAccumulation = false;
};

struct CaptureSequencerPostRenderResult
{
    bool ExitRequested = false;
    bool captureSuccess = false;
    std::filesystem::path CapturePath;
};

class CaptureSequencer
{
public:
    CaptureSequencer() = default;
    explicit CaptureSequencer(const CommandLineOptions& cmdLine);

    CaptureSequencerSettings& settings() { return m_settings; }
    const CaptureSequencerSettings& settings() const { return m_settings; }

    void requestStart() { m_start = true; }
    bool isStarting() const { return m_start; }
    bool isActive() const { return m_active; }
    bool isDoingWork() const { return m_start || m_active; }
    bool exitAfterCapture() const { return m_exitAfterCapture; }
    bool captureSuccess() const { return m_captureSuccess; }
    int resetAndWarmupCounter() const { return m_resetAndWarmupCounter; }
    int sequenceRecordCounter() const { return m_sequenceRecordCounter; }

    void preAnim(float& elapsedTimeSeconds, bool asyncLoadingInProgress, const std::function<void(double)>& setSceneTime);
    CaptureSequencerPreRenderActions preRender();
    CaptureSequencerPostRenderResult postRender(
        bool realtimeMode,
        bool accumulationCompleted,
        double sceneTime,
        const std::function<bool(const char*)>& dumpScreenshotCallback);

private:
    CaptureSequencerSettings m_settings;
    int m_resetAndWarmupCounter = -1;
    int m_sequenceRecordCounter = -1;
    bool m_start = false;
    bool m_active = false;
    bool m_captureSuccess = false;
    bool m_exitAfterCapture = false;
};

} // namespace caustica
