#pragma once

namespace caustica
{
    // Installs a SetUnhandledExceptionFilter that writes a symbolized stack
    // trace to 'caustica_crash.log' next to the module containing this code
    // (next to the exe for hosts, next to caustica.*.pyd for the extension).
    //
    // Note: fail-fast exceptions (heap corruption 0xC0000374 / fast-fail
    // 0xC0000409) bypass unhandled-exception filters by design; SEH faults
    // such as access violations are caught and symbolized.
    //
    // Safe to call multiple times; only the first call installs the filter.
    void installUnhandledExceptionLogger();
}
