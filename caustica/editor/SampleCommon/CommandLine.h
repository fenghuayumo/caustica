#pragma once

#include <string>
#include <optional>
#include <cstdint>
#include <cfloat>

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

    int  UseNEE                     = 1;
    int  NEEType                    = 2;
    int  UseReSTIRDI                = false;
    int  UseReSTIRGI                = true;
    int  UseReSTIRPT                = false;
    int  RealtimeSamplesPerPixel    = 1;
    int  ReferenceSamplesPerPixel   = 4096;
    int  StandaloneDenoiser         = true;
    int  RealtimeAA                 = 3;

    bool OverrideToRealtimeMode     = false;
    bool OverrideToReferenceMode    = false;

    bool OverrideAutoexposureOff    = false;
    float OverrideExposureOffset    = FLT_MAX;
    
    bool DisableFireflyFilters      = false;
    bool DisablePostProcessFilters  = false;

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

    std::string PropShowTags        = "";
    std::string PropCameraAttach    = "";

    // Embedded Python scripting hooks.  When --pythonScript is given the
    // script is enqueued for execution right after the scene finishes loading.
    // --pythonExpr runs an inline expression with the same lifecycle.
    std::string pythonScript        = "";
    std::string pythonExpr          = "";

	CommandLineOptions(){}

	bool InitFromCommandLine(int argc, char const* const* argv);
};
