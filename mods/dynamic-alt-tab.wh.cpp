// ==WindhawkMod==
// @id            dynamic-alt-tab
// @name          Dynamic Alt-Tab
// @description   Replaces the native Windows Alt-Tab with a fluid, hardware-accelerated live glass carousel and custom themes.
// @version       1.0
// @author        TheatriChris
// @github        https://github.com/chrisc44890
// @include       *
// @compilerOptions -ld2d1 -ldwmapi -lole32 -lgdi32 -lshell32 -ldwrite -lversion -luuid
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Dynamic Alt-Tab
Replaces the native Windows Alt-Tab screen with a fluid, hardware-accelerated live glass carousel and custom themes.

**Note:** This mod relies on internal Windows 11 APIs (`twinui.pcshell.dll`) and is designed specifically for Windows 11 and tested on 25H2. It may not hook successfully on Windows 10 or unsupported Insider builds.

### Features
* **Hardware Accelerated:** Built with Direct2D 1.1 for buttery smooth 60FPS rendering.
* **Live Thumbnails:** Uses DWM thumbnail routing to show real-time, live previews of your apps.
* **Custom Themes:** 
  * *3D Glass Carousel:* A gorgeous spatial rotating cylinder with physical depth shadows.
  ![3D Carousel](https://i.imgur.com/n8FrdUV.gif)
  * *macOS Style Flat Grid:* An Exposé-style grid with clean, silver-aluminum highlights.
  ![MacOS](https://i.imgur.com/no7EoWE.gif)
  * *Apple Liquid Glass:* Waterdrop theme inspired by Apple's Liquid Glass design language
  ![Liquid Glass](https://i.imgur.com/pfgvH3z.gif)
  * *Material 3 Expressive:* Playful pastel pills that dynamically stretch and squish.
  ![Material 3](https://i.imgur.com/eycgEo9.gif)
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- theme: "Apple Liquid Glass"
  $name: Visual Theme
  $description: Choose your aesthetic.
  $options:
    - "3D Glass Carousel": 3D Glass Carousel
    - "macOS Style Flat Grid": macOS Style Flat Grid
    - "Apple Liquid Glass": Apple Liquid Glass
    - "Material 3 Expressive": Material 3 Expressive
- bg_opacity: 60
  $name: Background Dim Opacity
  $description: How dark the background will be. (0 = clear, 100 = pitch black).
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <math.h>
#include <vector>
#include <string>
#include <atomic>
#include <algorithm>
#include <windhawk_utils.h>

typedef UINT (WINAPI* timeBeginPeriod_t)(UINT);
typedef UINT (WINAPI* timeEndPeriod_t)(UINT);

struct AccentPolicy {
    int state;
    int flags;
    DWORD gradientColor;
    int animationId;
};

struct CompositionAttributeData {
    int attribute;
    void* data;
    SIZE_T size;
};

typedef BOOL(WINAPI* SetWindowCompositionAttribute_t)(HWND, CompositionAttributeData*);

HWND g_overlayHwnd = NULL;
HANDLE g_renderThreadHandle = NULL;
HANDLE g_hookThreadHandle = NULL;
DWORD g_hookThreadId = 0;
HHOOK g_keyboardHook = NULL;
std::atomic<bool> g_threadRunning(false);

ID2D1Factory1* g_pD2DFactory = nullptr;
ID2D1DCRenderTarget* g_pDCRenderTarget = nullptr;
ID2D1DeviceContext* g_pDeviceContext = nullptr; 

IDWriteFactory* g_pDWriteFactory = nullptr;
IDWriteTextFormat* g_pTextFormat = nullptr;

ID2D1SolidColorBrush* g_pGlassBrush = nullptr;
ID2D1SolidColorBrush* g_pBorderBrush = nullptr;
ID2D1SolidColorBrush* g_pCloseBrush = nullptr;
ID2D1SolidColorBrush* g_pTextBrush = nullptr;
ID2D1SolidColorBrush* g_pDimBrush = nullptr;
ID2D1SolidColorBrush* g_pWhiteBrush = nullptr;
ID2D1LinearGradientBrush* g_pSpecularBrush = nullptr;

HDC g_hdcMem = NULL;
HBITMAP g_hBitmap = NULL;
int g_vW = 0, g_vH = 0;

bool g_isAltTabbing = false;
bool g_isClosing = false;
bool g_isFirstFrame = false;
int g_selectedIndex = 0;
POINT g_mousePos = { 0, 0 };

float g_introOutroProgress = 0.0f;
float g_introOutroTarget = 0.0f;

std::atomic<int> g_theme(2);
std::atomic<int> g_bgOpacity(60);

std::atomic<bool> g_wantsToOpen(false);
std::atomic<bool> g_cancelAltTab(false);
std::atomic<int> g_tabSteps(0);
std::atomic<DWORD> g_threadIdForAltTabShowWindow(0);

float g_leftPoolX1 = 0.0f;
float g_leftPoolX2 = 0.0f;
float g_leftPoolY = 0.0f;
float g_leftPoolScale = 0.0f;
float g_leftPoolVx1 = 0.0f;
float g_leftPoolVx2 = 0.0f;
float g_leftPoolVy = 0.0f;
float g_leftPoolVScale = 0.0f;

float g_rightPoolX1 = 0.0f;
float g_rightPoolX2 = 0.0f;
float g_rightPoolY = 0.0f;
float g_rightPoolScale = 0.0f;
float g_rightPoolVx1 = 0.0f;
float g_rightPoolVx2 = 0.0f;
float g_rightPoolVy = 0.0f;
float g_rightPoolVScale = 0.0f;

struct AppCard {
    HWND hwnd;
    HTHUMBNAIL thumbnail;
    std::wstring title;
    float currentX, targetX, vx;
    float currentY, targetY, vy;
    float currentScale, targetScale, vScale;
    float currentCornerRadius, targetCornerRadius, vCornerRadius;
    float currentWidthRadius, targetWidthRadius, vWidthRadius;
};

std::vector<AppCard> g_cards;

void LoadSettings() {
    PCWSTR themeStrRaw = Wh_GetStringSetting(L"theme");
    std::wstring themeStr = themeStrRaw ? themeStrRaw : L"";
    if (themeStrRaw) Wh_FreeStringSetting(themeStrRaw);

    if (themeStr == L"3D Glass Carousel") g_theme.store(0);
    else if (themeStr == L"macOS Style Flat Grid") g_theme.store(1);
    else if (themeStr == L"Apple Liquid Glass") g_theme.store(2);
    else if (themeStr == L"Material 3 Expressive") g_theme.store(3);
    else g_theme.store(2); // Default
    
    int opacity = Wh_GetIntSetting(L"bg_opacity");
    if (opacity < 0) opacity = 0;
    if (opacity > 100) opacity = 100;
    g_bgOpacity.store(opacity);
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

std::wstring GetWindowTextSafe(HWND hwnd) {
    wchar_t buf[256] = {0};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    
    if (pid == GetCurrentProcessId()) {
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(hwnd, WM_GETTEXT, 256, (LPARAM)buf, SMTO_ABORTIFHUNG | SMTO_NORMAL, 10, &result) && result > 0) {
            return std::wstring(buf);
        }
        wchar_t className[256] = {0};
        GetClassNameW(hwnd, className, 256);
        if (wcscmp(className, L"CabinetWClass") == 0) return L"File Explorer";
        return L""; 
    } else {
        GetWindowTextW(hwnd, buf, 256);
        return std::wstring(buf);
    }
}

bool IsValidAppWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || hwnd == GetShellWindow()) return false;
    
    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return false;

    int cloakedVal = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloakedVal, sizeof(cloakedVal))) && cloakedVal != 0) {
        return false;
    }

    RECT rc;
    if (GetWindowRect(hwnd, &rc)) {
        if ((rc.right - rc.left) <= 10 || (rc.bottom - rc.top) <= 10) return false;
    }

    wchar_t className[256]; 
    GetClassNameW(hwnd, className, 256);
    std::wstring cls(className);
    
    if (cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd" || 
        cls == L"Shell_SecondaryTrayWnd" || cls == L"Windows.UI.Core.CoreWindow" ||
        cls == L"GlassAltTabOverlayClass" || cls == L"XamlExplorerHostIslandWindow" ||
        cls == L"SmearFrameOverlayClass") { 
        return false;
    }

    std::wstring title = GetWindowTextSafe(hwnd);
    if (title.empty()) return false;

    return true;
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (IsValidAppWindow(hwnd)) {
        AppCard card = {0};
        card.hwnd = hwnd;
        card.title = GetWindowTextSafe(hwnd);
        card.vx = 0.0f;
        card.vy = 0.0f;
        card.vScale = 0.0f;
        card.vCornerRadius = 0.0f;
        card.vWidthRadius = 0.0f;
        g_cards.push_back(card);
    }
    return TRUE;
}

