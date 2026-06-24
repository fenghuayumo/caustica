#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace caustica
{

// Progress bar helper — shows a native OS progress window during long operations.
// Usage:
//   ProgressBar bar("Loading scene...");
//   bar.Set(50);
//   // ... work ...
//   bar.Set(100);
//   // bar.Stop() called automatically in destructor
class ProgressBar
{
public:
    ProgressBar() = default;
    explicit ProgressBar(const char* windowText) { Start(windowText); }
    ~ProgressBar() { Stop(); }

    bool Start(const char* windowText);
    void Set(int percentage);
    void Stop();
    [[nodiscard]] bool Active() const;

private:
    mutable std::recursive_mutex m_mtx;
    int m_slot = -1;
};

// Register the active window handle (for centering progress bars).
// Call from the main thread after window creation.
void HelpersRegisterActiveWindow();

// Returns the active window handle (for fullscreen detection, etc.).
// Windows only — returns NULL on other platforms.
void* HelpersGetActiveWindow();

// Set non-interactive mode — progress bars are suppressed.
void HelpersSetNonInteractive();

// Returns true if non-interactive mode is set.
bool HelpersIsNonInteractive();

} // namespace caustica
