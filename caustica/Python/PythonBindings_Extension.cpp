// Extension-mode entry point: builds a real Python extension module
// (caustica.pyd / caustica.so) so that standalone Python interpreters can drive
// caustica for offline rendering and headless data generation.
//
//    python -c "import caustica; r = caustica.Renderer(headless=True); ..."

#if CAUSTICA_WITH_PYTHON

#include "PythonBindingsCore.h"
#include "RenderSession.h"

#include <engine/EntryPoint.h>
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/optional.h>

#include "SceneEditor.h"
#include <EditorUI.h>

#include <scene/Scene.h>
#include <math/box.h>
#include <math/math.h>

#include <stdexcept>

namespace nb = nanobind;
using caustica::editor::SceneEditor;
using caustica::render::RenderSessionState;

namespace
{
    // Tracks the most recently created Renderer instance so module-level
    // helpers like `caustica.app()` and `caustica.settings()` keep working.
    SceneEditor* g_currentExtensionApp = nullptr;

    SceneEditor& RequireCurrentApp()
    {
        if (!g_currentExtensionApp)
            throw std::runtime_error("caustica: no Renderer is currently active. Create one via caustica.Renderer(...)");
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
    std::optional<caustica::math::box3> CurrentSceneBoundingBox(SceneEditor* app)
    {
        if (!app)
            return std::nullopt;
        auto scene = app->GetScene();
        if (!scene)
            return std::nullopt;
        const caustica::math::box3 bbox = scene->GetSceneBounds();
        if (bbox.isempty() || !IsFiniteBox(bbox))
            return std::nullopt;
        return bbox;
    }
}

// Small wrapper holding RenderSession + automatic singleton tracking so the
// shared bindings (Material, Light, Settings, ...) keep functioning unchanged.
class PyRenderer
{
public:
    PyRenderer(int width, int height, bool headless, bool useVulkan,
               int adapterIndex, bool debug, const std::string& scene,
               bool realtimeMode, int accumulationTarget)
    {
        RenderSession::Config cfg;
        cfg.width              = width;
        cfg.height             = height;
        cfg.headless           = headless;
        cfg.useVulkan          = useVulkan;
        cfg.adapterIndex       = adapterIndex;
        cfg.debug              = debug;
        cfg.nonInteractive     = true;
        cfg.scene              = scene;
        cfg.realtimeMode       = realtimeMode;
        cfg.accumulationTarget = accumulationTarget;

        m_session = std::make_unique<RenderSession>(cfg);
        m_owned   = m_session->GetSceneEditor() != nullptr;
        if (!m_owned)
            throw std::runtime_error("caustica.Renderer: failed to initialize the renderer (see log for details)");
        g_currentExtensionApp = m_session->GetSceneEditor();
    }

    ~PyRenderer()
    {
        Close();
    }

    void Close()
    {
        if (m_session)
        {
            if (g_currentExtensionApp == m_session->GetSceneEditor())
                g_currentExtensionApp = nullptr;
            m_session.reset();
            m_owned = false;
        }
    }

    bool LoadScene(const std::string& sceneName, bool wait) {
        return m_session ? m_session->LoadScene(sceneName, wait) : false;
    }

    bool LoadGaussianSplats(const std::string& fileName, bool convertRdfToRub) {
        return m_session && m_session->GetSceneEditor()
            ? m_session->GetSceneEditor()->LoadGaussianSplatFile(fileName, convertRdfToRub)
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

    bool SaveScreenshot(const std::string& path) {
        return m_session ? m_session->SaveScreenshot(path) : false;
    }

    bool SetCamera(nb::object pos, nb::object dir, nb::object up) {
        if (!m_session) return false;
        return m_session->SetCamera(ToFloat3(pos), ToFloat3(dir), ToFloat3(up));
    }

    bool LoadMeshFile(const std::string& fileName) {
        SceneEditor* app = GetApp();
        return app ? app->LoadMeshFile(fileName) : false;
    }

    void SetCameraFOV(float fov) {
        if (m_session) m_session->SetCameraFOV(fov);
    }

    void SetCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height) {
        if (m_session) m_session->SetCameraIntrinsics(fx, fy, cx, cy, width, height);
    }