D2D1_COLOR_F GetM3Color(int index, float alpha) {
    int val = index % 5;
    if (val == 0) return D2D1::ColorF(0.48f, 0.82f, 0.68f, alpha); // Soft Mint
    if (val == 1) return D2D1::ColorF(0.72f, 0.62f, 0.88f, alpha); // Soft Lavender
    if (val == 2) return D2D1::ColorF(0.92f, 0.58f, 0.58f, alpha); // Soft Coral
    if (val == 3) return D2D1::ColorF(0.92f, 0.82f, 0.48f, alpha); // Soft Lemon
    return D2D1::ColorF(0.48f, 0.72f, 0.92f, alpha); // Soft Sky Blue
}

void StartAltTab() {
    g_isAltTabbing = true;
    g_isClosing = false;
    g_isFirstFrame = true; 
    g_introOutroProgress = 0.0f;
    g_introOutroTarget = 1.0f;
    g_cancelAltTab = false;
    
    for (auto& card : g_cards) {
        if (card.thumbnail) DwmUnregisterThumbnail(card.thumbnail);
    }
    g_cards.clear();
    EnumWindows(EnumWindowsProc, 0);
    
    if (g_cards.empty()) { 
        g_isAltTabbing = false; 
        return; 
    }
    
    g_selectedIndex = 0; 
    
    for (auto& card : g_cards) {
        DwmRegisterThumbnail(g_overlayHwnd, card.hwnd, &card.thumbnail);
        card.currentX = 0.0f;
        card.currentY = 0.0f;
        card.currentScale = 0.1f;
        card.currentCornerRadius = 190.0f;
        card.currentWidthRadius = 300.0f;
    }

    int vW = GetSystemMetrics(SM_CXVIRTUALSCREEN), vH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    g_leftPoolX1 = vW / 2.0f;
    g_leftPoolX2 = vW / 2.0f;
    g_leftPoolY = vH / 2.0f + 140.0f;
    g_leftPoolScale = 0.0f;
    g_leftPoolVx1 = 0.0f; g_leftPoolVx2 = 0.0f; g_leftPoolVy = 0.0f; g_leftPoolVScale = 0.0f;

    g_rightPoolX1 = vW / 2.0f;
    g_rightPoolX2 = vW / 2.0f;
    g_rightPoolY = vH / 2.0f + 140.0f;
    g_rightPoolScale = 0.0f;
    g_rightPoolVx1 = 0.0f; g_rightPoolVx2 = 0.0f; g_rightPoolVy = 0.0f; g_rightPoolVScale = 0.0f;
    
    ShowWindow(g_overlayHwnd, SW_SHOWNA);
}

