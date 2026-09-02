// Extension-mode entry point: builds a real Python extension module
// (caustica.pyd / caustica.so) so that standalone Python interpreters can drive
// caustica for offline rendering and headless data generation.
//
//    python -c "import caustica; r = caustica.Renderer(headless=True); ..."

#if CAUSTICA_WITH_PYTHON

#include "PythonBindingsCore.h"
#include "PythonDevice.h"
#include "RenderSession.h"

#include <caustica/version.h>

#include <engine/EntryPoint.h>
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/vector.h>

#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/SceneSpawn.h>
#include <engine/RenderSessionApi.h>
#include <engine/App.h>
#include <engine/AppResources.h>
#include <math/box.h>
#include <math/math.h>
#include <scene/Scene.h>

#include <memory>
#include <stdexcept>
#include <vector>

namespace nb = nanobind;
using caustica::App;
using caustica::Scene;

namespace
{
    // Tracks the most recently created Renderer instance so module-level
    // helpers like `caustica.app()` and `caustica.settings()` keep working.
    App* g_currentExtensionApp = nullptr;

    App& RequireCurrentApp()
    {
        if (!g_currentExtensionApp)
            throw std::runtime_error("caustica: no App is currently active. create one via caustica.App(...) or caustica.Renderer(...)");
        return *g_currentExtensionApp;
    }

    caustica::math::float3 ToFloat3(const nb::object& src)
    {
        nb::sequence seq = nb::cast<nb::sequence>(src);
        std::vector<float> v;
        for (auto h : seq) v.push_back(nb::cast<float>(nb::handle(h)));
        if (v.size() != 3)
            throw std::runtime_error("Expected an iterable of 3 floats");
        return caustica::math::float3(v[0], v[1], v[2]);
    }

    nb::tuple Float3ToTuple(const caustica::math::float3& v) { return nb::make_tuple(v.x, v.y, v.z); }

    bool IsFiniteBox(const caustica::math::box3& bounds)
    {
        return caustica::math::all(caustica::math::isfinite(bounds.m_mins))
            && caustica::math::all(caustica::math::isfinite(bounds.m_maxs));
    }

    // Mirrors the helper used by Scene.bounds in the shared core bindings.
    // Returns the C++ Scene bounds, or std::nullopt when no scene is loaded.
    std::optional<caustica::math::box3> CurrentSceneBoundingBox(App* app)
    {
        if (!app)
            return std::nullopt;
        auto scene = caustica::activeScene(*app);
        if (!scene)
            return std::nullopt;
        const caustica::math::box3 bbox = scene->getSceneBounds();
        if (bbox.isempty() || !IsFiniteBox(bbox))
            return std::nullopt;
        return bbox;
    }
}

nb::ndarray<nb::numpy, uint8_t, nb::shape<-1, -1, 4>, nb::c_contig, nb::device::cpu>
FramebufferToNumpy(const RenderSession::FramebufferLdr& fb)
{
    auto* data = new std::vector<uint8_t>(fb.pixels);
    const size_t height = fb.height;
    const size_t width = fb.width;
    nb::capsule owner(data, [](void* p) noexcept {
        delete static_cast<std::vector<uint8_t>*>(p);
    });
    return nb::ndarray<nb::numpy, uint8_t, nb::shape<-1, -1, 4>, nb::c_contig, nb::device::cpu>(
        data->data(), { height, width, 4 }, owner);
}

struct PyFrame
{
    RenderSession::FramebufferLdr rgb;
};

std::shared_ptr<caustica_py::PythonDevice> MakePythonDevice(
    bool useVulkan, const std::string& adapter, bool debug)
{
    caustica_py::PythonDevice::Config cfg;
    cfg.useVulkan = useVulkan;
    cfg.adapter = adapter;
    cfg.debug = debug;
    return std::make_shared<caustica_py::PythonDevice>(cfg);
}

