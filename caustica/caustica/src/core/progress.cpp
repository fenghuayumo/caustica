#include <core/progress.h>
#include <core/log.h>

#include <atomic>
#include <cassert>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#include <imm.h>
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Imm32.lib")
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
#endif
        std::recursive_mutex Mutex;
        std::thread         Thread;
    };

    ProgressBarGlobals g_ProgressSlots[g_ProgressSlotCount];
    bool g_NonInteractive = false;

#ifdef _WIN32
    std::atomic<HWND> g_hActiveParentWindow(NULL);

    constexpr int kCardWidth = 420;
    constexpr int kCardHeight = 86;
    constexpr UINT WM_PROGRESS_REFRESH = WM_APP + 40;

    void ShowLastError()
    {
        DWORD err = GetLastError();
        wchar_t buf[4096];
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, err, 0, buf, sizeof(buf) / sizeof(wchar_t), NULL);
        MessageBoxW(NULL, buf, L"LastError", MB_OK | MB_ICONERROR);
    }

    std::wstring Utf8ToWide(const std::string& text)
    {
        if (text.empty())
            return {};
        const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        if (needed <= 1)
            return {};
        std::wstring wide(static_cast<size_t>(needed - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), needed);
        return wide;
    }

    void PaintProgressCard(HWND hwnd, ProgressBarGlobals& slot)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!hdc)
            return;

        RECT client{};
        GetClientRect(hwnd, &client);
        const int w = client.right - client.left;
        const int h = client.bottom - client.top;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        const COLORREF bg = RGB(28, 28, 32);
        const COLORREF border = RGB(58, 58, 66);
        const COLORREF textColor = RGB(230, 230, 235);
        const COLORREF track = RGB(48, 48, 56);
        const COLORREF fill = RGB(72, 148, 220);

        HBRUSH bgBrush = CreateSolidBrush(bg);
        FillRect(memDC, &client, bgBrush);
        DeleteObject(bgBrush);

        HPEN borderPen = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ oldPen = SelectObject(memDC, borderPen);
        HGDIOBJ oldBrush = SelectObject(memDC, GetStockObject(NULL_BRUSH));
        Rectangle(memDC, 0, 0, w, h);
        SelectObject(memDC, oldBrush);
        SelectObject(memDC, oldPen);
        DeleteObject(borderPen);

        std::string title;
        int value = 0;
        {
            std::lock_guard guard(slot.Mutex);
            title = slot.Title;
            value = slot.Value;
        }
        if (value < 0) value = 0;
        if (value > 100) value = 100;

        const std::wstring wideTitle = Utf8ToWide(title);
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, textColor);

        HFONT font = CreateFontW(
            -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(memDC, font);

        RECT textRect{ 22, 16, w - 22, 44 };
        DrawTextW(memDC, wideTitle.c_str(), -1, &textRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(memDC, oldFont);
        DeleteObject(font);

        const int barLeft = 22;
        const int barRight = w - 22;
        const int barTop = 52;
        const int barBottom = 64;
        RECT trackRect{ barLeft, barTop, barRight, barBottom };
        HBRUSH trackBrush = CreateSolidBrush(track);
        FillRect(memDC, &trackRect, trackBrush);
        DeleteObject(trackBrush);

        if (value > 0)
        {
            const int fillRight = barLeft + (barRight - barLeft) * value / 100;
            RECT fillRect{ barLeft, barTop, fillRight, barBottom };
            HBRUSH fillBrush = CreateSolidBrush(fill);
            FillRect(memDC, &fillRect, fillBrush);
            DeleteObject(fillBrush);
        }

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
    }

    LRESULT CALLBACK ProgressWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        ProgressBarGlobals* slot = reinterpret_cast<ProgressBarGlobals*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_NCCREATE:
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_PROGRESS_REFRESH:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            if (slot)
                PaintProgressCard(hwnd, *slot);
            else
            {
                PAINTSTRUCT ps;
                BeginPaint(hwnd, &ps);
                EndPaint(hwnd, &ps);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
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

    void CenterOnParentOrMonitor(int width, int height, int slotIndex, int& x, int& y)
    {
        HWND hParent = g_hActiveParentWindow.load();
        if (hParent && IsWindow(hParent))
        {
            RECT parentRect{};
            GetWindowRect(hParent, &parentRect);
            x = parentRect.left + (parentRect.right - parentRect.left - width) / 2;
            y = parentRect.top + (parentRect.bottom - parentRect.top - height) / 2;
            y += (height + 12) * slotIndex;
            return;
        }

        POINT cursor{};
        GetCursorPos(&cursor);
        HMONITOR hMonitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(hMonitor, &mi))
        {
            x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - width) / 2;
            y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - height) / 2;
            y += (height + 12) * slotIndex;
        }
        else
        {
            x = CW_USEDEFAULT;
            y = CW_USEDEFAULT;
        }
    }

    void ShowProgressWindow(int slotIndex)
    {
        ProgressBarGlobals& slot = g_ProgressSlots[slotIndex];

        // Progress windows never accept text. Disable IME hooks before creating
        // any HWND on this temporary UI thread; some third-party IMEs otherwise
        // retain freed global-memory handles when the window is destroyed.
        ImmDisableIME(GetCurrentThreadId());

        const wchar_t* wClassName = L"CausticaProgressCard";
        HINSTANCE hInstance = GetModuleHandleW(nullptr);
        static std::mutex registerClassMtx;
        static bool registerClassFlag = false;
        {
            std::lock_guard<std::mutex> lock(registerClassMtx);
            if (!registerClassFlag)
            {
                WNDCLASSW wc = {};
                wc.lpfnWndProc = ProgressWndProc;
                wc.hInstance = hInstance;
                wc.lpszClassName = wClassName;
                wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                wc.hbrBackground = nullptr;
                if (!RegisterClassW(&wc))
                {
                    const DWORD err = GetLastError();
                    if (err != ERROR_CLASS_ALREADY_EXISTS)
                        ShowLastError();
                }
                registerClassFlag = true;
            }
        }

        int x = CW_USEDEFAULT;
        int y = CW_USEDEFAULT;
        CenterOnParentOrMonitor(kCardWidth, kCardHeight, slotIndex, x, y);

        HWND hwnd = nullptr;
        {
            std::lock_guard guard(slot.Mutex);
            if (!slot.Active)
                return;

            slot.hMainWindow = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                wClassName,
                L"",
                WS_POPUP,
                x, y, kCardWidth, kCardHeight,
                nullptr, nullptr, hInstance, reinterpret_cast<LPVOID>(&slot));

            if (slot.hMainWindow == nullptr)
            {
                ShowLastError();
                assert(false);
                return;
            }
            hwnd = slot.hMainWindow;

            // Win11 rounded corners when available; ignored on older Windows.
            constexpr DWORD kDwmWindowCornerPreference = 33;
            constexpr DWORD kDwmWcpRound = 2;
            DwmSetWindowAttribute(hwnd, kDwmWindowCornerPreference, &kDwmWcpRound, sizeof(kDwmWcpRound));
        }

        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);

        while (true)
        {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                    return;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            {
                std::lock_guard guard(slot.Mutex);
                if (!slot.Active)
                    break;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            Sleep(16);
        }
        SendMessageW(hwnd, WM_CLOSE, 0, 0);
    }

    void ProgressBarSetTitleImpl(int slotIndex, const char* statusText)
    {
        assert(slotIndex >= 0 && slotIndex < g_ProgressSlotCount);
        ProgressBarGlobals& slot = g_ProgressSlots[slotIndex];
        std::lock_guard guard(slot.Mutex);
        slot.Title = statusText ? statusText : "";
        slot.Value = 0;
        if (slot.hMainWindow)
            PostMessageW(slot.hMainWindow, WM_PROGRESS_REFRESH, 0, 0);
    }

    int ProgressBarStartImpl(const char* statusText)
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
                slot.Title = statusText ? statusText : "";
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
        if (slot.hMainWindow)
            PostMessageW(slot.hMainWindow, WM_PROGRESS_REFRESH, 0, 0);
    }