void StopAltTab() {
    if (g_isClosing) return;
    g_isClosing = true;
    g_introOutroTarget = 0.0f; 
    
    if (g_cancelAltTab) {
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        return;
    }

    if (!g_cards.empty() && g_selectedIndex >= 0 && g_selectedIndex < (int)g_cards.size()) {
        HWND target = g_cards[g_selectedIndex].hwnd;
        
        if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
        
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        
        SetForegroundWindow(g_overlayHwnd);
        
        DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
        DWORD targetThread = GetWindowThreadProcessId(target, NULL);
        
        if (fgThread != targetThread) {
            AttachThreadInput(fgThread, targetThread, TRUE);
            SetWindowPos(target, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetWindowPos(target, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(target);
            SetFocus(target);
            AttachThreadInput(fgThread, targetThread, FALSE);
        } else {
            SetForegroundWindow(target);
            SetFocus(target);
        }
        
        SwitchToThisWindow(target, TRUE); 
    }
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    static bool s_tabDown = false;
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        bool altDown = ((p->flags & LLKHF_ALTDOWN) != 0) || (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        
        if (p->vkCode == VK_TAB && altDown) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                if (!s_tabDown) {
                    s_tabDown = true;
                    g_wantsToOpen = true;
                    bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    g_tabSteps.fetch_add(shiftDown ? -1 : 1);
                }
            } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                s_tabDown = false;
            }
            return 1; 
        }
        
        if ((p->vkCode == VK_LMENU || p->vkCode == VK_RMENU || p->vkCode == VK_MENU) && (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)) {
            g_wantsToOpen = false;
            s_tabDown = false;
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

DWORD WINAPI HookThreadProc(LPVOID lpParam) {
    HANDLE hEvent = (LPVOID)lpParam;
    g_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    if (hEvent) SetEvent(hEvent);
    
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        DispatchMessage(&msg);
    }
    
    if (g_keyboardHook) UnhookWindowsHookEx(g_keyboardHook);
    return 0;
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_MOUSEMOVE) {
        g_mousePos.x = (int)(short)LOWORD(lParam);
        g_mousePos.y = (int)(short)HIWORD(lParam);
    }
    
    if (uMsg == WM_MOUSEWHEEL && g_isAltTabbing && !g_isClosing) {
        int zDelta = (short)HIWORD(wParam);
        int count = (int)g_cards.size();
        if (count > 0) {
            if (zDelta > 0) {
                g_selectedIndex = (g_selectedIndex - 1 + count) % count;
            } else {
                g_selectedIndex = (g_selectedIndex + 1) % count;
            }
        }
        return 0;
    }
    
    if (uMsg == WM_LBUTTONUP && g_isAltTabbing && !g_isClosing) {
        int x = (int)(short)LOWORD(lParam);
        int y = (int)(short)HIWORD(lParam);
        
        bool clickedCard = false;
        
        std::vector<int> clickOrder(g_cards.size());
        for (int i = 0; i < (int)g_cards.size(); ++i) clickOrder[i] = i;
        std::sort(clickOrder.begin(), clickOrder.end(), [](int a, int b) {
            return fabsf((float)a - (float)g_selectedIndex) < fabsf((float)b - (float)g_selectedIndex);
        });

        for (int i : clickOrder) {
            float cardW = g_cards[i].currentWidthRadius * g_cards[i].currentScale;
            float cardH = 380.0f * g_cards[i].currentScale;
            float left = g_cards[i].currentX - cardW;
            float right = g_cards[i].currentX + cardW;
            float top = g_cards[i].currentY - cardH / 2.0f;
            float bottom = g_cards[i].currentY + cardH / 2.0f;
            
            float cx = right - 25.0f * g_cards[i].currentScale;
            float cy = top + 25.0f * g_cards[i].currentScale;
            float distSq = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            
            if (distSq < (144.0f * g_cards[i].currentScale * g_cards[i].currentScale)) {
                PostMessage(g_cards[i].hwnd, WM_CLOSE, 0, 0);
                if (g_cards[i].thumbnail) DwmUnregisterThumbnail(g_cards[i].thumbnail);
                g_cards.erase(g_cards.begin() + i);
                if (g_selectedIndex >= (int)g_cards.size()) g_selectedIndex = (int)g_cards.size() - 1;
                if (g_cards.empty()) {
                    g_cancelAltTab = true;
                    g_wantsToOpen = false;
                }
                return 0;
            }
            
            if (x >= left && x <= right && y >= top && y <= bottom) {
                clickedCard = true;
                g_selectedIndex = i;
                g_wantsToOpen = false; 
                return 0;
            }
        }
        
        if (!clickedCard) {
            g_cancelAltTab = true;
            g_wantsToOpen = false;
            return 0;
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

VOID CALLBACK RenderTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    bool wantsToOpen = g_wantsToOpen.load();
    if (wantsToOpen && !g_isAltTabbing) {
        StartAltTab();
    } else if (!wantsToOpen && g_isAltTabbing && !g_isClosing) {
        StopAltTab();
    }
    
    int steps = g_tabSteps.exchange(0);
    if (steps != 0 && g_isAltTabbing && !g_cards.empty()) {
        g_selectedIndex = (g_selectedIndex + steps) % (int)g_cards.size();
        if (g_selectedIndex < 0) g_selectedIndex += (int)g_cards.size();
    }
    
    if (!g_isAltTabbing && !g_isClosing) return;
    
    g_introOutroProgress += (g_introOutroTarget - g_introOutroProgress) * 0.22f;
    
    int vW = GetSystemMetrics(SM_CXVIRTUALSCREEN), vH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int vX = GetSystemMetrics(SM_XVIRTUALSCREEN), vY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    HDC hdcScreen = GetDC(NULL);
    
    if (g_isClosing && g_introOutroProgress < 0.02f) {
        g_isAltTabbing = false;
        g_isClosing = false;
        g_introOutroProgress = 0.0f;
        
        for (auto& card : g_cards) {
            if (card.thumbnail) DwmUnregisterThumbnail(card.thumbnail);
        }
        g_cards.clear();
        ShowWindow(hwnd, SW_HIDE);
        ReleaseDC(NULL, hdcScreen);
        return;
    }
    
    if (!g_hBitmap || g_vW != vW || g_vH != vH) {
        if (g_hBitmap) DeleteObject(g_hBitmap);
        if (g_hdcMem) DeleteDC(g_hdcMem);
        g_hdcMem = CreateCompatibleDC(hdcScreen);
        g_hBitmap = CreateCompatibleBitmap(hdcScreen, vW, vH);
        SelectObject(g_hdcMem, g_hBitmap);
        g_vW = vW; g_vH = vH;
        
        if (g_pDCRenderTarget) {
            g_pDCRenderTarget->Release();
            g_pDCRenderTarget = nullptr;
            if (g_pDeviceContext) { g_pDeviceContext->Release(); g_pDeviceContext = nullptr; }
        }
    }
    
    if (!g_pDCRenderTarget && g_pD2DFactory) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0, 0, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT
        );
        g_pD2DFactory->CreateDCRenderTarget(&props, &g_pDCRenderTarget);

        if (g_pDCRenderTarget) {
            g_pDCRenderTarget->QueryInterface(__uuidof(ID2D1DeviceContext), (void**)&g_pDeviceContext);
        }
        
        g_pDCRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &g_pGlassBrush);
        g_pDCRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &g_pBorderBrush);
        g_pDCRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f), &g_pCloseBrush);
        g_pDCRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &g_pTextBrush);
        g_pDCRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), &g_pDimBrush);
        g_pDCRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &g_pWhiteBrush);

        ID2D1GradientStopCollection *pGradientStops = NULL;
        D2D1_GRADIENT_STOP gradientStops[3];
        gradientStops[0].color = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f);
        gradientStops[0].position = 0.0f;
        gradientStops[1].color = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f); 
        gradientStops[1].position = 0.45f;
        gradientStops[2].color = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.50f); 
        gradientStops[2].position = 1.0f;
        
        g_pDCRenderTarget->CreateGradientStopCollection(
            gradientStops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &pGradientStops
        );
        if (pGradientStops) {
            g_pDCRenderTarget->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(100, 100)),
                pGradientStops, &g_pSpecularBrush
            );
            pGradientStops->Release();
        }
    }
    
    if (g_pDCRenderTarget) {
        RECT rc = { 0, 0, vW, vH };
        g_pDCRenderTarget->BindDC(g_hdcMem, &rc);
        g_pDCRenderTarget->BeginDraw();
        
        g_pDCRenderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.01f));
        
        if (g_pDimBrush) {
            float dimAmount = (float)g_bgOpacity.load() / 100.0f;
            g_pDimBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, dimAmount * g_introOutroProgress));
            g_pDCRenderTarget->FillRectangle(D2D1::RectF(0, 0, (float)vW, (float)vH), g_pDimBrush);
        }

        for (int i = 0; i < (int)g_cards.size(); ++i) {
            AppCard& card = g_cards[i];
            float offset = (float)i - (float)g_selectedIndex;
            float distance = fabsf(offset);
            (void)distance;

            if (g_theme == 1) { 
                // macOS Flat Grid Style
                int cols = (int)g_cards.size();
                if (cols > 5) cols = 5;
                if (cols < 1) cols = 1;
                int rows = ((int)g_cards.size() + cols - 1) / cols;
                int col = i % cols;
                int row = i / cols;
                card.targetX = (vW / 2.0f) - ((cols - 1) * 360.0f) / 2.0f + (col * 360.0f);
                card.targetY = (vH / 2.0f) - ((rows - 1) * 260.0f) / 2.0f + (row * 260.0f);
                card.targetScale = (i == g_selectedIndex) ? 0.72f : 0.48f;
                card.targetCornerRadius = 14.0f;
                card.targetWidthRadius = 310.0f;
            } else if (g_theme == 2) {
                // Apple Liquid Glass Dynamic Layout
                float dir = (offset > 0.0f) ? 1.0f : (offset < 0.0f ? -1.0f : 0.0f);
                if (i == g_selectedIndex) {
                    card.targetX = vW / 2.0f;
                    card.targetY = vH / 2.0f - 110.0f; 
                    card.targetScale = 0.85f;
                    card.targetCornerRadius = 24.0f; 
                    card.targetWidthRadius = 300.0f;
                } else {
                    card.targetX = (vW / 2.0f) + (dir * 330.0f) + (offset * 110.0f); 
                    card.targetY = vH / 2.0f + 140.0f; 
                    card.targetScale = 0.35f; 
                    card.targetCornerRadius = 16.0f; 
                    card.targetWidthRadius = 140.0f; 
                }
            } else if (g_theme == 3) {
                // Material 3 Expressive
                card.targetX = (vW / 2.0f) + (offset * 330.0f);
                card.targetY = vH / 2.0f - 20.0f;
                card.targetScale = (i == g_selectedIndex) ? 0.78f : 0.42f;
                card.targetCornerRadius = (i == g_selectedIndex) ? 28.0f : 190.0f;
                card.targetWidthRadius = (i == g_selectedIndex) ? 300.0f : 190.0f; 
            } else { 
                // 3D Glass Carousel
                float angle = std::clamp(offset * 0.35f, -1.4f, 1.4f);
                float depth = cosf(angle);
                card.targetX = (vW / 2.0f) + (sinf(angle) * 580.0f);
                card.targetY = (vH / 2.0f) + (depth * 80.0f) - 60.0f; 
                card.targetScale = (i == g_selectedIndex ? 0.95f : 0.45f) * (0.6f + 0.4f * depth);
                card.targetCornerRadius = 16.0f;
                card.targetWidthRadius = 300.0f * (0.5f + 0.5f * depth); 
            }

            card.targetY += (1.0f - g_introOutroProgress) * 400.0f;
            card.targetScale *= (0.5f + 0.5f * g_introOutroProgress);

            if (g_isFirstFrame) {
                card.currentX = card.targetX;
                card.currentY = card.targetY + 200.0f; 
                card.currentScale = card.targetScale * 0.5f;
                card.currentCornerRadius = card.targetCornerRadius;
                card.currentWidthRadius = card.targetWidthRadius;
            }

            if (g_theme == 3) {
                float stiffness = 0.16f;
                float damping = 0.45f;
                card.vx = (card.vx + (card.targetX - card.currentX) * stiffness) * damping;
                card.vy = (card.vy + (card.targetY - card.currentY) * stiffness) * damping;
                card.vScale = (card.vScale + (card.targetScale - card.currentScale) * stiffness) * damping;
                card.vCornerRadius = (card.vCornerRadius + (card.targetCornerRadius - card.currentCornerRadius) * stiffness) * damping;
                card.vWidthRadius = (card.vWidthRadius + (card.targetWidthRadius - card.currentWidthRadius) * stiffness) * damping;
            } else if (g_theme == 2) {
                float stiffness = (i == g_selectedIndex) ? 0.32f : 0.24f;
                float damping = (i == g_selectedIndex) ? 0.62f : 0.65f;
                
                card.vx = (card.vx + (card.targetX - card.currentX) * stiffness) * damping;
                card.vy = (card.vy + (card.targetY - card.currentY) * stiffness) * damping;
                card.vScale = (card.vScale + (card.targetScale - card.currentScale) * stiffness) * damping;
                card.vCornerRadius = (card.vCornerRadius + (card.targetCornerRadius - card.currentCornerRadius) * stiffness) * damping;
                card.vWidthRadius = (card.vWidthRadius + (card.targetWidthRadius - card.currentWidthRadius) * stiffness) * damping;
            } else if (g_theme == 1) {
                float stiffness = 0.35f;
                float damping = 0.72f; 
                card.vx = (card.vx + (card.targetX - card.currentX) * stiffness) * damping;
                card.vy = (card.vy + (card.targetY - card.currentY) * stiffness) * damping;
                card.vScale = (card.vScale + (card.targetScale - card.currentScale) * stiffness) * damping;
                card.vCornerRadius = (card.vCornerRadius + (card.targetCornerRadius - card.currentCornerRadius) * stiffness) * damping;
                card.vWidthRadius = (card.vWidthRadius + (card.targetWidthRadius - card.currentWidthRadius) * stiffness) * damping;
            } else {
                card.vx = (card.vx + (card.targetX - card.currentX) * 0.14f) * 0.78f;
                card.vy = (card.vy + (card.targetY - card.currentY) * 0.14f) * 0.78f;
                card.vScale = (card.vScale + (card.targetScale - card.currentScale) * 0.14f) * 0.78f;
                card.vCornerRadius = (card.vCornerRadius + (card.targetCornerRadius - card.currentCornerRadius) * 0.14f) * 0.78f;
                card.vWidthRadius = (card.vWidthRadius + (card.targetWidthRadius - card.currentWidthRadius) * 0.14f) * 0.78f;
            }

            card.currentX += card.vx;
            card.currentY += card.vy;
            card.currentScale += card.vScale;
            card.currentCornerRadius += card.vCornerRadius;
            card.currentWidthRadius += card.vWidthRadius;
        }

        g_isFirstFrame = false;

        if (g_theme == 2) {
            float targetX1_L = 99999.0f;
            float targetX2_L = -99999.0f;
            float targetY_L = 0.0f;
            float count_L = 0.0f;

            float targetX1_R = 99999.0f;
            float targetX2_R = -99999.0f;
            float targetY_R = 0.0f;
            float count_R = 0.0f;

            for (int i = 0; i < (int)g_cards.size(); ++i) {
                AppCard& card = g_cards[i];
                float cardW = card.currentWidthRadius * card.currentScale;
                if (i < g_selectedIndex) {
                    targetX1_L = (std::min)(targetX1_L, card.currentX - cardW);
                    targetX2_L = (std::max)(targetX2_L, card.currentX + cardW);
                    targetY_L += card.currentY;
                    count_L += 1.0f;
                } else if (i > g_selectedIndex) {
                    targetX1_R = (std::min)(targetX1_R, card.currentX - cardW);
                    targetX2_R = (std::max)(targetX2_R, card.currentX + cardW);
                    targetY_R += card.currentY;
                    count_R += 1.0f;
                }
            }

            float targetScale_L = 0.0f;
            if (count_L > 0.1f) {
                targetY_L /= count_L;
                targetScale_L = 1.0f;
                targetX1_L -= 32.0f;
                targetX2_L += 32.0f;
            } else {
                AppCard& activeCard = g_cards[g_selectedIndex];
                float activeW = activeCard.currentWidthRadius * activeCard.currentScale;
                targetX1_L = activeCard.currentX - activeW;
                targetX2_L = targetX1_L;
                targetY_L = activeCard.currentY + 140.0f;
                targetScale_L = 0.0f;
            }

            float targetScale_R = 0.0f;
            if (count_R > 0.1f) {
                targetY_R /= count_R;
                targetScale_R = 1.0f;
                targetX1_R -= 32.0f;
                targetX2_R += 32.0f;
            } else {
                AppCard& activeCard = g_cards[g_selectedIndex];
                float activeW = activeCard.currentWidthRadius * activeCard.currentScale;
                targetX1_R = activeCard.currentX + activeW;
                targetX2_R = targetX1_R;
                targetY_R = activeCard.currentY + 140.0f;
                targetScale_R = 0.0f;
            }

            float poolStiffness = 0.22f;
            float poolDamping = 0.65f;

            g_leftPoolVx1 = (g_leftPoolVx1 + (targetX1_L - g_leftPoolX1) * poolStiffness) * poolDamping;
            g_leftPoolVx2 = (g_leftPoolVx2 + (targetX2_L - g_leftPoolX2) * poolStiffness) * poolDamping;
            g_leftPoolVy = (g_leftPoolVy + (targetY_L - g_leftPoolY) * poolStiffness) * poolDamping;
            g_leftPoolVScale = (g_leftPoolVScale + (targetScale_L - g_leftPoolScale) * poolStiffness) * poolDamping;

            g_leftPoolX1 += g_leftPoolVx1;
            g_leftPoolX2 += g_leftPoolVx2;
            g_leftPoolY += g_leftPoolVy;
            g_leftPoolScale += g_leftPoolVScale;

            g_rightPoolVx1 = (g_rightPoolVx1 + (targetX1_R - g_rightPoolX1) * poolStiffness) * poolDamping;
            g_rightPoolVx2 = (g_rightPoolVx2 + (targetX2_R - g_rightPoolX2) * poolStiffness) * poolDamping;
            g_rightPoolVy = (g_rightPoolVy + (targetY_R - g_rightPoolY) * poolStiffness) * poolDamping;
            g_rightPoolVScale = (g_rightPoolVScale + (targetScale_R - g_rightPoolScale) * poolStiffness) * poolDamping;

            g_rightPoolX1 += g_rightPoolVx1;
            g_rightPoolX2 += g_rightPoolVx2;
            g_rightPoolY += g_rightPoolVy;
            g_rightPoolScale += g_rightPoolVScale;
        }

        if (g_theme == 2) {
            if (g_leftPoolScale > 0.01f) {
                float poolH = (380.0f * 0.35f + 64.0f) * g_leftPoolScale; 
                float cornerRadius = poolH * 0.5f;
                D2D1_RECT_F poolRect = D2D1::RectF(g_leftPoolX1, g_leftPoolY - poolH * 0.5f, g_leftPoolX2, g_leftPoolY + poolH * 0.5f);
                D2D1_ROUNDED_RECT roundedPool = { poolRect, cornerRadius, cornerRadius };

                // Obsidian deep-dark puddle color
                g_pGlassBrush->SetColor(D2D1::ColorF(0.04f, 0.04f, 0.06f, 0.40f * g_introOutroProgress * g_leftPoolScale));
                g_pDCRenderTarget->FillRoundedRectangle(roundedPool, g_pGlassBrush);

                if (g_pSpecularBrush) {
                    g_pSpecularBrush->SetStartPoint(D2D1::Point2F(poolRect.left, poolRect.top));
                    g_pSpecularBrush->SetEndPoint(D2D1::Point2F(poolRect.right, poolRect.bottom));
                    g_pSpecularBrush->SetOpacity(g_introOutroProgress * g_leftPoolScale * 0.65f);
                    g_pDCRenderTarget->DrawRoundedRectangle(roundedPool, g_pSpecularBrush, 1.2f);
                }

                if (g_pWhiteBrush) {
                    g_pWhiteBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f * g_introOutroProgress * g_leftPoolScale));
                    D2D1_RECT_F topGloss = D2D1::RectF(poolRect.left + cornerRadius, poolRect.top, poolRect.right - cornerRadius, poolRect.top + 1.5f);
                    g_pDCRenderTarget->FillRectangle(topGloss, g_pWhiteBrush);
                }
            }

            if (g_rightPoolScale > 0.01f) {
                float poolH = (380.0f * 0.35f + 64.0f) * g_rightPoolScale;
                float cornerRadius = poolH * 0.5f;
                D2D1_RECT_F poolRect = D2D1::RectF(g_rightPoolX1, g_rightPoolY - poolH * 0.5f, g_rightPoolX2, g_rightPoolY + poolH * 0.5f);
                D2D1_ROUNDED_RECT roundedPool = { poolRect, cornerRadius, cornerRadius };

                g_pGlassBrush->SetColor(D2D1::ColorF(0.04f, 0.04f, 0.06f, 0.40f * g_introOutroProgress * g_rightPoolScale));
                g_pDCRenderTarget->FillRoundedRectangle(roundedPool, g_pGlassBrush);

                if (g_pSpecularBrush) {
                    g_pSpecularBrush->SetStartPoint(D2D1::Point2F(poolRect.left, poolRect.top));
                    g_pSpecularBrush->SetEndPoint(D2D1::Point2F(poolRect.right, poolRect.bottom));
                    g_pSpecularBrush->SetOpacity(g_introOutroProgress * g_rightPoolScale * 0.65f);
                    g_pDCRenderTarget->DrawRoundedRectangle(roundedPool, g_pSpecularBrush, 1.2f);
                }

                if (g_pWhiteBrush) {
                    g_pWhiteBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f * g_introOutroProgress * g_rightPoolScale));
                    D2D1_RECT_F topGloss = D2D1::RectF(poolRect.left + cornerRadius, poolRect.top, poolRect.right - cornerRadius, poolRect.top + 1.5f);
                    g_pDCRenderTarget->FillRectangle(topGloss, g_pWhiteBrush);
                }
            }
        }
        
        std::vector<int> renderOrder(g_cards.size());
        for (int i = 0; i < (int)g_cards.size(); ++i) renderOrder[i] = i;
        std::sort(renderOrder.begin(), renderOrder.end(), [](int a, int b) {
            float distA = fabsf((float)a - (float)g_selectedIndex);
            float distB = fabsf((float)b - (float)g_selectedIndex);
            return distA > distB; 
        });

        for (int i : renderOrder) {
            AppCard& card = g_cards[i];
            float offset = (float)i - (float)g_selectedIndex;
            float distance = fabsf(offset);
            (void)distance; 
            
            float cardOpacity = g_introOutroProgress;
            if (g_theme != 1 && g_theme != 3 && g_theme != 0) {
                cardOpacity = g_introOutroProgress / (1.0f + distance * 0.40f);
            }

            float squishX = 0.0f;
            if (g_theme == 3) {
                squishX = card.vx * 0.16f; 
            }
            
            float cardW = (card.currentWidthRadius + squishX) * card.currentScale;
            float cardH = 380.0f * card.currentScale;
            bool isHovered = (g_mousePos.x > (card.currentX - cardW) && 
                              g_mousePos.x < (card.currentX + cardW) && 
                              g_mousePos.y > (card.currentY - cardH / 2.0f) && 
                              g_mousePos.y < (card.currentY + cardH / 2.0f));
            
            if (isHovered && i != g_selectedIndex) {
                card.currentScale += (card.targetScale * 1.05f - card.currentScale) * 0.4f;
            }
            
            D2D1_RECT_F cr = D2D1::RectF(
                card.currentX - cardW, 
                card.currentY - cardH / 2.0f, 
                card.currentX + cardW, 
                card.currentY + cardH / 2.0f
            );

            float cornerRadius = card.currentCornerRadius * card.currentScale;
            ID2D1Brush* activeBorderBrush = g_pBorderBrush;
            
            float textOpac = cardOpacity;
            float thumbOpac = (i == g_selectedIndex ? 1.0f : 0.6f);

            if (g_theme == 0) {
                // Completely solid cards that fade to black but do NOT become transparent to each other
                float depthDarken = 1.0f / (1.0f + distance * 0.50f);
                cardOpacity = g_introOutroProgress;
                thumbOpac = (i == g_selectedIndex ? 1.0f : 0.8f * depthDarken);
                textOpac = depthDarken * g_introOutroProgress;
                
                g_pGlassBrush->SetColor(D2D1::ColorF(0.12f * depthDarken, 0.12f * depthDarken, 0.15f * depthDarken, 1.0f * g_introOutroProgress));
                if (i == g_selectedIndex) {
                    g_pBorderBrush->SetColor(D2D1::ColorF(0.85f, 0.35f, 0.95f, 1.0f * g_introOutroProgress));
                } else {
                    g_pBorderBrush->SetColor(D2D1::ColorF(0.85f * depthDarken, 0.85f * depthDarken, 0.95f * depthDarken, 0.4f * g_introOutroProgress));
                }
                g_pTextBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, textOpac));
            } else if (g_theme == 2) {
                float scaleRatio = (card.currentScale - 0.20f) / (0.85f - 0.20f);
                if (scaleRatio < 0.0f) scaleRatio = 0.0f;
                if (scaleRatio > 1.0f) scaleRatio = 1.0f;

                textOpac = cardOpacity * (0.15f + 0.85f * powf(scaleRatio, 1.5f)); 
                thumbOpac = 0.35f + (0.65f * scaleRatio); 
                
                g_pTextBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, textOpac));
            } else if (g_theme == 3) {
                D2D1_COLOR_F baseColor = GetM3Color(i, (i == g_selectedIndex ? 0.90f : 0.65f) * cardOpacity);
                if (i == g_selectedIndex) {
                    g_pTextBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f * cardOpacity)); 
                } else {
                    g_pTextBrush->SetColor(D2D1::ColorF(0.1f, 0.1f, 0.15f, 1.0f * cardOpacity));
                }
                g_pGlassBrush->SetColor(baseColor);
                g_pBorderBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)); 
            } else {
                g_pGlassBrush->SetColor(D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.65f * cardOpacity));
                if (i == g_selectedIndex) {
                    if (g_theme == 1) {
                        g_pBorderBrush->SetColor(D2D1::ColorF(0.92f, 0.92f, 0.95f, 0.95f * cardOpacity));
                    } else {
                        g_pBorderBrush->SetColor(D2D1::ColorF(0.85f, 0.35f, 0.95f, 0.85f * cardOpacity));
                    }
                } else {
                    g_pBorderBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f * cardOpacity));
                }
                g_pTextBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f * cardOpacity));
            }

            D2D1_ROUNDED_RECT roundedCard = { cr, cornerRadius, cornerRadius };

            if (g_theme == 0 && g_introOutroProgress > 0.01f) {
                D2D1_ROUNDED_RECT shadowCard = roundedCard;
                shadowCard.rect.left += 15.0f * card.currentScale;
                shadowCard.rect.right += 15.0f * card.currentScale;
                shadowCard.rect.top += 25.0f * card.currentScale;
                shadowCard.rect.bottom += 25.0f * card.currentScale;
                g_pDimBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f * g_introOutroProgress));
                g_pDCRenderTarget->FillRoundedRectangle(shadowCard, g_pDimBrush);
            }

            if (g_theme != 2) {
                g_pDCRenderTarget->FillRoundedRectangle(roundedCard, g_pGlassBrush);
                if (g_theme != 3) {
                    g_pDCRenderTarget->DrawRoundedRectangle(roundedCard, activeBorderBrush, 2.0f);
                } else if (i == g_selectedIndex) {
                    g_pBorderBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.50f * cardOpacity));
                    g_pDCRenderTarget->DrawRoundedRectangle(roundedCard, g_pBorderBrush, 3.0f);
                }
            } else if (g_theme == 2) {
                if (i == g_selectedIndex) {
                    g_pGlassBrush->SetColor(D2D1::ColorF(0.92f, 0.94f, 0.98f, 0.24f * cardOpacity));
                    g_pDCRenderTarget->FillRoundedRectangle(roundedCard, g_pGlassBrush);

                    if (g_pSpecularBrush) {
                        g_pSpecularBrush->SetStartPoint(D2D1::Point2F(cr.left, cr.top));
                        g_pSpecularBrush->SetEndPoint(D2D1::Point2F(cr.right, cr.bottom));
                        g_pSpecularBrush->SetOpacity(cardOpacity * 0.95f);
                        g_pDCRenderTarget->DrawRoundedRectangle(roundedCard, g_pSpecularBrush, 1.5f);
                    }
                    if (g_pWhiteBrush) {
                        g_pWhiteBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f * cardOpacity));
                        D2D1_RECT_F topGloss = D2D1::RectF(cr.left + cornerRadius, cr.top, cr.right - cornerRadius, cr.top + 1.5f);
                        g_pDCRenderTarget->FillRectangle(topGloss, g_pWhiteBrush);
                    }
                }
            }

            if (g_pTextFormat && g_pTextBrush && textOpac > 0.01f) {
                float textPadding = cornerRadius + (10.0f * card.currentScale);
                if (g_theme != 3 && g_theme != 2) textPadding = 20.0f * card.currentScale;

                g_pDCRenderTarget->DrawText(
                    card.title.c_str(), 
                    (UINT32)card.title.length(), 
                    g_pTextFormat, 
                    D2D1::RectF(cr.left + textPadding, cr.top + 12 * card.currentScale, cr.right - textPadding, cr.top + 36 * card.currentScale), 
                    g_pTextBrush
                );
            }
            
            float cx = cr.right - 25.0f * card.currentScale;
            float cy = cr.top + 25.0f * card.currentScale;
            float distSq = (g_mousePos.x - cx) * (g_mousePos.x - cx) + (g_mousePos.y - cy) * (g_mousePos.y - cy);
            bool isCloseHovered = (distSq < (144.0f * card.currentScale * card.currentScale));
            
            if (thumbOpac > 0.01f && textOpac > 0.01f) {
                if (g_theme == 3) {
                    if (isCloseHovered) g_pCloseBrush->SetColor(D2D1::ColorF(0.95f, 0.15f, 0.2f, 1.0f * cardOpacity));
                    else g_pCloseBrush->SetColor(D2D1::ColorF(0.1f, 0.1f, 0.15f, 0.35f * cardOpacity));
                } else {
                    if (isCloseHovered) g_pCloseBrush->SetColor(D2D1::ColorF(1.0f, 0.1f, 0.1f, textOpac));
                    else g_pCloseBrush->SetColor(D2D1::ColorF(0.8f, 0.2f, 0.2f, 0.7f * textOpac));
                }
                
                if (g_theme != 2 || i == g_selectedIndex) {
                    D2D1_ELLIPSE closeEllipse = { {cx, cy}, 11.0f * card.currentScale, 11.0f * card.currentScale };
                    g_pDCRenderTarget->FillEllipse(closeEllipse, g_pCloseBrush);
                    
                    if (g_pWhiteBrush) {
                        g_pWhiteBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, textOpac));
                        g_pDCRenderTarget->DrawLine(D2D1::Point2F(cx - 4 * card.currentScale, cy - 4 * card.currentScale), D2D1::Point2F(cx + 4 * card.currentScale, cy + 4 * card.currentScale), g_pWhiteBrush, 2.0f);
                        g_pDCRenderTarget->DrawLine(D2D1::Point2F(cx + 4 * card.currentScale, cy - 4 * card.currentScale), D2D1::Point2F(cx - 4 * card.currentScale, cy + 4 * card.currentScale), g_pWhiteBrush, 2.0f);
                    }
                }
            }
            
            if (card.thumbnail) {
                DWM_THUMBNAIL_PROPERTIES tp = {0}; 
                tp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY; 
                tp.fVisible = (thumbOpac > 0.02f) ? TRUE : FALSE;
                
                int padX = (int)(cornerRadius * 0.35f) + (int)(10.0f * card.currentScale);
                int padYBottom = (int)(cornerRadius * 0.35f) + (int)(10.0f * card.currentScale);
                int padYTop = (int)(45.0f * card.currentScale);
                
                tp.rcDestination = { 
                    (int)(cr.left + padX), 
                    (int)(cr.top + padYTop), 
                    (int)(cr.right - padX), 
                    (int)(cr.bottom - padYBottom) 
                }; 
                tp.opacity = (BYTE)(255.0f * cardOpacity * thumbOpac);

                if (thumbOpac > 0.02f && g_pDimBrush) {
                    float totalThumbAlpha = (g_theme == 0) ? g_introOutroProgress : (cardOpacity * thumbOpac);
                    g_pDimBrush->SetColor(D2D1::ColorF(0.06f, 0.06f, 0.08f, totalThumbAlpha));
                    D2D1_RECT_F thumbRect = D2D1::RectF(tp.rcDestination.left, tp.rcDestination.top, tp.rcDestination.right, tp.rcDestination.bottom);
                    g_pDCRenderTarget->FillRectangle(thumbRect, g_pDimBrush);
                }

                DwmUpdateThumbnailProperties(card.thumbnail, &tp);
            }
        }
        
        HRESULT hr = g_pDCRenderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            g_pDCRenderTarget->Release();
            g_pDCRenderTarget = nullptr;
            if (g_pDeviceContext) { g_pDeviceContext->Release(); g_pDeviceContext = nullptr; }
        }
    }
    
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT p0 = { 0, 0 }; 
    SIZE s0 = { vW, vH }; 
    POINT p1 = { vX, vY };
    UpdateLayeredWindow(hwnd, hdcScreen, &p1, &s0, g_hdcMem, &p0, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, hdcScreen);
}