// Scene + frame loop bound to an existing Device. Device/GPU outlive scene switches.
class PyApp
{
public:
    PyApp(std::shared_ptr<caustica_py::PythonDevice> device,
              int width, int height, bool headless,
              const std::string& scene, bool realtimeMode, int accumulationTarget)
    {
        if (!device)
            throw std::runtime_error("caustica.App: Device is required");

        RenderSession::Config cfg;
        cfg.width              = width;
        cfg.height             = height;
        cfg.headless           = headless;
        cfg.useVulkan          = device->useVulkan();
        cfg.adapter            = device->config().adapter;
        cfg.debug              = device->config().debug;
        cfg.nonInteractive     = true;
        cfg.scene              = scene;
        cfg.realtimeMode       = realtimeMode;
        cfg.accumulationTarget = accumulationTarget;

        m_session = std::make_unique<RenderSession>(std::move(device), cfg);
        m_owned   = m_session->GetEngine() != nullptr;
        if (!m_owned)
            throw std::runtime_error("caustica.App: failed to initialize (see log for details)");
        g_currentExtensionApp = &m_session->GetEngine()->app();
    }

    PyApp(PyApp&&) noexcept = default;
    PyApp& operator=(PyApp&&) noexcept = default;
    PyApp(const PyApp&) = delete;
    PyApp& operator=(const PyApp&) = delete;

    ~PyApp()
    {
        Close();
    }

    void Close()
    {
        if (m_session)
        {
            if (m_session->GetEngine() && g_currentExtensionApp == &m_session->GetEngine()->app())
                g_currentExtensionApp = nullptr;
            m_session.reset();
            m_owned = false;
        }
    }

    bool LoadScene(const std::string& sceneName, bool wait, double timeoutSeconds, int warmupFrames) {
        return m_session ? m_session->LoadScene(sceneName, wait, timeoutSeconds, warmupFrames) : false;
    }

    bool WaitUntilReady(double timeoutSeconds, int warmupFrames) {
        return m_session ? m_session->WaitUntilReady(timeoutSeconds, warmupFrames) : false;
    }

    bool IsSceneReady() const { return m_session && m_session->IsSceneReady(); }

    bool LoadGaussianSplats(const std::string& fileName, bool convertRdfToRub) {
        return m_session && m_session->GetApp()
            ? caustica::loadGaussianSplatFile(*m_session->GetApp(), fileName, convertRdfToRub)
            : false;
    }

    bool Step(float dt) {
        return m_session ? m_session->Step(dt) : false;
    }

    bool StepN(int frames) {
        return m_session ? m_session->StepN(frames) : false;
    }

    int StepUntilAccumulated(int maxFrames) {
        return m_session ? m_session->StepUntilAccumulated(maxFrames) : 0;
    }

    bool PrepareAnimationFrame(double sceneTime, bool importedAnimations, bool keyframes) {
        return m_session
            ? m_session->PrepareAnimationFrame(sceneTime, importedAnimations, keyframes)
            : false;
    }

    int RenderReferenceFrame(int spp, bool oidn, int maxFrames) {
        return m_session ? m_session->RenderReferenceFrame(spp, oidn, maxFrames) : 0;
    }

    bool RenderRealtimeFrame(float dt) {
        return m_session ? m_session->RenderRealtimeFrame(dt) : false;
    }

    bool SaveScreenshot(const std::string& path) {
        return m_session ? m_session->SaveScreenshot(path) : false;
    }

    std::optional<RenderSession::FramebufferLdr> GetFramebufferLdr() {
        return m_session ? m_session->GetFramebufferLdr() : std::nullopt;
    }

    bool SetCamera(nb::object pos, nb::object dir, nb::object up) {
        if (!m_session) return false;
        return m_session->SetCamera(ToFloat3(pos), ToFloat3(dir), ToFloat3(up));
    }

    bool loadMeshFile(const std::string& fileName) {
        if (!m_session || !m_session->GetApp())
            return false;
        // Same engine path as Sample.load_mesh_file / SceneContentEditor.
        return caustica::spawnFromFile(*m_session->GetApp(), fileName) != caustica::ecs::NullEntity;
    }

    void SetCameraFOV(float fov) {
        if (m_session) m_session->SetCameraFOV(fov);
    }