#else
    void ProgressBarSetTitleImpl(int, const char*) {}
    int ProgressBarStartImpl(const char*) { return -1; }
    void ProgressBarStopImpl(int) {}
    void ProgressBarUpdateImpl(int, int) {}
#endif

} // anonymous namespace

// --- ProgressBar ---
bool ProgressBar::start(const char* statusText)
{
    std::lock_guard lock(m_mtx);
    if (m_slot != -1)
    {
        // Update status in place — avoids tearing down / recreating the card.
        ProgressBarSetTitleImpl(m_slot, statusText);
        return true;
    }
    m_slot = ProgressBarStartImpl(statusText);
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

void ProgressBar::stop()
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
void helpersSetNonInteractive() { g_NonInteractive = true; }
bool helpersIsNonInteractive() { return g_NonInteractive; }

#ifdef _WIN32
void helpersRegisterActiveWindow(void* nativeWindowHandle)
{
    HWND hwnd = nativeWindowHandle ? static_cast<HWND>(nativeWindowHandle) : GetActiveWindow();
    g_hActiveParentWindow.store(hwnd);
}

void* helpersGetActiveWindow()
{
    return g_hActiveParentWindow.load();
}
#else
void helpersRegisterActiveWindow(void*) {}
void* helpersGetActiveWindow() { return nullptr; }
#endif

} // namespace caustica