DWORD WINAPI OverlayThreadProc(LPVOID lpParam) {
    HANDLE hEvent = (HANDLE)lpParam;
    
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_pD2DFactory);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_pDWriteFactory));
    
    if (g_pDWriteFactory) {
        if (!g_pTextFormat) {
            g_pDWriteFactory->CreateTextFormat(L"Segoe UI Variable Text", NULL, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &g_pTextFormat);
            if (g_pTextFormat) {
                g_pTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                g_pTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                g_pTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

                IDWriteInlineObject* pEllipsis = nullptr;
                if (SUCCEEDED(g_pDWriteFactory->CreateEllipsisTrimmingSign(g_pTextFormat, &pEllipsis))) {
                    DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                    g_pTextFormat->SetTrimming(&trimming, pEllipsis);
                    pEllipsis->Release();
                }
            }
        }
    }

    HINSTANCE hInstance = GetModuleHandle(NULL);
    const wchar_t CLASS_NAME[] = L"GlassAltTabOverlayClass";

    WNDCLASS wc = { };
    wc.lpfnWndProc = OverlayWndProc; 
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    RegisterClass(&wc);

    int screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g_overlayHwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        CLASS_NAME, L"GlassAltTabOverlay", WS_POPUP,
        screenX, screenY, screenW, screenH,
        NULL, NULL, hInstance, NULL
    );

    if (!g_overlayHwnd) {
        if (hEvent) SetEvent(hEvent);
        return 0;
    }

    int backdrop = 3; 
    DwmSetWindowAttribute(g_overlayHwnd, 38, &backdrop, sizeof(backdrop));

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto setWindowCompositionAttribute = (SetWindowCompositionAttribute_t)GetProcAddress(user32, "SetWindowCompositionAttribute");
        if (setWindowCompositionAttribute) {
            AccentPolicy policy = { 4, 1, 0x90000000, 0 }; 
            CompositionAttributeData data = { 19, &policy, sizeof(policy) }; 
            setWindowCompositionAttribute(g_overlayHwnd, &data);
        }
    }

    if (hEvent) SetEvent(hEvent);

    timeBeginPeriod_t pTimeBeginPeriod = nullptr;
    timeEndPeriod_t pTimeEndPeriod = nullptr;
    HMODULE hWinmm = LoadLibraryW(L"winmm.dll");
    if (hWinmm) {
        pTimeBeginPeriod = (timeBeginPeriod_t)GetProcAddress(hWinmm, "timeBeginPeriod");
        pTimeEndPeriod = (timeEndPeriod_t)GetProcAddress(hWinmm, "timeEndPeriod");
    }

    if (pTimeBeginPeriod) pTimeBeginPeriod(1);
    SetTimer(g_overlayHwnd, 1, 8, RenderTimerProc);

    MSG msg;
    while (g_threadRunning && GetMessage(&msg, NULL, 0, 0)) {
        DispatchMessage(&msg);
    }

    KillTimer(g_overlayHwnd, 1);
    if (pTimeEndPeriod) pTimeEndPeriod(1);
    if (hWinmm) FreeLibrary(hWinmm);
    
    for (auto& card : g_cards) {
        if (card.thumbnail) DwmUnregisterThumbnail(card.thumbnail);
    }
    g_cards.clear();

    if (g_pSpecularBrush) { g_pSpecularBrush->Release(); g_pSpecularBrush = nullptr; }
    if (g_pGlassBrush) { g_pGlassBrush->Release(); g_pGlassBrush = nullptr; }
    if (g_pBorderBrush) { g_pBorderBrush->Release(); g_pBorderBrush = nullptr; }
    if (g_pCloseBrush) { g_pCloseBrush->Release(); g_pCloseBrush = nullptr; }
    if (g_pTextBrush) { g_pTextBrush->Release(); g_pTextBrush = nullptr; }
    if (g_pDimBrush) { g_pDimBrush->Release(); g_pDimBrush = nullptr; }
    if (g_pWhiteBrush) { g_pWhiteBrush->Release(); g_pWhiteBrush = nullptr; }
    
    if (g_pDeviceContext) { g_pDeviceContext->Release(); g_pDeviceContext = nullptr; }
    
    if (g_pTextFormat) { g_pTextFormat->Release(); g_pTextFormat = nullptr; }
    if (g_pDWriteFactory) { g_pDWriteFactory->Release(); g_pDWriteFactory = nullptr; }
    if (g_pDCRenderTarget) { g_pDCRenderTarget->Release(); g_pDCRenderTarget = nullptr; }
    if (g_pD2DFactory) { g_pD2DFactory->Release(); g_pD2DFactory = nullptr; }
    if (g_hBitmap) { DeleteObject(g_hBitmap); g_hBitmap = NULL; }
    if (g_hdcMem) { DeleteDC(g_hdcMem); g_hdcMem = NULL; }

    DestroyWindow(g_overlayHwnd);
    UnregisterClass(CLASS_NAME, hInstance);
    CoUninitialize();
    return 0;
}

