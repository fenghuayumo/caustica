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
//   // bar.stop() called automatically in destructor
class ProgressBar
{
public:
    ProgressBar() = default;
    explicit ProgressBar(const char* windowText) { start(windowText); }
    ~ProgressBar() { stop(); }

    bool start(const char* windowText);
    void Set(int percentage);
    void stop();
    [[nodiscard]] bool Active() const;

private:
    mutable std::recursive_mutex m_mtx;
    int m_slot = -1;
};

// Register the active window handle (for centering progress bars).
// Pass the platform native handle when available; falls back to GetActiveWindow().
void helpersRegisterActiveWindow(void* nativeWindowHandle = nullptr);

// Returns the active window handle (for fullscreen detection, etc.).
// Windows only — returns NULL on other platforms.
void* helpersGetActiveWindow();

// Set non-interactive mode — progress bars are suppressed.
void helpersSetNonInteractive();

// Returns true if non-interactive mode is set.
bool helpersIsNonInteractive();

} // namespace caustica
