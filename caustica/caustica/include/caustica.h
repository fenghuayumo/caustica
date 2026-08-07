#pragma once

// Public C++ embedding surface for Caustica.
//
// Prefer this single include in new applications. Do not include
// engine/internal/*, DefaultPlugins, WorldRenderer, SceneManager,
// LoadSession, RenderThread, or GpuSharedCaches from application code.
//
// Contract: docs/public-api.md
// Reference app: application/samples/thin_client/Main.cpp

#include <engine/EngineApp.h>
#include <engine/EntityWorld.h>
#include <engine/Plugin.h>
#include <engine/MeshDeformApi.h>
#include <engine/RenderSessionApi.h>
#include <engine/CameraApi.h>
#include <engine/SceneLifecycle.h>
#include <engine/SceneQuery.h>
#include <engine/SceneSpawn.h>
#include <engine/SceneTransform.h>
#include <engine/EnqueueRenderCommand.h>
#include <engine/SystemSets.h>
#include <engine/EntryPoint.h>
