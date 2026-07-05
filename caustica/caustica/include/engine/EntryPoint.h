#pragma once

#include <functional>

namespace caustica
{

class App;

using AppHook = void (*)();

int runApp(App& app, const std::function<bool(App&)>& startup, AppHook preGpuInit = nullptr);

void InvokePreGpuDeviceInitHook();

} // namespace caustica
