// ==WindhawkMod==
// @id              cursor-motion-blur
// @name            Cursor Motion Blur
// @description     Adds high-speed cartoon motion blur to your mouse pointer.
// @version         1.0
// @author          TheatriChris
// @github          https://github.com/chrisc44890
// @include         explorer.exe
// @compilerOptions -lgdiplus -lgdi32 -lwinmm -lshell32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Cursor Motion Blur
Replaces your standard Windows cursor with a smooth, tapered motion blur trail when moving at high speeds.

### Features
* **Zero Input Lag:** Draws directly off the hardware cursor coordinates.
* **Game Detection:** Automatically disables itself when playing fullscreen or borderless DirectX games.
* **Taskbar Friendly:** Retains native edge-detection for auto-hiding taskbars.
* **Dynamic Rendering:** Idles at 0% CPU usage when the mouse is stationary.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- trigger_velocity: 25
  $name: Trigger Velocity
  $description: How fast the mouse needs to trigger the blur (pixels per frame).
- stop_velocity: 10
  $name: Stop Velocity
  $description: Velocity threshold to stop the blur. Must be lower than Trigger Velocity.
- tail_offset_x: 6
  $name: Tail Offset X
  $description: X-axis offset for where the tail connects to the cursor.
- tail_offset_y: 10
  $name: Tail Offset Y
  $description: Y-axis offset for where the tail connects to the cursor.
- tail_length: 10
  $name: Tail Length
  $description: How many frames the blur trails behind you.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <gdiplus.h>
#include <math.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <deque>

using namespace Gdiplus;

// Global state variables
HWND g_overlayHwnd = NULL;
HANDLE g_threadHandle = NULL;
ULONG_PTR g_gdiplusToken;
std::deque<Point> g_history;
POINT g_lastPos = { 0, 0 }; 

Bitmap* g_cursorBitmap = nullptr;
int g_cursorHotspotX = 0;
int g_cursorHotspotY = 0;

// Settings cache
float g_triggerVelocity = 25.0f;
float g_stopVelocity = 10.0f;
int g_tailOffsetX = 6;
int g_tailOffsetY = 10;
int g_tailLength = 10;

void LoadSettings() {
    g_triggerVelocity = (float)Wh_GetIntSetting(L"trigger_velocity");
    g_stopVelocity = (float)Wh_GetIntSetting(L"stop_velocity");
    g_tailOffsetX = Wh_GetIntSetting(L"tail_offset_x");
    g_tailOffsetY = Wh_GetIntSetting(L"tail_offset_y");
    g_tailLength = Wh_GetIntSetting(L"tail_length");

    if (g_triggerVelocity <= 0.0f) g_triggerVelocity = 25.0f;
    if (g_stopVelocity <= 0.0f) g_stopVelocity = 10.0f;
    if (g_tailLength < 2) g_tailLength = 10; 
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

bool IsGameRunning() {
    HWND hwnd = GetForegroundWindow();
    
    if (!hwnd || hwnd == GetDesktopWindow() || hwnd == FindWindowW(L"Progman", NULL) || hwnd == FindWindowW(L"WorkerW", NULL)) {
        return false;
    }

    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN) {
            return true;
        }
    }

    RECT rcApp;
    GetWindowRect(hwnd, &rcApp);
    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hMonitor, &mi)) {
        bool isFullscreen = (rcApp.left <= mi.rcMonitor.left &&
                             rcApp.top <= mi.rcMonitor.top &&
                             rcApp.right >= mi.rcMonitor.right &&
                             rcApp.bottom >= mi.rcMonitor.bottom);
        if (isFullscreen) {
            RECT rcClip;
            if (GetClipCursor(&rcClip)) {
                int vW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                int vH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                if ((rcClip.right - rcClip.left) < vW || (rcClip.bottom - rcClip.top) < vH) {
                    return true;
                }
            }
            
            CURSORINFO ci = { sizeof(CURSORINFO) };
            if (GetCursorInfo(&ci) && ci.flags == 0) {
                return true;
            }
        }
    }
    return false;
}