    void SetCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height) {
        if (m_session) m_session->setCameraIntrinsics(fx, fy, cx, cy, width, height);
    }

    App* GetApp() {
        return m_session && m_session->GetEngine() ? &m_session->GetEngine()->app() : nullptr;
    }

    // Offline/load precache: CreateStateObject for every cooked feature preset.
    // Call after at least one successful step() so hit groups exist.
    uint32_t PrecacheRtFeaturePresets(bool showProgress = true)
    {
        App* app = GetApp();
        if (!app)
            return 0;
        return caustica::precacheRtFeaturePresets(*app, showProgress);
    }

    bool isValid() const { return m_session && m_session->GetEngine() != nullptr; }

    std::shared_ptr<caustica_py::PythonDevice> GetDevice() const
    {
        return m_session ? m_session->GetDevice() : nullptr;
    }

    std::optional<caustica::AdapterInfo> GetSelectedAdapter() const
    {
        if (auto device = GetDevice())
            return device->selectedAdapter();
        if (!m_session || !m_session->GetEngine())
            return std::nullopt;
        caustica::GpuDevice* device = m_session->GetEngine()->app().getGpuDevice();
        if (!device || !device->getSelectedAdapter())
            return std::nullopt;
        return *device->getSelectedAdapter();
    }

    PyFrame GetFrame()
    {
        auto fb = GetFramebufferLdr();
        if (!fb)
            throw std::runtime_error("get_frame failed: no rendered LDR texture (call step() or render() first)");
        return PyFrame{ std::move(*fb) };
    }

    PyFrame Render(float dt)
    {
        if (!Step(dt))
            throw std::runtime_error("caustica.App.render: step failed");
        return GetFrame();
    }

    PyFrame RenderReference(int spp, bool oidn, int maxFrames)
    {
        if (RenderReferenceFrame(spp, oidn, maxFrames) <= 0)
            throw std::runtime_error("caustica.App.render_reference: accumulation failed");
        return GetFrame();
    }

    bool CreateScene(const std::string& builtin, bool wait, double timeoutSeconds, int warmupFrames)
    {
        std::string name = builtin;
        if (name.rfind("builtin:", 0) != 0)
            name = "builtin:" + name;
        return LoadScene(name, wait, timeoutSeconds, warmupFrames);
    }

protected:
    std::unique_ptr<RenderSession> m_session;
    bool m_owned = false;
};

class PyRenderer : public PyApp
{
public:
    PyRenderer(int width, int height, bool headless, bool useVulkan,
               const std::string& adapter, bool debug, const std::string& scene,
               bool realtimeMode, int accumulationTarget)
        : PyApp(MakePythonDevice(useVulkan, adapter, debug),
                    width, height, headless, scene, realtimeMode, accumulationTarget)
    {
    }
};

