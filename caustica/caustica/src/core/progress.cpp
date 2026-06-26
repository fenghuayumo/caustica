#include <core/progress.h>
#include <core/log.h>

#include <atomic>
#include <cassert>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "Comctl32.lib")
#endif

namespace caustica
{

// --- globals ---
namespace
{
    constexpr int g_ProgressSlotCount = 8;

    struct ProgressBarGlobals
    {
        bool                Active = false;
        std::string         Title;
        int                 Value = 0;
#ifdef _WIN32
        HWND                hMainWindow = nullptr;
        HWND                hProgressBar = nullptr;
#endif
        std::recursive_mutex Mutex;
        std::thread         Thread;
    };

    ProgressBarGlobals g_ProgressSlots[g_ProgressSlotCount];
    bool g_NonInteractive = false;

#ifdef _WIN32
    std::atomic<HWND> g_hActiveParentWindow(NULL);

    void ShowLastError()
    {
        DWORD err = GetLastError();
        wchar_t buf[4096];
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, err, 0, buf, sizeof(buf) / sizeof(wchar_t), NULL);
        MessageBoxW(NULL, buf, L"LastError", MB_OK | MB_ICONERROR);
    }

    LRESULT CALLBACK ProgressWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
            ProgressBarGlobals& slot = *reinterpret_cast<ProgressBarGlobals*>(cs->lpCreateParams);
            std::lock_guard guard(slot.Mutex);
            assert(slot.Active);
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            slot.hProgressBar = CreateWindowEx(0, PROGRESS_CLASS, NULL, WS_CHILD | WS_VISIBLE,
                0, 0, clientRect.right, clientRect.bottom, hwnd, NULL, GetModuleHandle(NULL), NULL);
            SendMessage(slot.hProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            SendMessage(slot.hProgressBar, PBM_SETPOS, 0, 0);
            return 0;
        }
        case WM_CLOSE:   DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0);  return 0;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    bool IsWindowFullscreen()
    {
        HWND activeWindow = g_hActiveParentWindow.load();
        if (!IsWindow(activeWindow)) return false;

        RECT windowRect;
        if (!GetWindowRect(activeWindow, &windowRect)) return false;

        HMONITOR hMonitor = MonitorFromWindow(activeWindow, MONITOR_DEFAULTTONEAREST);
        if (!hMonitor) return false;

        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfo(hMonitor, &mi)) return false;

        return EqualRect(&windowRect, &mi.rcMonitor);
    }

    void ShowProgressWindow(int slotIndex)
    {
        ProgressBarGlobals& slot = g_ProgressSlots[slotIndex];

        const char* wClassName = "ProgressWindowClass";
        HINSTANCE hInstance = GetModuleHandle(NULL);
        static std::mutex registerClassMtx;
        static bool registerClassFlag = false;
        {
            std::lock_guard<std::mutex> lock(registerClassMtx);
            if (!registerClassFlag)
            {
                INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_PROGRESS_CLASS };
                InitCommonControlsEx(&icex);
                WNDCLASS wc = {};
                wc.lpfnWndProc = ProgressWndProc;
                wc.hInstance = hInstance;
                wc.lpszClassName = wClassName;
                wc.hCursor = LoadCursor(NULL, IDC_ARROW);
                if (!RegisterClass(&wc))
                    ShowLastError();
                registerClassFlag = true;
            }
        }

        int width = 600;
        int height = 100;
        int x = CW_USEDEFAULT;
        int y = CW_USEDEFAULT;

        HWND hParent = g_hActiveParentWindow.load();
        HWND hwnd = NULL;
        {
            std::lock_guard guard(slot.Mutex);
            if (!slot.Active)
                return;

            if (hParent)
            {
                RECT parentRect;
                GetWindowRect(hParent, &parentRect);
                x = parentRect.left + (parentRect.right - parentRect.left - width) / 2;
                y = parentRect.top + (parentRect.bottom - parentRect.top - height * 3) / 2;
                y += height * slotIndex;
            }

            slot.hMainWindow = CreateWindowEx(WS_EX_TOOLWINDOW, wClassName, slot.Title.c_str(),
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, width, height,
                NULL, NULL, hInstance, reinterpret_cast<LPVOID>(&slot));

            if (slot.hMainWindow == NULL)
            {
                ShowLastError();
                assert(false);
                return;
            }
            hwnd = slot.hMainWindow;
        }

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        while (true)
        {
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                    return;
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            {
                std::lock_guard guard(slot.Mutex);
                if (!slot.Active)
                    break;
                PostMessage(slot.hProgressBar, PBM_SETPOS, slot.Value, 0);
            }
            Sleep(16);
        }
        SendMessage(hwnd, WM_CLOSE, 0, 0);
    }

    int ProgressBarStartImpl(const char* windowText)
    {
        if (g_NonInteractive || IsWindowFullscreen())
            return -1;

        for (int i = 0; i < g_ProgressSlotCount; i++)
        {
            std::lock_guard guard(g_ProgressSlots[i].Mutex);
            ProgressBarGlobals& slot = g_ProgressSlots[i];
            if (!slot.Active)
            {
                slot.Active = true;
                slot.Title = windowText;
                slot.Value = 0;
                assert(!slot.Thread.joinable());
                slot.Thread = std::thread(ShowProgressWindow, i);
                return i;
            }
        }
        return -1;
    }

    void ProgressBarStopImpl(int slotIndex)
    {
        assert(slotIndex >= 0 && slotIndex < g_ProgressSlotCount);
        std::thread threadToJoin;
        {
            ProgressBarGlobals& slot = g_ProgressSlots[slotIndex];
            std::lock_guard guard(slot.Mutex);
            slot.Active = false;
            slot.Title = "";
            slot.Value = 0;
            slot.hMainWindow = nullptr;
            slot.hProgressBar = nullptr;
            threadToJoin.swap(slot.Thread);
        }
        if (threadToJoin.joinable())
            threadToJoin.join();
    }

    void ProgressBarUpdateImpl(int slotIndex, int percentage)
    {
        assert(slotIndex >= 0 && slotIndex < g_ProgressSlotCount);
        ProgressBarGlobals& slot = g_ProgressSlots[slotIndex];
        std::lock_guard guard(slot.Mutex);
        slot.Value = percentage;
    }
