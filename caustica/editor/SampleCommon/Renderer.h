#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <memory>
#include <rhi/nvrhi.h>
#include <math/math.h>
#include <render/Core/CameraController.h>

class PathTracerApp;
#include <scene/SceneGraph.h>  // for GaussianSplat
class GaussianSplatPass;
struct SampleConstants;

namespace caustica {
class PerspectiveCamera;
class IView;
}

// =============================================================================
// Renderer — Method bodies extracted from Sample.
// =============================================================================
class Renderer
{
public:
    using RenderCodeFn = std::function<void(nvrhi::IFramebuffer*, nvrhi::CommandListHandle, const SampleConstants&)>;

    explicit Renderer(PathTracerApp& owner);
    ~Renderer();

    // Callbacks
    RenderCodeFn renderCodeCallback;
    std::function<void()> createRTPipelinesCb;
    std::function<void()> destroyRTPipelinesCb;

    // --- Camera ---
    dm::float2 computeCameraJitter(uint32_t frameIndex) const;
    void updateCameraFromScene(const std::shared_ptr<caustica::PerspectiveCamera>& cam);
    void updateViews(nvrhi::IFramebuffer* framebuffer);
    void saveCurrentCamera() const;
    void loadCurrentCamera();
    std::string getCurrentCameraPosDirUp() const;
    bool setCurrentCameraPosDirUp(const std::string& val);
    void setCameraVerticalFOV(float fov);
    void setCameraIntrinsics(float fx, float fy, float cx, float cy, float w, float h);
    void clearCameraIntrinsics();

    // --- Acceleration structures ---
    void transitionMeshBuffersToReadOnly();

    // --- Utilities ---
    std::string getResolutionInfo() const;
    float getAvgTimePerFrame() const;
    void debugDrawLine(dm::float3 start, dm::float3 stop, dm::float4 col1, dm::float4 col2);

    PathTracerApp& owner() { return m_owner; }

private:
    [[nodiscard]] caustica::CameraUpdateParams makeCameraUpdateParams() const;

    PathTracerApp& m_owner;
};