struct AutoThreadId {
    AutoThreadId() { g_threadIdForAltTabShowWindow = GetCurrentThreadId(); }
    ~AutoThreadId() { g_threadIdForAltTabShowWindow = 0; }
};

using XamlAltTabViewHost_ViewLoaded_t = void(WINAPI*)(void*);
XamlAltTabViewHost_ViewLoaded_t XamlAltTabViewHost_ViewLoaded_Original;
void WINAPI XamlAltTabViewHost_ViewLoaded_Hook(void* pThis) {
    AutoThreadId lock;
    XamlAltTabViewHost_ViewLoaded_Original(pThis);
}

using XamlAltTabViewHost_DisplayAltTab_t = void(WINAPI*)(void*);
XamlAltTabViewHost_DisplayAltTab_t XamlAltTabViewHost_DisplayAltTab_Original;
void WINAPI XamlAltTabViewHost_DisplayAltTab_Hook(void* pThis) {
    if (g_threadIdForAltTabShowWindow != 0) return XamlAltTabViewHost_DisplayAltTab_Original(pThis);
    AutoThreadId lock;
    XamlAltTabViewHost_DisplayAltTab_Original(pThis);
}

using CAltTabViewHost_Show_t = HRESULT(WINAPI*)(void*, void*, void*, void*);
CAltTabViewHost_Show_t CAltTabViewHost_Show_Original;
HRESULT WINAPI CAltTabViewHost_Show_Hook(void* pThis, void* p1, void* p2, void* p3) {
    AutoThreadId lock;
    return CAltTabViewHost_Show_Original(pThis, p1, p2, p3);
}