#else
    int ProgressBarStartImpl(const char*) { return -1; }
    void ProgressBarStopImpl(int) {}
    void ProgressBarUpdateImpl(int, int) {}
#endif

} // anonymous namespace

// --- ProgressBar ---
bool ProgressBar::Start(const char* windowText)
{
    std::lock_guard lock(m_mtx);
    assert(!Active());
    m_slot = ProgressBarStartImpl(windowText);
    return Active();
}

void ProgressBar::Set(int percentage)
{
    std::lock_guard lock(m_mtx);
    if (percentage < 0) percentage = 0;
    if (percentage > 100) percentage = 100;
    if (m_slot != -1)
        ProgressBarUpdateImpl(m_slot, percentage);
}

void ProgressBar::Stop()
{
    std::lock_guard lock(m_mtx);
    if (Active())
        ProgressBarStopImpl(m_slot);
    m_slot = -1;
}

bool ProgressBar::Active() const
{
    std::lock_guard lock(m_mtx);
    return m_slot != -1;
}

// --- helpers ---
void HelpersSetNonInteractive() { g_NonInteractive = true; }
bool HelpersIsNonInteractive() { return g_NonInteractive; }

#ifdef _WIN32
void HelpersRegisterActiveWindow(void* nativeWindowHandle)
{
    HWND hwnd = nativeWindowHandle ? static_cast<HWND>(nativeWindowHandle) : GetActiveWindow();
    g_hActiveParentWindow.store(hwnd);
}

void* HelpersGetActiveWindow()
{
    return g_hActiveParentWindow.load();
}
#else
void HelpersRegisterActiveWindow() {}
void* HelpersGetActiveWindow() { return nullptr; }
#endif

} // namespace caustica