void CacheRealCursor() {
    CURSORINFO ci = { sizeof(CURSORINFO) };
    if (GetCursorInfo(&ci) && ci.flags == CURSOR_SHOWING) {
        static HCURSOR s_lastSeen = NULL;
        if (ci.hCursor != s_lastSeen) {
            if (g_cursorBitmap) { 
                delete g_cursorBitmap; 
                g_cursorBitmap = nullptr;
            }
            
            ICONINFO ii;
            if (GetIconInfo(ci.hCursor, &ii)) {
                g_cursorHotspotX = ii.xHotspot;
                g_cursorHotspotY = ii.yHotspot;
                g_cursorBitmap = Bitmap::FromHICON(ci.hCursor);
                
                DeleteObject(ii.hbmColor);
                DeleteObject(ii.hbmMask);
            }
            s_lastSeen = ci.hCursor;
        }
    }
}

void ToggleCursor(bool show) {
    static bool isHidden = false;
    if (show && isHidden) {
        SystemParametersInfo(SPI_SETCURSORS, 0, NULL, 0);
        isHidden = false;
    } else if (!show && !isHidden) {
        BYTE andMask[128];
        BYTE xorMask[128];
        memset(andMask, 0xFF, sizeof(andMask)); 
        memset(xorMask, 0x00, sizeof(xorMask)); 
        
        HCURSOR hBlank1 = CreateCursor(NULL, 0, 0, 32, 32, andMask, xorMask);
        HCURSOR hBlank2 = CreateCursor(NULL, 0, 0, 32, 32, andMask, xorMask);
        HCURSOR hBlank3 = CreateCursor(NULL, 0, 0, 32, 32, andMask, xorMask);
        
        SetSystemCursor(hBlank1, 32512); 
        SetSystemCursor(hBlank2, 32649); 
        SetSystemCursor(hBlank3, 32513); 
        
        isHidden = true;
    }
}

