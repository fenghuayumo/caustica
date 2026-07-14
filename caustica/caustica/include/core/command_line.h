#pragma once

#include <cfloat>
#include <cstdint>
#include <optional>
#include <string>

// =============================================================================
// CommandLineOptions — Application command-line configuration.
//
// Moved from editor/SampleCommon/CommandLine.h to base/core/ so it can be
// used by both engine and editor layers without circular dependencies.
// =============================================================================

struct CommandLineOptions
{
    std::string scene;
    bool nonInteractive = false;
    bool noWindow = false;
    bool debug = false;
    uint32_t width = 1920;
    uint32_t height = 1080;
    bool fullscreen = false;
    std::string adapter;
    std::string graphicsBackend;
    int adapterIndex = -1;
    bool useVulkan = false;
    bool stopAnimations = false;
    bool disableSER = false;

    std::string capturePath = "";
    bool captureSimple = false;
    bool captureSequence = false;
    float sequenceWarmupStart = 0;
    float sequenceRecordStart = 0;
    float sequenceFPS = 60.0f;
    int sequenceFrameCount = 0;

    std::string cameraPosDirUp = "";

    int UseNEE = 1;
    int NEEType = 2;
    int UseReSTIRDI = false;
    int UseReSTIRGI = true;
    int UseReSTIRPT = false;
    int RealtimeSamplesPerPixel = 1;
    int ReferenceSamplesPerPixel = 4096;
    int StandaloneDenoiser = true;
    int RealtimeAA = 3;

    bool OverrideToRealtimeMode = false;
    bool OverrideToReferenceMode = false;

    bool OverrideAutoexposureOff = false;
    float OverrideExposureOffset = FLT_MAX;

    bool DisableFireflyFilters = false;
    bool DisablePostProcessFilters = false;

    std::string GaussianSplatFileName = "";
    bool GaussianSplatConvertRdfToRub = true;
    bool GaussianSplatDepthTest = true;
    float GaussianSplatScale = 1.0f;
    float GaussianSplatAlphaScale = 1.0f;
    float GaussianSplatBrightness = 1.0f;
    bool GaussianSplatAsEmitter = false;
    float GaussianSplatEmissionIntensity = 1.0f;
    int GaussianSplatEmissionMaxProxyCount = 8192;
    float GaussianSplatAlphaCullThreshold = 1.0f / 255.0f;

    std::string PropShowTags = "";
    std::string PropCameraAttach = "";

    std::string pythonScript = "";
    std::string pythonExpr = "";

    int sceneSwitchTestInterval = 0;
    int sceneSwitchTestCount = 0;
    bool syncRender = false;

    CommandLineOptions() = default;

    // Parses argc/argv using cxxopts. Returns false on error or --help.
    bool initFromCommandLine(int argc, char const* const* argv);
};
