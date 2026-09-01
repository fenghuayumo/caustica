#pragma once

class LightSamplingCache;
class EnvMapProcessor;
class ProceduralSky;
class OpacityMicromapBuilder;
class ZoomTool;

namespace caustica::scene { class SceneRenderData; }

namespace caustica::editor
{

// Pass debug / tuning widgets. Render exposes state; editor owns ImGui.
bool DrawLightSamplingInfo(LightSamplingCache& cache);
bool DrawLightSamplingDebug(LightSamplingCache& cache, float indent);
bool DrawEnvMapProcessorDebug(EnvMapProcessor& env, float indent);
bool DrawProceduralSkyDebug(ProceduralSky& sky, float indent);
bool DrawOpacityMicromapDebug(
    OpacityMicromapBuilder& builder,
    const caustica::scene::SceneRenderData& renderData,
    float indent);
bool DrawZoomToolDebug(ZoomTool& zoom);

} // namespace caustica::editor
