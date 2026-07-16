#pragma once

// Compatibility umbrella for the former SceneApi god-facade.
// Prefer focused headers for new code:
//   AppResources.h, SceneQuery.h, SceneSpawn.h, CameraApi.h,
//   SceneLifecycle.h, RenderSessionApi.h, RenderFrameApi.h
// Plugin schedule entry points (updateCamera / prepareRenderFrame / ...) live in ScenePlugins.h.

#include <engine/AppResources.h>
#include <engine/SceneQuery.h>
#include <engine/SceneSpawn.h>
#include <engine/CameraApi.h>
#include <engine/SceneLifecycle.h>
#include <engine/RenderSessionApi.h>
#include <engine/RenderFrameApi.h>
#include <engine/ScenePlugins.h>
