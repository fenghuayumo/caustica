#pragma once

#include <scene/camera/Camera.h>
#include <scene/View.h>
#include <render/passes/geometry/TemporalAntiAliasingPass.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct PathTracerSettings;

namespace caustica::render
{
class WorldRenderer;
}

namespace caustica
{

struct CameraUpdateParams
{
    dm::uint2   renderSize          = {};
    float       displayAspectRatio  = 1.0f;
    uint32_t    sampleIndex         = 0;
    uint64_t    frameIndex          = 0;
    bool        realtimeMode        = false;
    int         realtimeAA          = 0;
    bool        dbgFreezeRealtimeNoiseSeed = false;
    bool        syncPreviousView    = false;
    caustica::render::TemporalAntiAliasingJitter temporalAAJitter =
        caustica::render::TemporalAntiAliasingJitter::R2;
    caustica::render::TemporalAntiAliasingPass* temporalAAPass = nullptr;
};

// First-person camera, planar views, and file/string pose import.
// Scene cameras sync via Extract → CameraRenderProxy → applyCameraRenderProxyToController.
// Optional bindSideEffects() wires UI/API FOV changes to accumulation reset.
class CameraController
{
public:
    CameraController();

    FirstPersonCamera& camera() { return m_camera; }
    [[nodiscard]] const FirstPersonCamera& camera() const { return m_camera; }

    std::shared_ptr<PlanarView>& view() { return m_view; }
    std::shared_ptr<PlanarView>& viewPrevious() { return m_viewPrevious; }
    [[nodiscard]] const std::shared_ptr<PlanarView>& view() const { return m_view; }
    [[nodiscard]] const std::shared_ptr<PlanarView>& viewPrevious() const { return m_viewPrevious; }

    void ensureViews(dm::uint2 renderSize);

    void swapViews();

    // Bind path-tracer side effects for interactive FOV/intrinsics changes.
    void bindSideEffects(::PathTracerSettings& settings, caustica::render::WorldRenderer* worldRenderer);
    void markCameraChanged();

    [[nodiscard]] float verticalFOV() const { return m_verticalFOV; }
    void setVerticalFOV(float fov);
    // Like setVerticalFOV but also markCameraChanged when side effects are bound.
    void setVerticalFOVInteractive(float fov);

    [[nodiscard]] float zNear() const { return m_zNear; }
    void setZNear(float zNear) { m_zNear = zNear; }

    [[nodiscard]] float zFar() const { return m_zFar; }

    [[nodiscard]] bool useCustomIntrinsics() const { return m_useCustomIntrinsics; }
    [[nodiscard]] dm::float4 intrinsics() const { return m_intrinsics; }
    [[nodiscard]] dm::float2 intrinsicsViewport() const { return m_intrinsicsViewport; }

    void setIntrinsics(float fx, float fy, float cx, float cy, float w, float h);
    void setIntrinsicsInteractive(float fx, float fy, float cx, float cy, float w, float h);
    void clearIntrinsics();
    void clearIntrinsicsInteractive();

    [[nodiscard]] uint32_t selectedCameraIndex() const { return m_selectedCameraIndex; }
    void setSelectedCameraIndex(uint32_t index) { m_selectedCameraIndex = index; }
    uint32_t& selectedCameraIndex() { return m_selectedCameraIndex; }

    [[nodiscard]] dm::float2 computeJitter(const CameraUpdateParams& params) const;

    void updateViews(const CameraUpdateParams& params);
    void syncPreviousViewFromCurrent();

    void saveToFile(const std::filesystem::path& path, float zNear, float fovYRadians) const;
    void loadFromFile(const std::filesystem::path& path);
    // Default campos.txt next to the executable (FOV from current projection).
    void saveToDefaultFile() const;
    void loadFromDefaultFile();

    [[nodiscard]] std::string getPosDirUpString() const;
    bool setFromPosDirUpString(const std::string& val);

    void setupDefaultCamera();

    [[nodiscard]] bool cameraMovedSinceLastFrame() const;
    void updateLastCameraState();

private:
    FirstPersonCamera               m_camera;
    std::shared_ptr<PlanarView>     m_view;
    std::shared_ptr<PlanarView>     m_viewPrevious;

    float                           m_verticalFOV = 60.0f;
    float                           m_zNear       = 0.001f;
    float                           m_zFar        = 100000.0f;
    bool                            m_useCustomIntrinsics = false;
    dm::float4                      m_intrinsics = dm::float4(0.f);
    dm::float2                      m_intrinsicsViewport = dm::float2(0.f);

    dm::float3                      m_lastPos = { 0, 0, 0 };
    dm::float3                      m_lastDir = { 0, 0, 0 };
    dm::float3                      m_lastUp  = { 0, 0, 0 };

    uint32_t                        m_selectedCameraIndex = 0;

    ::PathTracerSettings* m_settings = nullptr;
    caustica::render::WorldRenderer* m_worldRenderer = nullptr;
};

} // namespace caustica