VOID CALLBACK SmearTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    POINT pt;
    GetCursorPos(&pt);

    int dx = pt.x - g_lastPos.x;
    int dy = pt.y - g_lastPos.y;
    float velocity = sqrt((float)(dx * dx + dy * dy));
    g_lastPos = pt; 

    // Move these to the top so we can manipulate them from inside the game check
    static DWORD lastFullscreenCheck = 0;
    static bool isGameCached = false;
    static bool isSmearing = false;
    static int lowVelocityFrames = 0;
    static bool needsClear = false;

    if (dwTime - lastFullscreenCheck > 500) {
        isGameCached = IsGameRunning();
        lastFullscreenCheck = dwTime;
    }

    int vX = GetSystemMetrics(SM_XVIRTUALSCREEN) + 1;
    int vY = GetSystemMetrics(SM_YVIRTUALSCREEN) + 1;
    int vW = GetSystemMetrics(SM_CXVIRTUALSCREEN) - 2;
    int vH = GetSystemMetrics(SM_CYVIRTUALSCREEN) - 2;

    if (isGameCached) {
        // Controlled demolition: If a game launched while we were moving, kill it safely
        if (isSmearing || !g_history.empty() || needsClear) {
            isSmearing = false;
            g_history.clear();
            ToggleCursor(true);
            // We DO NOT return here! Let it bleed into the drawing block so it pushes a blank frame to the GPU and wipes the screen
        } else {
            return; // Everything is clean, go to sleep
        }
    } else {
        CacheRealCursor();

        if (velocity > g_triggerVelocity && !isSmearing) {
            isSmearing = true;
            lowVelocityFrames = 0;
            ToggleCursor(false);
        } 
        else if (velocity < g_stopVelocity && isSmearing) {
            lowVelocityFrames++;
            if (lowVelocityFrames > 2) { 
                isSmearing = false;
                ToggleCursor(true);
            }
        } 
        else if (velocity >= g_stopVelocity && isSmearing) {
            lowVelocityFrames = 0;
        }

        // --- BUFFER MANAGEMENT ---
        if (isSmearing) {
            // Fast move: Keep recording path history
            g_history.push_front(Point(pt.x - vX, pt.y - vY));
            while (g_history.size() > (size_t)g_tailLength) { 
                g_history.pop_back();
            }
        } else {
            // Slow/Stopped: The river is running out of water rapidly.
            if (!g_history.empty()) {
                g_history.pop_back();
                if (!g_history.empty()) {
                    g_history.pop_back();
                }
            }
        }
    }

    bool isDrawing = isSmearing || !g_history.empty();
    
    if (isDrawing || needsClear) {
        HDC hdcScreen = GetDC(NULL);
        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, vW, vH);
        HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

        Graphics graphics(hdcMem);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);

        if (isDrawing) {
            if (g_history.size() >= 2) {
                // PASS 1: Draw the black outline underneath everything
                for (size_t i = 0; i < g_history.size() - 1; ++i) {
                    int opacity = 220 - (i * (220 / (g_history.size() - 1))); 
                    float width = 20.0f - (i * (12.0f / (g_history.size() - 1)));

                    Pen blackPen(Color(opacity, 0, 0, 0), width);
                    blackPen.SetStartCap(LineCapRound);
                    blackPen.SetEndCap(LineCapRound);

                    Point p1(g_history[i].X + g_tailOffsetX, g_history[i].Y + g_tailOffsetY);
                    Point p2(g_history[i+1].X + g_tailOffsetX, g_history[i+1].Y + g_tailOffsetY);

                    graphics.DrawLine(&blackPen, p1, p2);
                }

                // PASS 2: Draw the white core on top
                for (size_t i = 0; i < g_history.size() - 1; ++i) {
                    int opacity = 220 - (i * (220 / (g_history.size() - 1))); 
                    float width = 20.0f - (i * (12.0f / (g_history.size() - 1)));

                    Pen whitePen(Color(opacity, 255, 255, 255), width * 0.6f);
                    whitePen.SetStartCap(LineCapRound);
                    whitePen.SetEndCap(LineCapRound);

                    Point p1(g_history[i].X + g_tailOffsetX, g_history[i].Y + g_tailOffsetY);
                    Point p2(g_history[i+1].X + g_tailOffsetX, g_history[i+1].Y + g_tailOffsetY);

                    graphics.DrawLine(&whitePen, p1, p2);
                }
            }

            // Draw the fake cursor ONLY while we are actively smearing
            if (isSmearing && g_cursorBitmap) {
                graphics.DrawImage(g_cursorBitmap, 
                    g_history.front().X - g_cursorHotspotX, 
                    g_history.front().Y - g_cursorHotspotY);
            }
            needsClear = true; 
        } else {
            needsClear = false; 
        }

        BLENDFUNCTION blend = { 0 };
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255; 
        blend.AlphaFormat = AC_SRC_ALPHA;

        POINT ptPos = { vX, vY };
        SIZE sizeWnd = { vW, vH };
        POINT ptSrc = { 0, 0 };

        UpdateLayeredWindow(hwnd, hdcScreen, &ptPos, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

        SelectObject(hdcMem, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
    }
}

DWORD WINAPI OverlayThreadProc(LPVOID lpParam) {
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    HINSTANCE hInstance = GetModuleHandle(NULL);
    const wchar_t CLASS_NAME[] = L"SmearFrameOverlayClass";

    WNDCLASS wc = { };
    wc.lpfnWndProc = DefWindowProc; 
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;

    RegisterClass(&wc);

    int screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g_overlayHwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CLASS_NAME,
        L"SmearOverlay",
        WS_POPUP,
        screenX, screenY, screenW, screenH,
        NULL, NULL, hInstance, NULL
    );

    if (!g_overlayHwnd) return 0;

    ShowWindow(g_overlayHwnd, SW_SHOWNA);
    
    GetCursorPos(&g_lastPos);

    timeBeginPeriod(1);
    SetTimer(g_overlayHwnd, 1, 1, SmearTimerProc);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    timeEndPeriod(1);
    GdiplusShutdown(g_gdiplusToken);
    return 0;
}

BOOL Wh_ModInit() {
    LoadSettings();
    g_threadHandle = CreateThread(NULL, 0, OverlayThreadProc, NULL, 0, NULL);
    return TRUE;
}

void Wh_ModUninit() {
    ToggleCursor(true);
    
    if (g_cursorBitmap) { 
        delete g_cursorBitmap; 
        g_cursorBitmap = nullptr; 
    }

    if (g_overlayHwnd) {
        PostMessage(g_overlayHwnd, WM_QUIT, 0, 0);
    }
    if (g_threadHandle) {
        WaitForSingleObject(g_threadHandle, INFINITE);
        CloseHandle(g_threadHandle);
    }
}
