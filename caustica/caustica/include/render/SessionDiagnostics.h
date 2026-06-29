#pragma once

#include <core/progress.h>

#include <chrono>

namespace caustica::render
{

// Host-owned session state consumed by the path tracer (progress UI, benchmarks, async load).
struct SessionDiagnostics
{
    ProgressBar progressInitializingRenderer;
    bool asyncLoadingInProgress = false;

    std::chrono::high_resolution_clock::time_point benchStart = std::chrono::high_resolution_clock::now();
    std::chrono::high_resolution_clock::time_point benchLast = std::chrono::high_resolution_clock::now();
    int benchFrames = 0;
};

} // namespace caustica::render
