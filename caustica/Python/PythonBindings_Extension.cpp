// Extension-mode entry point: standalone `import caustica` drives EngineApp.

#if CAUSTICA_WITH_PYTHON

#include "PythonBindingsCore.h"
#include "PythonDevice.h"
#include "PythonEngineApp.h"
#include "RenderSession.h"

#include <caustica/version.h>

#include <engine/EngineApp.h>
#include <engine/SensorApi.h>
#include <backend/GpuDevice.h>

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/vector.h>

#include <memory>
#include <stdexcept>
#include <vector>

namespace nb = nanobind;

namespace
{
    nb::ndarray<nb::numpy, uint8_t, nb::shape<-1, -1, 4>, nb::c_contig, nb::device::cpu>
    FramebufferToNumpy(const caustica::LdrFramebuffer& fb)
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
        caustica::SensorOutput sensor;
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

    caustica_py::PyEngineApp MakeEngineApp(
        std::shared_ptr<caustica_py::PythonDevice> device,
        int width, int height, bool headless,
        bool vulkan, const std::string& adapter, bool debug,
        const std::string& scene, bool realtime, int accumulationTarget)
    {
        if (!device)
            device = MakePythonDevice(vulkan, adapter, debug);

        RenderSession::Config cfg;
        cfg.width = width;
        cfg.height = height;
        cfg.headless = headless;
        cfg.useVulkan = device->useVulkan();
        cfg.adapter = device->config().adapter;
        cfg.debug = device->config().debug;
        cfg.nonInteractive = true;
        cfg.scene = scene;
        cfg.realtimeMode = realtime;
        cfg.accumulationTarget = accumulationTarget;
        return caustica_py::PyEngineApp(std::move(device), cfg);
    }

    caustica::LdrFramebuffer RequireLdrFramebuffer(caustica_py::PyEngineApp& self)
    {
        auto fb = self.engine().readLdrFramebuffer();
        if (!fb)
            throw std::runtime_error("no rendered LDR texture (call step_frame() first)");
        return std::move(*fb);
    }
}

