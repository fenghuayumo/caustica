#pragma once

// ENGINE-INTERNAL. Schedule-plan tests construct a GPU-less runtime.

#include <engine/App.h>

#include <memory>

namespace caustica::detail
{

[[nodiscard]] std::unique_ptr<App> createBareAppForTest();

} // namespace caustica::detail
