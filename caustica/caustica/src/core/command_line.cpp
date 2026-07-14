#include <core/command_line.h>
#include <core/log.h>

#include <cxxopts.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>

bool CommandLineOptions::initFromCommandLine(int _argc, char const* const* _argv)
{
    using namespace cxxopts;

    try
    {
        std::filesystem::path exe_path = _argv[0];
        Options options(exe_path.filename().string(),
            "RTX Path Tracing — ray tracing and neural graphics research platform.");

        bool help = false;

        options.add_options()
            ("s,scene", "Preferred scene to load (.scene.json)", value(scene))
            ("nonInteractive", "start in non-interactive mode, disabling popups", value(nonInteractive))
            ("noWindow", "start without a window (headless)", value(noWindow))
            ("d,debug", "Enable D3D12/VK debug layer and NVRHI validation", value(debug))
            ("width", "Window width", value(width))
            ("height", "Window height", value(height))
            ("f,fullscreen", "Run in fullscreen mode", value(fullscreen))
            ("a,adapter", "Preferred adapter substring (e.g. NVIDIA or RTX)", value(adapter))
            ("h,help", "Print this help message", value(help))
            ("backend", "render backend: dx12/d3d12 or vulkan/vk", value(graphicsBackend))
            ("api", "Alias for --backend", value(graphicsBackend))
            ("graphicsApi", "Alias for --backend", value(graphicsBackend))
            ("d3d12", "Use DirectX 12 (default)")
            ("dx12", "Alias for --d3d12")
            ("vk", "Use Vulkan", value(useVulkan))
            ("vulkan", "Alias for --vk", value(useVulkan))
            ("stopAnimations", "start with animations disabled", value(stopAnimations))
            ("noSER", "Disable Shader Execution Reordering", value(disableSER))
            ("adapterIndex", "Preferred adapter index (default: -1 = auto)", value(adapterIndex))
            ("captureSimple", "Trigger simple screenshot capture", value(captureSimple))
            ("captureSequence", "Trigger sequence capture", value(captureSequence))
            ("capturePath", "Output file/path for captures", value(capturePath))
            ("sequenceWarmupStart", "Warmup start time for sequence", value(sequenceWarmupStart))
            ("sequenceRecordStart", "Recording start time for sequence", value(sequenceRecordStart))
            ("sequenceFPS", "Fixed FPS for sequence capture", value(sequenceFPS))
            ("sequenceFrameCount", "Number of frames for sequence", value(sequenceFrameCount))
            ("useNEE", "Enable/disable NEE", value(UseNEE))
            ("NEEType", "NEE type: 0=uniform, 1=power, 2=NEE-AT", value(NEEType))
            ("useReSTIRDI", "Enable/disable ReSTIR DI", value(UseReSTIRDI))
            ("useReSTIRGI", "Enable/disable ReSTIR GI", value(UseReSTIRGI))
            ("useReSTIRPT", "Enable/disable ReSTIR PT", value(UseReSTIRPT))
            ("realtimeSamplesPerPixel", "SPP in realtime mode", value(RealtimeSamplesPerPixel))
            ("referenceSamplesPerPixel", "SPP in reference mode", value(ReferenceSamplesPerPixel))
            ("standaloneDenoiser", "Enable/disable standalone denoiser", value(StandaloneDenoiser))
            ("realtimeAA", "Realtime AA mode: 0=none, 1=TAA, 2=DLSS, 3=DLSS-RR", value(RealtimeAA))
            ("overrideToRealtimeMode", "Override scene default", value(OverrideToRealtimeMode))
            ("overrideToReferenceMode", "Override scene default", value(OverrideToReferenceMode))
            ("overrideAutoexposureOff", "Override scene default", value(OverrideAutoexposureOff))
            ("overrideExposureOffset", "Override scene default", value(OverrideExposureOffset))
            ("disableFireflyFilters", "Disable firefly filters", value(DisableFireflyFilters))
            ("disablePostProcessFilters", "Disable post-process filters", value(DisablePostProcessFilters))
            ("scene.splats", "Gaussian Splat PLY file", value(GaussianSplatFileName))
            ("scene.splats.convertRdfToRub", "Convert RDF to RUB coord space", value(GaussianSplatConvertRdfToRub))
            ("scene.splats.depthTest", "Depth-test splats", value(GaussianSplatDepthTest))
            ("scene.splats.scale", "Splat footprint scale", value(GaussianSplatScale))
            ("scene.splats.alphaScale", "Splat opacity scale", value(GaussianSplatAlphaScale))
            ("scene.splats.brightness", "Splat brightness", value(GaussianSplatBrightness))
            ("scene.splats.asEmitter", "Treat splats as emissive", value(GaussianSplatAsEmitter))
            ("scene.splats.emissionIntensity", "Splat emission intensity", value(GaussianSplatEmissionIntensity))
            ("scene.splats.emissionProxyLimit", "Max emission proxies", value(GaussianSplatEmissionMaxProxyCount))
            ("scene.splats.alphaCull", "Splat alpha cull threshold", value(GaussianSplatAlphaCullThreshold))
            ("cameraPosDirUp", "Camera position (9 comma-separated values)", value(cameraPosDirUp))
            ("propShowTags", "Filter props by comma-separated tags", value(PropShowTags))
            ("propCameraAttach", "Attach camera to named prop", value(PropCameraAttach))
            ("pythonScript", "Path to Python script to run after scene load", value(pythonScript))
            ("pythonExpr", "Inline Python expression to run after scene load", value(pythonExpr))
            ("sceneSwitchTest", "Auto-switch scenes every N render frames (render-thread path)", value(sceneSwitchTestInterval))
            ("sceneSwitchTestCount", "Exit after this many auto scene switches (0 = unlimited)", value(sceneSwitchTestCount))
            ("syncRender", "Run rendering on the main thread (disable async render thread)", value(syncRender));

        int argc = _argc;
        char const* const* argv = _argv;
        options.parse(argc, argv);

        if (!graphicsBackend.empty())
        {
            std::string backend = graphicsBackend;
            std::transform(backend.begin(), backend.end(), backend.begin(),
                [](unsigned char c) { return char(std::tolower(c)); });

            if (backend == "vulkan" || backend == "vk")
                useVulkan = true;
            else if (backend == "dx12" || backend == "d3d12" || backend == "directx12" || backend == "directx")
                useVulkan = false;
            else
            {
                caustica::error("Unknown render backend '%s'. Expected dx12/d3d12 or vulkan/vk.",
                    graphicsBackend.c_str());
                return false;
            }
        }

        if (help)
        {
            std::string helpMessage = options.help();
            caustica::info("%s", helpMessage.c_str());
            return false;
        }

        return true;
    }
    catch (const exceptions::exception& e)
    {
        std::string errorMessage = e.what();
        caustica::error("%s", errorMessage.c_str());
        return false;
    }
}