using ShowWindow_t = decltype(&ShowWindow);
ShowWindow_t ShowWindow_Original;
BOOL WINAPI ShowWindow_Hook(HWND hWnd, int nCmdShow) {
    if (nCmdShow != SW_HIDE) {
        // Note: This suppresses ShowWindow calls for ANY window on the thread while the 
        // internal immersive layout methods are executing. Explorer's Alt-Tab thread is 
        // highly isolated, so this safely catches the native switcher without collateral damage.
        if (g_threadIdForAltTabShowWindow == GetCurrentThreadId()) {
            return TRUE; 
        }
        
        wchar_t className[256];
        if (GetClassNameW(hWnd, className, 256)) {
            if (wcscmp(className, L"XamlExplorerHostIslandWindow") == 0 ||
                wcscmp(className, L"MultitaskingViewFrame") == 0 ||
                wcscmp(className, L"TaskSwitcherWnd") == 0 ||
                wcscmp(className, L"Shell_InputSwitchTopLevelWindow") == 0) {
                return TRUE; 
            }
        }
    }
    return ShowWindow_Original(hWnd, nCmdShow);
}

BOOL Wh_ModInit() {
    LoadSettings();
    HMODULE twinui = LoadLibraryW(L"twinui.pcshell.dll");
    if (twinui) {
        WindhawkUtils::SYMBOL_HOOK hooks[] = {
            { {LR"(public: virtual long __cdecl XamlAltTabViewHost::ViewLoaded(void))"}, &XamlAltTabViewHost_ViewLoaded_Original, XamlAltTabViewHost_ViewLoaded_Hook, true },
            { {LR"(private: void __cdecl XamlAltTabViewHost::DisplayAltTab(void))"}, &XamlAltTabViewHost_DisplayAltTab_Original, XamlAltTabViewHost_DisplayAltTab_Hook, true },
            { {LR"(public: virtual long __cdecl CAltTabViewHost::Show(struct IImmersiveMonitor *,enum ALT_TAB_VIEW_FLAGS,struct IApplicationView *))"}, &CAltTabViewHost_Show_Original, CAltTabViewHost_Show_Hook, true }
        };
        if (!WindhawkUtils::HookSymbols(twinui, hooks, ARRAYSIZE(hooks))) {
            Wh_Log(L"Dynamic Alt-Tab: Failed to hook twinui.pcshell.dll symbols. Mod may not function correctly.");
            return FALSE;
        }
    } else {
        Wh_Log(L"Dynamic Alt-Tab: twinui.pcshell.dll not found. System unsupported.");
        return FALSE;
    }
    
    if (!WindhawkUtils::SetFunctionHook((void*)ShowWindow, (void*)ShowWindow_Hook, (void**)&ShowWindow_Original)) {
        Wh_Log(L"Dynamic Alt-Tab: Failed to hook ShowWindow. Mod may not function correctly.");
        return FALSE;
    }

    HANDLE hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!hEvent) return FALSE;
    
    g_threadRunning = true;
    g_renderThreadHandle = CreateThread(NULL, 0, OverlayThreadProc, hEvent, 0, NULL);
    if (!g_renderThreadHandle) {
        CloseHandle(hEvent);
        return FALSE;
    }
    
    WaitForSingleObject(hEvent, 3000);
    CloseHandle(hEvent);
    
    HANDLE hHookEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_hookThreadHandle = CreateThread(NULL, 0, HookThreadProc, hHookEvent, 0, &g_hookThreadId);
    if (g_hookThreadHandle) {
        WaitForSingleObject(hHookEvent, 3000);
        CloseHandle(hHookEvent);
    }
    
    return TRUE;
}

void Wh_ModUninit() {
    if (g_hookThreadId) {
        PostThreadMessage(g_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThreadHandle, INFINITE);
        CloseHandle(g_hookThreadHandle);
        g_hookThreadHandle = NULL;
    }
    g_threadRunning = false;
    if (g_overlayHwnd) {
        PostMessage(g_overlayHwnd, WM_QUIT, 0, 0);
    }
    if (g_renderThreadHandle) {
        WaitForSingleObject(g_renderThreadHandle, INFINITE);
        CloseHandle(g_renderThreadHandle);
        g_renderThreadHandle = NULL;
    }
}