    SceneEditor* GetApp() {
        return m_session ? m_session->GetSceneEditor() : nullptr;
    }

    bool IsValid() const { return m_session && m_session->GetSceneEditor() != nullptr; }

private:
    std::unique_ptr<RenderSession> m_session;
    bool m_owned = false;
};

NB_MODULE(caustica, m)
{
    m.doc() = "caustica Python extension - drive the path-tracer for offline rendering.";

    caustica_py::RegisterCoreBindings(m);

    nb::class_<PyRenderer>(m, "Renderer",
        "Standalone path-tracer renderer.  Each instance owns its own GPU\n"
        "device (DX12 / Vulkan), shaders, scene, and back buffer.  In headless\n"
        "mode rendering uses offscreen back buffers without creating an OS window.\n\n"
        "Example:\n"
        "    import caustica\n"
        "    r = caustica.Renderer(width=1280, height=720, headless=True,\n"
        "                       scene='builtin:plane_cube')\n"
        "    r.settings.accumulation_target = 256\n"
        "    r.step_until_accumulated()\n"
        "    r.save_screenshot('frame.png')\n"
        "    r.close()")
        .def(nb::init<int, int, bool, bool, int, bool, const std::string&, bool, int>(),
             nb::arg("width") = 1920,
             nb::arg("height") = 1080,
             nb::arg("headless") = true,
             nb::arg("vulkan") = false,
             nb::arg("adapter_index") = -1,
             nb::arg("debug") = false,
             nb::arg("scene") = std::string(),
             nb::arg("realtime") = false,
             nb::arg("accumulation_target") = 64)

        .def("close", &PyRenderer::Close,
             "Tear down the GPU device, scene and back buffer.  Called automatically\n"
             "by the destructor but can be invoked explicitly to free GPU memory before\n"
             "the Python process exits.")

        .def("load_scene",
             [](PyRenderer& self, const std::string& name, bool wait) { return self.LoadScene(name, wait); },
             nb::arg("scene_name"), nb::arg("wait_until_ready") = true,
             "Load a scene by name, builtin primitive reference, or inline scene JSON string.")

        .def("load_gaussian_splats",
             [](PyRenderer& self, const std::string& fileName, bool convertRdfToRub) {
                 return self.LoadGaussianSplats(fileName, convertRdfToRub);
             },
             nb::arg("file_name"), nb::arg("convert_rdf_to_rub") = true,
             "Append a 3DGS .ply file as a GaussianSplat entity in the current scene.")

        .def("load_mesh_file", &PyRenderer::LoadMeshFile,
             nb::arg("file_name"),
             "Append a mesh file (.gltf, .glb, or .obj) to the current scene.")

        .def("step", &PyRenderer::Step,
             nb::arg("dt") = -1.0f,
             "Render exactly one frame.  Returns True on success.")

        .def("step_n", &PyRenderer::StepN,
             nb::arg("frames"),
             "Render N frames.")

        .def("step_until_accumulated", &PyRenderer::StepUntilAccumulated,
             nb::arg("max_frames") = 0,
             "Reset accumulation and keep stepping until the SPP target is reached\n"
             "(or `max_frames` frames have been produced if positive).")

        .def("save_screenshot", &PyRenderer::SaveScreenshot,
             nb::arg("output_path"),
             "Save the current back buffer to PNG/JPG/BMP/TGA.")

        .def("set_camera", &PyRenderer::SetCamera,
             nb::arg("position"), nb::arg("direction"),
             nb::arg("up") = nb::make_tuple(0.0f, 1.0f, 0.0f),
             "Position the camera using world-space pos / dir / up triples.")

        .def("set_camera_fov", &PyRenderer::SetCameraFOV,
             nb::arg("vertical_fov_degrees"))

        .def("set_camera_intrinsics", &PyRenderer::SetCameraIntrinsics,
             nb::arg("fx"), nb::arg("fy"), nb::arg("cx"), nb::arg("cy"),
             nb::arg("width"), nb::arg("height"),
             "Set an off-center pinhole projection from pixel-space camera intrinsics.")

        .def("get_scene_bounds",
             [](PyRenderer& self) -> nb::object {
                 auto bbox = CurrentSceneBoundingBox(self.GetApp());
                 if (!bbox) return nb::none();
                 return nb::make_tuple(Float3ToTuple(bbox->m_mins), Float3ToTuple(bbox->m_maxs));
             },
             "Return the active scene's world-space ((min.xyz), (max.xyz)) AABB, or None.")

        .def_prop_ro("scene_bounds",
             [](PyRenderer& self) -> nb::object {
                 auto bbox = CurrentSceneBoundingBox(self.GetApp());
                 if (!bbox) return nb::none();
                 return nb::make_tuple(Float3ToTuple(bbox->m_mins), Float3ToTuple(bbox->m_maxs));
             },
             "World-space ((min.xyz), (max.xyz)) AABB covering all renderable\n"
             "content in the active scene, or ``None`` when no scene is loaded.\n"
             "Equivalent to ``self.app.scene_bounds`` / ``self.app.scene.bounds``.\n"
             "Refreshed automatically after every ``load_scene`` / ``load_mesh_file`` call.")

        .def_prop_ro("scene_bounds_center",
             [](PyRenderer& self) -> nb::object {
                 auto bbox = CurrentSceneBoundingBox(self.GetApp());
                 if (!bbox) return nb::none();
                 return Float3ToTuple(bbox->center());
             },
             "Center of `scene_bounds`, or ``None`` for an empty scene.")

        .def_prop_ro("scene_bounds_size",
             [](PyRenderer& self) -> nb::object {
                 auto bbox = CurrentSceneBoundingBox(self.GetApp());
                 if (!bbox) return nb::none();
                 return Float3ToTuple(bbox->diagonal());
             },
             "Diagonal extent (max - min) of `scene_bounds`, or ``None`` for an empty scene.")

        .def_prop_ro("app",
             [](PyRenderer& self) -> SceneEditor* { return self.GetApp(); },
             nb::rv_policy::reference,
             "Access the underlying Sample instance to use the shared bindings.")

        .def_prop_ro("settings",
             [](PyRenderer& self) -> PathTracerSettings* {
                 SceneEditor* app = self.GetApp();
                 return app ? &app->GetRenderSessionState().settings : nullptr;
             },
             nb::rv_policy::reference,
             "Live `Settings` mirror (same object as caustica.settings()).")

        .def("__enter__", [](PyRenderer& self) -> PyRenderer* { return &self; },
             nb::rv_policy::reference)
        .def("__exit__",  [](PyRenderer& self, nb::object, nb::object, nb::object) -> bool {
             self.Close();
             return false;
        }, nb::arg().none(), nb::arg().none(), nb::arg().none());

    m.def("app", []() -> SceneEditor* { return &RequireCurrentApp(); },
          nb::rv_policy::reference,
          "Return the Sample owned by the most recently created Renderer.");

    m.def("settings", []() -> PathTracerSettings* {
            SceneEditor& app = RequireCurrentApp();
            return &app.GetRenderSessionState().settings;
        },
          nb::rv_policy::reference,
          "Shortcut for the global Settings (same as Renderer.settings).");

    m.def("builtin_scene_json", &caustica_py::BuiltinSceneJson,
          nb::arg("builtin_model") = std::string("plane_cube"),
          "Return a minimal inline scene JSON string for builtin primitive models\n"
          "('plane', 'cube', 'sphere', or 'plane_cube').");

    m.attr("MODE") = "extension";
}

// caustica_py links causEngine (EntryPoint) but not the editor executable entry.
namespace caustica
{
Application* createApplication() { return nullptr; }
}

#endif // CAUSTICA_WITH_PYTHON
