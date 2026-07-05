#include <engine/EngineScheduleRegistration.h>

#include <engine/App.h>

namespace caustica
{

void registerEngineScheduleBridge(App& app)
{
    app.registerDefaultSchedules();
}

} // namespace caustica