NB_MODULE(caustica, m)
{
    m.doc() = "caustica Python extension - snake_case projection of C++ EngineApp.";
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
        "Enumerate GPU adapters for DX12 or Vulkan without creating an EngineApp.");

    auto deviceClass = nb::class_<caustica_py::PythonDevice>(m, "GpuDevice",
        "GPU handle matching C++ GpuDevice injection into EngineAppDesc.\n"
        "Size / headless are applied when the first EngineApp binds.")
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
             "Release the GPU. Fails if an EngineApp is still bound.")
        .def("__enter__", [](caustica_py::PythonDevice& self) -> caustica_py::PythonDevice* { return &self; },
             nb::rv_policy::reference)
        .def("__exit__", [](caustica_py::PythonDevice& self, nb::object, nb::object, nb::object) -> bool {
             self.close();
             return false;
        }, nb::arg().none(), nb::arg().none(), nb::arg().none())
        .def("__repr__", [](const caustica_py::PythonDevice& self) {
            return std::string("<caustica.GpuDevice created=") + (self.isCreated() ? "True" : "False")
                + " bound=" + (self.isBound() ? "True" : "False") + ">";
        });

    nb::class_<PyFrame>(m, "Frame",
        "One rendered output. rgb is LDR RGBA8. depth is linear |view Z|. segmentation aliases instance_id.")
        .def_prop_ro("width", [](const PyFrame& self) { return self.sensor.width; })
        .def_prop_ro("height", [](const PyFrame& self) { return self.sensor.height; })
        .def_prop_ro("channels", [](const PyFrame&) { return 4; })
        .def_prop_ro("pixels", [](const PyFrame& self) {
                return nb::bytes(self.sensor.rgb.data(), self.sensor.rgb.size());
            })
        .def_prop_ro("rgb", [](const PyFrame& self) { return caustica_py::sensorRgbNumpy(self.sensor); },
            "NumPy (H, W, 4) uint8 RGBA. Requires NumPy.")
        .def_prop_ro("depth", [](const PyFrame& self) { return caustica_py::sensorDepthNumpy(self.sensor); },
            "NumPy (H, W) float32 linear |view Z| meters. 0 = miss.")
        .def_prop_ro("normal", [](const PyFrame& self) { return caustica_py::sensorNormalNumpy(self.sensor); },
            "NumPy (H, W, 3) float32 camera-space normals.")
        .def_prop_ro("instance_id", [](const PyFrame& self) { return caustica_py::sensorInstanceIdNumpy(self.sensor); },
            "NumPy (H, W) uint32. 0 = miss.")
        .def_prop_ro("semantic_id", [](const PyFrame& self) { return caustica_py::sensorSemanticIdNumpy(self.sensor); },
            "NumPy (H, W) uint32. 0 = unlabeled / miss.")
        .def_prop_ro("segmentation", [](const PyFrame& self) { return caustica_py::sensorInstanceIdNumpy(self.sensor); },
            "Alias of instance_id.")
        .def_prop_ro("motion_vector", [](const PyFrame& self) { return caustica_py::sensorMotionVectorNumpy(self.sensor); },
            "NumPy (H, W, 2) float32 screen-space motion in pixels.")
        .def("__repr__", [](const PyFrame& self) {
            return std::string("<caustica.Frame ")
                + std::to_string(self.sensor.width) + "x" + std::to_string(self.sensor.height) + ">";
        });

    nb::class_<caustica::LdrFramebuffer>(m, "Framebuffer",
        "CPU-side LDR framebuffer from EngineApp.read_ldr_framebuffer().")
        .def_ro("width", &caustica::LdrFramebuffer::width)
        .def_ro("height", &caustica::LdrFramebuffer::height)
        .def_ro("channels", &caustica::LdrFramebuffer::channels)
        .def_prop_ro("format", [](const caustica::LdrFramebuffer&) { return "RGBA8"; })
        .def_prop_ro("dtype", [](const caustica::LdrFramebuffer&) { return "uint8"; })
        .def_prop_ro("pixels", [](const caustica::LdrFramebuffer& self) {
                return nb::bytes(self.pixels.data(), self.pixels.size());
            })
        .def_prop_ro("shape", [](const caustica::LdrFramebuffer& self) {
                return nb::make_tuple(self.height, self.width, self.channels);
            })
        .def("__len__", [](const caustica::LdrFramebuffer& self) { return self.pixels.size(); })
        .def("__repr__", [](const caustica::LdrFramebuffer& self) {
                return std::string("<caustica.Framebuffer ")
                    + std::to_string(self.width) + "x" + std::to_string(self.height)
                    + " RGBA8, " + std::to_string(self.pixels.size()) + " bytes>";
            });

    auto engineClass = nb::class_<caustica_py::PyEngineApp>(m, "EngineApp",
        "Host entry matching C++ caustica::EngineApp. Python methods are snake_case.\n\n"
        "    engine = caustica.EngineApp.create(width=1280, height=720, headless=True,\n"
        "                                       scene='builtin:plane_cube')\n"
        "    engine.step_until_accumulated()\n"
        "    engine.save_screenshot('frame.png')");
    caustica_py::BindEngineApp(engineClass);

    engineClass
        .def_static("create",
            [](std::shared_ptr<caustica_py::PythonDevice> device,
               int width, int height, bool headless,
               bool vulkan, const std::string& adapter, bool debug,
               const std::string& scene, bool realtime, int accumulationTarget) {
                return MakeEngineApp(std::move(device), width, height, headless,
                                     vulkan, adapter, debug, scene, realtime, accumulationTarget);
            },
            nb::arg("device") = std::shared_ptr<caustica_py::PythonDevice>(),
            nb::arg("width") = 1920,
            nb::arg("height") = 1080,
            nb::arg("headless") = true,
            nb::arg("vulkan") = false,
            nb::arg("adapter") = "auto",
            nb::arg("debug") = false,
            nb::arg("scene") = std::string("{\"entities\":[]}"),
            nb::arg("realtime") = false,
            nb::arg("accumulation_target") = 64,
            "Create an EngineApp. Pass device= to reuse a GpuDevice across scene loads.")
        .def_prop_ro("device", [](const caustica_py::PyEngineApp& self) { return self.device(); })
        .def_prop_ro("selected_adapter", [](const caustica_py::PyEngineApp& self)
            -> std::optional<caustica::AdapterInfo> {
                if (auto device = self.device())
                    return device->selectedAdapter();
                return std::nullopt;
            })
        .def("read_ldr_framebuffer",
             [](caustica_py::PyEngineApp& self) {
                 return RequireLdrFramebuffer(self);
             })
        .def("get_pixels",
             [](caustica_py::PyEngineApp& self) {
                 return FramebufferToNumpy(RequireLdrFramebuffer(self));
             },
             "Python sugar: LDR readback as a NumPy (H, W, 4) uint8 array.")
        .def("render",
             [](caustica_py::PyEngineApp& self, float dt) {
                 if (!self.step(dt))
                     throw std::runtime_error("EngineApp.render: step_frame failed");
                 auto output = self.engine().readSensorOutput();
                 if (!output)
                     throw std::runtime_error("EngineApp.render: sensor readback failed");
                 return PyFrame{ std::move(*output) };
             },
             nb::arg("dt") = -1.0f,
             "Python sugar: step_frame() then return a Frame with RGB and AOV buffers.")
        .def("render_reference",
             [](caustica_py::PyEngineApp& self, int spp, bool oidn, int maxFrames) {
                 if (self.renderReferenceFrame(spp, oidn, maxFrames) <= 0)
                     throw std::runtime_error("EngineApp.render_reference: accumulation failed");
                 auto output = self.engine().readSensorOutput();
                 if (!output)
                     throw std::runtime_error("EngineApp.render_reference: sensor readback failed");
                 return PyFrame{ std::move(*output) };
             },
             nb::arg("spp") = 64, nb::arg("oidn") = true, nb::arg("max_frames") = 0,
             "Python sugar: accumulate a reference frame and return a Frame.");

    m.def("engine", []() -> caustica_py::PyEngineApp* {
            caustica_py::PyEngineApp* current = caustica_py::currentEngineApp();
            if (!current)
                throw std::runtime_error("caustica: no EngineApp is currently active");
            return current;
        },
        nb::rv_policy::reference,
        "Return the most recently created EngineApp.");
    m.def("settings", []() -> PathTracerSettings* {
            caustica_py::PyEngineApp* current = caustica_py::currentEngineApp();
            if (!current)
                throw std::runtime_error("caustica: no EngineApp is currently active");
            return &current->engine().settings();
        },
        nb::rv_policy::reference,
        "Shortcut for caustica.engine().settings.");
    m.def("builtin_scene_json", &caustica_py::BuiltinSceneJson,
          nb::arg("builtin_model") = std::string("plane_cube"));

    m.attr("MODE") = "extension";
}

#endif // CAUSTICA_WITH_PYTHON