NB_MODULE(caustica, m)
{
    m.doc() = "caustica Python extension - drive the path-tracer for offline rendering.";
    m.attr("__version__") = CAUSTICA_VERSION_STRING;

    caustica_py::RegisterCoreBindings(m);

    nb::class_<caustica::rhi::AdapterDesc>(m, "AdapterInfo",
        "Backend-neutral GPU adapter descriptor returned by enumerate_adapters().")
        .def_prop_ro("index", [](const caustica::rhi::AdapterDesc& self) { return self.index; })
        .def_prop_ro("name", [](const caustica::rhi::AdapterDesc& self) { return self.name; })
        .def_prop_ro("backend", [](const caustica::rhi::AdapterDesc& self) {
            switch (self.api)
            {
            case caustica::rhi::GraphicsAPI::D3D11: return std::string("d3d11");
            case caustica::rhi::GraphicsAPI::D3D12: return std::string("d3d12");
            case caustica::rhi::GraphicsAPI::VULKAN: return std::string("vulkan");
            }
            return std::string("unknown");
        })
        .def_prop_ro("type", [](const caustica::rhi::AdapterDesc& self) {
            return std::string(caustica::rhi::adapterTypeToString(self.type));
        })
        .def_prop_ro("vendor_id", [](const caustica::rhi::AdapterDesc& self) { return self.vendorID; })
        .def_prop_ro("device_id", [](const caustica::rhi::AdapterDesc& self) { return self.deviceID; })
        .def_prop_ro("dedicated_video_memory", [](const caustica::rhi::AdapterDesc& self) {
            return self.dedicatedVideoMemory;
        })
        .def_prop_ro("selection_score", [](const caustica::rhi::AdapterDesc& self) {
            return self.selectionScore;
        })
        .def_prop_ro("supports_ray_tracing_pipeline", [](const caustica::rhi::AdapterDesc& self) {
            return self.supportsRayTracingPipeline;
        })
        .def_prop_ro("supports_ray_query", [](const caustica::rhi::AdapterDesc& self) {
            return self.supportsRayQuery;
        })
        .def_prop_ro("suitable", [](const caustica::rhi::AdapterDesc& self) { return self.suitable; })
        .def_prop_ro("software", [](const caustica::rhi::AdapterDesc& self) { return self.software; })
        .def_prop_ro("uuid", [](const caustica::rhi::AdapterDesc& self) -> std::optional<std::string> {
            if (!self.uuid)
                return std::nullopt;
            return caustica::rhi::adapterUuidToString(*self.uuid);
        })
        .def_prop_ro("luid", [](const caustica::rhi::AdapterDesc& self) -> std::optional<std::string> {
            if (!self.luid)
                return std::nullopt;
            return caustica::rhi::adapterLuidToString(*self.luid);
        })
        .def("__repr__", [](const caustica::rhi::AdapterDesc& self) {
            return std::string("<caustica.AdapterInfo index=") + std::to_string(self.index)
                + " name='" + self.name + "' backend='"
                + (self.api == caustica::rhi::GraphicsAPI::VULKAN ? "vulkan" :
                   self.api == caustica::rhi::GraphicsAPI::D3D12 ? "d3d12" : "d3d11")
                + "'>";
        });

    m.def("enumerate_adapters", [](bool useVulkan, bool debug) {
            std::vector<caustica::AdapterInfo> adapters;
            std::string error;
            const caustica::rhi::GraphicsAPI api = useVulkan
                ? caustica::rhi::GraphicsAPI::VULKAN
                : caustica::rhi::GraphicsAPI::D3D12;
            if (!caustica::GpuDevice::enumerateAvailableAdapters(api, adapters, debug, &error))
                throw std::runtime_error("caustica.enumerate_adapters: " + error);
            return adapters;
        },
        nb::arg("vulkan") = false,
        nb::arg("debug") = false,
        "Enumerate GPU adapters for DX12 or Vulkan without creating a renderer.");

    auto deviceClass = nb::class_<caustica_py::PythonDevice>(m, "Device",
        "GPU handle. Creating a Device does not load a scene.\n"
        "The logical GPU, presentation surface, and optional window are created\n"
        "on the first App bind.")
        .def(nb::new_([](bool vulkan, const std::string& adapter, bool debug) {
                return MakePythonDevice(vulkan, adapter, debug);
            }),
            nb::arg("vulkan") = false,
            nb::arg("adapter") = "auto",
            nb::arg("debug") = false)
        .def_prop_ro("vulkan", [](const caustica_py::PythonDevice& self) { return self.useVulkan(); })
        .def_prop_ro("adapter", [](const caustica_py::PythonDevice& self) { return self.config().adapter; })
        .def_prop_ro("debug", [](const caustica_py::PythonDevice& self) { return self.config().debug; })
        .def_prop_ro("created", [](const caustica_py::PythonDevice& self) { return self.isCreated(); })
        .def_prop_ro("bound", [](const caustica_py::PythonDevice& self) { return self.isBound(); })
        .def_prop_ro("width", [](const caustica_py::PythonDevice& self) { return self.width(); })
        .def_prop_ro("height", [](const caustica_py::PythonDevice& self) { return self.height(); })
        .def_prop_ro("headless", [](const caustica_py::PythonDevice& self) { return self.headless(); })
        .def_prop_ro("selected_adapter", &caustica_py::PythonDevice::selectedAdapter)
        .def("close", &caustica_py::PythonDevice::close,
             "Release the GPU, surface, and window. Fails if an App is still bound;\n"
             "close the App first.")
        .def("__enter__", [](caustica_py::PythonDevice& self) -> caustica_py::PythonDevice* { return &self; },
             nb::rv_policy::reference)
        .def("__exit__", [](caustica_py::PythonDevice& self, nb::object, nb::object, nb::object) -> bool {
             self.close();
             return false;
        }, nb::arg().none(), nb::arg().none(), nb::arg().none())
        .def("__repr__", [](const caustica_py::PythonDevice& self) {
            return std::string("<caustica.Device created=") + (self.isCreated() ? "True" : "False")
                + " bound=" + (self.isBound() ? "True" : "False") + ">";
        });

    nb::class_<PyFrame>(m, "Frame",
        "One rendered output. rgb is LDR RGBA8. depth and segmentation are reserved.")
        .def_prop_ro("width", [](const PyFrame& self) { return self.rgb.width; })
        .def_prop_ro("height", [](const PyFrame& self) { return self.rgb.height; })
        .def_prop_ro("channels", [](const PyFrame& self) { return self.rgb.channels; })
        .def_prop_ro("pixels", [](const PyFrame& self) {
                return nb::bytes(self.rgb.pixels.data(), self.rgb.pixels.size());
            })
        .def_prop_ro("rgb", [](PyFrame& self) { return FramebufferToNumpy(self.rgb); },
            "NumPy (H, W, 4) uint8 RGBA. Requires NumPy.")
        .def_prop_ro("depth", [](const PyFrame&) -> nb::object { return nb::none(); },
            "Not implemented yet.")
        .def_prop_ro("segmentation", [](const PyFrame&) -> nb::object { return nb::none(); },
            "Not implemented yet.")
        .def("__repr__", [](const PyFrame& self) {
            return std::string("<caustica.Frame ")
                + std::to_string(self.rgb.width) + "x" + std::to_string(self.rgb.height) + " RGBA8>";
        });

    nb::class_<RenderSession::FramebufferLdr>(m, "Framebuffer",
        "CPU-side LDR framebuffer from Renderer.get_framebuffer().\n"
        "pixels are tightly packed RGBA8 bytes (row-major, top-left origin).")
        .def_ro("width", &RenderSession::FramebufferLdr::width)
        .def_ro("height", &RenderSession::FramebufferLdr::height)
        .def_ro("channels", &RenderSession::FramebufferLdr::channels)
        .def_prop_ro("format", [](const RenderSession::FramebufferLdr&) { return "RGBA8"; })
        .def_prop_ro("dtype", [](const RenderSession::FramebufferLdr&) { return "uint8"; })
        .def_prop_ro("pixels", [](const RenderSession::FramebufferLdr& self) {
                return nb::bytes(self.pixels.data(), self.pixels.size());
            },
            "Raw tightly packed RGBA8 bytes; len == width * height * 4.")
        .def_prop_ro("shape", [](const RenderSession::FramebufferLdr& self) {
                return nb::make_tuple(self.height, self.width, self.channels);
            },
            "(H, W, C) shape matching NumPy image layout.")
        .def("__len__", [](const RenderSession::FramebufferLdr& self) { return self.pixels.size(); })
        .def("__repr__", [](const RenderSession::FramebufferLdr& self) {
                return std::string("<caustica.Framebuffer ")
                    + std::to_string(self.width) + "x" + std::to_string(self.height)
                    + " RGBA8, " + std::to_string(self.pixels.size()) + " bytes>";
            });

    nb::class_<PyApp>(m, "App",
        "Application session bound to a Device: scene, camera, settings, and stepping.\n"
        "load_scene / create_scene do not recreate the GPU.\n\n"
        "Example:\n"
        "    device = caustica.Device(adapter='auto')\n"
        "    app = device.create_app(width=1280, height=720, headless=True)\n"
        "    app.create_scene('plane_cube')\n"
        "    frame = app.render_reference(spp=64)\n"
        "    rgb = frame.rgb")
        .def(nb::init<std::shared_ptr<caustica_py::PythonDevice>, int, int, bool, const std::string&, bool, int>(),
             nb::arg("device"),
             nb::arg("width") = 1920,
             nb::arg("height") = 1080,
             nb::arg("headless") = true,
             nb::arg("scene") = std::string("{\"entities\":[]}"),
             nb::arg("realtime") = false,
             nb::arg("accumulation_target") = 64)

        .def_prop_ro("device", &PyApp::GetDevice,
             "The Device that owns the GPU and presentation surface for this runtime.")
        .def_prop_ro("selected_adapter", &PyApp::GetSelectedAdapter,
             "The adapter selected during renderer creation.")

        .def("close", &PyApp::Close,
             "Tear down the scene session and unbind this App from its Device.\n"
             "The Device keeps the GPU and surface; call Device.close() to release them.")

        .def("load_scene",
             [](PyApp& self, const std::string& name, bool wait, double timeoutSeconds, int warmupFrames) {
                 return self.LoadScene(name, wait, timeoutSeconds, warmupFrames);
             },
             nb::arg("scene_name"), nb::arg("wait_until_ready") = true,
             nb::arg("timeout_seconds") = 600.0, nb::arg("warmup_frames") = 4,
             "load a scene by name, builtin primitive reference, or inline scene JSON string.")

        .def("wait_until_ready", &PyApp::WaitUntilReady,
             nb::arg("timeout_seconds") = 600.0, nb::arg("warmup_frames") = 4,
             "Wait for scene import and GPU publication, then render warm-up frames.")

        .def_prop_ro("scene_ready", &PyApp::IsSceneReady,
             "True after scene import and GPU publication complete.")

        .def("load_gaussian_splats",
             [](PyApp& self, const std::string& fileName, bool convertRdfToRub) {
                 return self.LoadGaussianSplats(fileName, convertRdfToRub);
             },
             nb::arg("file_name"), nb::arg("convert_rdf_to_rub") = true,
             "Append a 3DGS .ply file as a GaussianSplat entity in the current scene.")

        .def("load_mesh_file", &PyApp::loadMeshFile,
             nb::arg("file_name"),
             "Append a mesh file (.gltf, .glb, .obj, .urdf, or .usd/.usda/.usdc) to the current scene.")

        .def("step", &PyApp::Step,
             nb::arg("dt") = -1.0f,
             "render exactly one frame.  Returns True on success.")

        .def("step_n", &PyApp::StepN,
             nb::arg("frames"),
             "render N frames.")

        .def("precache_rt_feature_presets", &PyApp::PrecacheRtFeaturePresets,
             nb::arg("show_progress") = true,
             "UE-style load/cook precache: CreateStateObject for every cooked PT feature\n"
             "preset (REF/BUILD/FILL). Call after step()/step_n(1). Returns ready count.\n"
             "Interactive runtime does not background-warm these on the frame loop.")

        .def("step_until_accumulated", &PyApp::StepUntilAccumulated,
             nb::arg("max_frames") = 0,
             "reset accumulation and keep stepping until the SPP target is reached\n"
             "(or `max_frames` frames have been produced if positive).")

        .def("prepare_animation_frame", &PyApp::PrepareAnimationFrame,
             nb::arg("time_seconds"),
             nb::arg("imported_animations") = true,
             nb::arg("keyframes") = true,
             "Evaluate animation once at an exact timeline time, reset temporal history,\n"
             "and leave the resulting pose frozen for reference accumulation.")

        .def("render_reference_frame", &PyApp::RenderReferenceFrame,
             nb::arg("spp") = 64,
             nb::arg("oidn") = true,
             nb::arg("max_frames") = 0,
             "Render one frozen-time reference output frame. Accumulates `spp` samples\n"
             "with dt=0, optionally runs OIDN synchronously, and returns engine frame count.")

        .def("render_realtime_frame", &PyApp::RenderRealtimeFrame,
             nb::arg("dt") = 1.0f / 60.0f,
             "Render one realtime output frame and advance animation by `dt`. Keeps NRD,\n"
             "TAA, or DLSS-RR temporal history according to the current settings.")

        .def("save_screenshot", &PyApp::SaveScreenshot,
             nb::arg("output_path"),
             "Save the current back buffer to PNG/JPG/BMP/TGA.")

        .def("get_framebuffer",
             [](PyApp& self, bool hdr) -> RenderSession::FramebufferLdr {
                 if (hdr)
                     throw std::runtime_error("get_framebuffer(hdr=True) is not implemented yet; use hdr=False for LDR RGBA8");
                 auto fb = self.GetFramebufferLdr();
                 if (!fb)
                     throw std::runtime_error("get_framebuffer failed: no rendered LDR texture (call step() first)");
                 return std::move(*fb);
             },
             nb::arg("hdr") = false,
             "Read back the current LDR final color as a Framebuffer.\n"
             "pixels are tightly packed RGBA8 (uint8), row-major, top-left origin.\n"
             "hdr=True is reserved for a future HDR path.")

        .def("get_pixels",
             [](PyApp& self, bool hdr) {
                 if (hdr)
                     throw std::runtime_error("get_pixels(hdr=True) is not implemented yet; use hdr=False for LDR RGBA8");
                 auto fb = self.GetFramebufferLdr();
                 if (!fb)
                     throw std::runtime_error("get_pixels failed: no rendered LDR texture (call step() first)");

                 auto* data = new std::vector<uint8_t>(std::move(fb->pixels));
                 const size_t height = fb->height;
                 const size_t width = fb->width;
                 nb::capsule owner(data, [](void* p) noexcept {
                     delete static_cast<std::vector<uint8_t>*>(p);
                 });
                 return nb::ndarray<nb::numpy, uint8_t, nb::shape<-1, -1, 4>, nb::c_contig, nb::device::cpu>(
                     data->data(), { height, width, 4 }, owner);
             },
             nb::arg("hdr") = false,
             "Read back the current LDR final color as a NumPy array of shape (H, W, 4), dtype=uint8 RGBA.\n"
             "Requires NumPy. Prefer get_framebuffer() if you only need raw bytes.")

        .def("set_camera", &PyApp::SetCamera,
             nb::arg("position"), nb::arg("direction"),
             nb::arg("up") = nb::make_tuple(0.0f, 1.0f, 0.0f),
             "Position the camera using world-space pos / dir / up triples.")

        .def("set_camera_fov", &PyApp::SetCameraFOV,
             nb::arg("vertical_fov_degrees"))

        .def("set_camera_intrinsics", &PyApp::SetCameraIntrinsics,
             nb::arg("fx"), nb::arg("fy"), nb::arg("cx"), nb::arg("cy"),
             nb::arg("width"), nb::arg("height"),
             "Set an off-center pinhole projection from pixel-space camera intrinsics.")

        .def("get_scene_bounds",
             [](PyApp& self) -> nb::object {
                 auto bbox = CurrentSceneBoundingBox(self.GetApp());
                 if (!bbox) return nb::none();
                 return nb::make_tuple(Float3ToTuple(bbox->m_mins), Float3ToTuple(bbox->m_maxs));
             },
             "Return the active scene's world-space ((min.xyz), (max.xyz)) AABB, or None.")

        .def_prop_ro("scene_bounds",
             [](PyApp& self) -> nb::object {
                 auto bbox = CurrentSceneBoundingBox(self.GetApp());
                 if (!bbox) return nb::none();
                 return nb::make_tuple(Float3ToTuple(bbox->m_mins), Float3ToTuple(bbox->m_maxs));
             },
             "World-space ((min.xyz), (max.xyz)) AABB covering all renderable\n"
             "content in the active scene, or ``None`` when no scene is loaded.\n"
             "Equivalent to ``self.app.scene_bounds`` / ``self.app.scene.bounds``.\n"
             "Refreshed automatically after every ``load_scene`` / ``load_mesh_file`` call.")

        .def_prop_ro("scene_bounds_center",
             [](PyApp& self) -> nb::object {
                 auto bbox = CurrentSceneBoundingBox(self.GetApp());
                 if (!bbox) return nb::none();
                 return Float3ToTuple(bbox->center());
             },
             "Center of `scene_bounds`, or ``None`` for an empty scene.")

        .def_prop_ro("scene_bounds_size",
             [](PyApp& self) -> nb::object {
                 auto bbox = CurrentSceneBoundingBox(self.GetApp());
                 if (!bbox) return nb::none();
                 return Float3ToTuple(bbox->diagonal());
             },
             "Diagonal extent (max - min) of `scene_bounds`, or ``None`` for an empty scene.")

        .def_prop_ro("scene",
             [](PyApp& self) {
                 App* app = self.GetApp();
                 return app ? caustica::activeScene(*app) : std::shared_ptr<Scene>{};
             },
             "Current loaded Scene, or None before a scene is available.")

        .def_prop_ro("app",
             [](PyApp& self) -> App* { return self.GetApp(); },
             nb::rv_policy::reference,
             "Underlying engine Sample. Prefer app.scene / app.settings / app.step().")

        .def_prop_ro("settings",
             [](PyApp& self) -> PathTracerSettings* {
                 App* app = self.GetApp();
                 return app ? caustica::settings(*app) : nullptr;
             },
             nb::rv_policy::reference,
             "Live `settings` mirror (same object as caustica.settings()).")

        .def("create_scene", &PyApp::CreateScene,
             nb::arg("builtin") = std::string("plane"),
             nb::arg("wait_until_ready") = true,
             nb::arg("timeout_seconds") = 600.0,
             nb::arg("warmup_frames") = 4,
             "Load a builtin primitive scene without recreating the Device.")

        .def("render",
             [](PyApp& self, float dt, nb::object cameras) {
                 if (!cameras.is_none() && nb::len(cameras) != 1)
                     throw std::runtime_error("App.render(cameras=...): multiple cameras are not implemented yet");
                 return self.Render(dt);
             },
             nb::arg("dt") = -1.0f,
             nb::arg("cameras") = nb::none(),
             "Step one frame and return a Frame. Multi-camera lists are not implemented yet.")

        .def("render_reference", &PyApp::RenderReference,
             nb::arg("spp") = 64,
             nb::arg("oidn") = true,
             nb::arg("max_frames") = 0,
             "Accumulate a reference frame and return a Frame.")

        .def("get_frame", &PyApp::GetFrame,
             "Return the last rendered LDR frame without stepping.")

        .def("get_rgb",
             [](PyApp& self, bool hdr) {
                 if (hdr)
                     throw std::runtime_error("get_rgb(hdr=True) is not implemented yet");
                 auto fb = self.GetFramebufferLdr();
                 if (!fb)
                     throw std::runtime_error("get_rgb failed: no rendered LDR texture (call step() or render() first)");
                 return FramebufferToNumpy(*fb);
             },
             nb::arg("hdr") = false,
             "Read back LDR RGBA8 as a NumPy (H, W, 4) uint8 array.")

        .def("get_depth", [](PyApp&) {
                throw std::runtime_error("App.get_depth() is not implemented yet");
            },
            "Reserved. Depth readback is not implemented yet.")

        .def("get_segmentation", [](PyApp&) {
                throw std::runtime_error("App.get_segmentation() is not implemented yet");
            },
            "Reserved. Segmentation readback is not implemented yet.")

        .def("__enter__", [](PyApp& self) -> PyApp* { return &self; },
             nb::rv_policy::reference)
        .def("__exit__",  [](PyApp& self, nb::object, nb::object, nb::object) -> bool {
             self.Close();
             return false;
        }, nb::arg().none(), nb::arg().none(), nb::arg().none());

    nb::class_<PyRenderer, PyApp>(m, "Renderer",
        "Compatibility wrapper: creates a Device and App together.\n"
        "Prefer Device + App when the GPU should outlive scene loads.")
        .def(nb::init<int, int, bool, bool, const std::string&, bool, const std::string&, bool, int>(),
             nb::arg("width") = 1920,
             nb::arg("height") = 1080,
             nb::arg("headless") = true,
             nb::arg("vulkan") = false,
             nb::arg("adapter") = "auto",
             nb::arg("debug") = false,
             nb::arg("scene") = std::string(),
             nb::arg("realtime") = false,
             nb::arg("accumulation_target") = 64);

    deviceClass.def("create_app",
        [](std::shared_ptr<caustica_py::PythonDevice> device,
           int width, int height, bool headless,
           const std::string& scene, bool realtime, int accumulationTarget) {
            return PyApp(std::move(device), width, height, headless, scene, realtime, accumulationTarget);
        },
        nb::arg("width") = 1920,
        nb::arg("height") = 1080,
        nb::arg("headless") = true,
        nb::arg("scene") = std::string("{\"entities\":[]}"),
        nb::arg("realtime") = false,
        nb::arg("accumulation_target") = 64,
        "Create an App on this Device. The GPU and surface are created on the first bind.");

    m.def("app", []() -> App* { return &RequireCurrentApp(); },
          nb::rv_policy::reference,
          "Return the engine Sample owned by the most recently created App or Renderer.");

    m.def("settings", []() -> PathTracerSettings* {
            return caustica::settings(RequireCurrentApp());
        },
          nb::rv_policy::reference,
          "Shortcut for the global settings (same as Renderer.settings).");

    m.def("builtin_scene_json", &caustica_py::BuiltinSceneJson,
          nb::arg("builtin_model") = std::string("plane_cube"),
          "Return a minimal inline scene JSON string for builtin primitive models\n"
          "('plane', 'cube', 'sphere', or 'plane_cube').");

    m.attr("MODE") = "extension";
}

#endif // CAUSTICA_WITH_PYTHON
