#include <engine/CaptureSequencer.h>

#include <core/format.h>

#include <algorithm>
#include <cassert>

namespace caustica
{

CaptureSequencer::CaptureSequencer(const CommandLineOptions& cmdLine)
{
    if (cmdLine.capturePath == "")
        return;

    m_settings.ScreenshotFileName = cmdLine.capturePath;

    if (cmdLine.captureSimple)
    {
        m_settings.Type = 0;
        m_start = true;
        m_settings.ResetAndWarmup = true;
        m_exitAfterCapture = true;
    }

    if (cmdLine.captureSequence && cmdLine.sequenceFPS > 0 && cmdLine.sequenceFrameCount > 0)
    {
        m_settings.Type = 1;
        m_start = true;
        assert(cmdLine.sequenceWarmupStart >= 0);
        assert(cmdLine.sequenceWarmupStart < cmdLine.sequenceRecordStart);
        assert(cmdLine.sequenceFPS >= 1 && cmdLine.sequenceFPS <= 1000);
        m_settings.SequenceDeltaTime = 1.0 / cmdLine.sequenceFPS;
        m_settings.SequenceBeginTime = cmdLine.sequenceWarmupStart;
        m_settings.SequenceRecordStartTime = cmdLine.sequenceRecordStart;

        m_settings.ResetAndWarmupFrames =
            int((cmdLine.sequenceRecordStart - cmdLine.sequenceWarmupStart) / m_settings.SequenceDeltaTime + 1e-6);
        m_settings.ResetAndWarmup = true;
        m_settings.SequenceRecordFrames = cmdLine.sequenceFrameCount;
        m_exitAfterCapture = true;
        assert(!cmdLine.stopAnimations);
    }
}

void CaptureSequencer::PreAnim(
    float& elapsedTimeSeconds,
    bool asyncLoadingInProgress,
    const std::function<void(double)>& setSceneTime)
{
    if (asyncLoadingInProgress)
        return;

    if (m_start && !m_settings.ScreenshotFileName.empty())
    {
        setSceneTime(m_settings.SequenceBeginTime);
        m_start = false;
        m_active = true;
        m_resetAndWarmupCounter = -1;
    }

    if (m_active && m_settings.Type == 1)
        elapsedTimeSeconds = static_cast<float>(m_settings.SequenceDeltaTime);
}

CaptureSequencerPreRenderActions CaptureSequencer::PreRender()
{
    CaptureSequencerPreRenderActions actions;
    if (m_active && m_settings.ResetAndWarmup)
    {
        if (m_resetAndWarmupCounter == -1)
        {
            m_resetAndWarmupCounter = m_settings.ResetAndWarmupFrames;
            actions.ResetRealtimeCaches = true;
        }
        if (m_resetAndWarmupCounter)
            actions.ResetAccumulation = true;

        m_resetAndWarmupCounter = std::max(0, m_resetAndWarmupCounter - 1);
    }

    return actions;
}

CaptureSequencerPostRenderResult CaptureSequencer::PostRender(
    bool realtimeMode,
    bool accumulationCompleted,
    double sceneTime,
    const std::function<bool(const char*)>& dumpScreenshotCallback)
{
    CaptureSequencerPostRenderResult result;

    if (m_active && !m_settings.ScreenshotFileName.empty()
        && !(m_settings.ResetAndWarmup && m_resetAndWarmupCounter > 0)
        && (realtimeMode || accumulationCompleted))
    {
        std::filesystem::path screenshotFile = m_settings.ScreenshotFileName;

        if (m_settings.Type == 1)
        {
            if (sceneTime < m_settings.SequenceRecordStartTime)
                return result;

            if (m_sequenceRecordCounter == -1)
                m_sequenceRecordCounter = m_settings.SequenceRecordFrames;
            assert(m_sequenceRecordCounter > 0);

            std::filesystem::path justName = screenshotFile.filename().stem();
            std::filesystem::path justExtension = screenshotFile.extension();
            screenshotFile.remove_filename();
            screenshotFile /= justName.string()
                + StringFormat("_%03d", m_settings.SequenceRecordFrames - m_sequenceRecordCounter)
                + justExtension.string();
            m_sequenceRecordCounter--;
        }

        m_captureSuccess = dumpScreenshotCallback(screenshotFile.string().c_str());
        result.CaptureSuccess = m_captureSuccess;
        result.CapturePath = screenshotFile;

        if (m_settings.Type == 0 || (m_settings.Type == 1 && m_sequenceRecordCounter == 0) || !m_captureSuccess)
        {
            m_active = false;
            m_resetAndWarmupCounter = -1;
            m_sequenceRecordCounter = -1;
        }
    }

    if (m_exitAfterCapture && !m_active && !m_start)
    {
        result.ExitRequested = true;
        result.CaptureSuccess = m_captureSuccess;
        result.CapturePath = m_settings.ScreenshotFileName;
    }

    return result;
}

} // namespace caustica
