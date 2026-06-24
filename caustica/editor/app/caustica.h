#pragma once

#include "PathTracerApp.h"

// Backward-compatible alias used by editor UI, Python bindings, and game sample code.
// New code should prefer PathTracerApp and PathTracerSettings/EditorUIState directly.
class Sample : public PathTracerApp
{
public:
    using PathTracerApp::PathTracerApp;
    ~Sample() override = default;
};
