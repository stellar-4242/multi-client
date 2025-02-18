#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>      // For GET_X_LPARAM, GET_Y_LPARAM
#include <dwmapi.h>
#include <gdiplus.h>       // GDI+ header
#include <shlobj.h>        // For SHGetFolderPathW
#include <tlhelp32.h>      // For process snapshot APIs
#include <winhttp.h>       // For HTTP requests
#include <vector>
#include <fstream>
#include <sstream>
#include <string>
#include <cwctype>         // For iswdigit
#include <unordered_map>
#include <algorithm>
#include "resource.h"      // Resource header (defines IDI_APP_ICON)
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")

using namespace Gdiplus;
using std::vector;
using std::wstring;
using std::unordered_map;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// Command IDs for tray menu items.
#define ID_TRAY_SHOW 1001
#define ID_TRAY_EXIT 1002
#define WM_TRAYICON (WM_USER + 1)

// Global GDI+ token.
ULONG_PTR g_gdiplusToken = 0;

// Tray icon data.
NOTIFYICONDATA nid = {0};

// Global handle for custom tray menu window.
HWND g_hTrayMenuWnd = NULL;

// Global map to store locked numeric UserIDs (PID -> UserID).
static unordered_map<DWORD, wstring> g_pidUserMap;
// Global cache for display names (numeric UserID -> displayName).
static unordered_map<wstring, wstring> g_displayNameCache;

// Global vectors for process buttons.
static vector<RECT> g_buttonRects;
static vector<DWORD> g_buttonPIDs;

//-----------------------------------------------------------------
// Function: GetUserIdForProcess
// Description:
//   Obtains the process creation time, determines the current user's Roblox logs
//   folder, and returns the numeric userId extracted from the most recent log file
//   that was updated after the process creation time.
//-----------------------------------------------------------------
wstring GetUserIdForProcess(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) return L"";
    FILETIME ftCreation, ftExit, ftKernel, ftUser;
    if (!GetProcessTimes(hProc, &ftCreation, &ftExit, &ftKernel, &ftUser)) {
         CloseHandle(hProc);
         return L"";
    }
    ULONGLONG processCreationTime = (((ULONGLONG)ftCreation.dwHighDateTime) << 32) | ftCreation.dwLowDateTime;
    CloseHandle(hProc);

    WCHAR localAppData[MAX_PATH];
    wstring logsFolder;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
        logsFolder = localAppData;
        logsFolder += L"\\Roblox\\logs\\";
    } else {
        logsFolder = L"C:\\RobloxLogs\\";
    }

    wstring searchPattern = logsFolder + L"*.*";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return L"";
    ULONGLONG bestTime = 0;
    wstring bestFile;
    do {
         if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
             ULONGLONG fileTime = (((ULONGLONG)findData.ftLastWriteTime.dwHighDateTime) << 32)
                                  | findData.ftLastWriteTime.dwLowDateTime;
             if (fileTime >= processCreationTime && fileTime > bestTime) {
                 bestTime = fileTime;
                 bestFile = findData.cFileName;
             }
         }
    } while (FindNextFileW(hFind, &findData));
    FindClose(hFind);
    if (bestFile.empty()) return L"";
    wstring filePath = logsFolder + bestFile;
    std::wifstream fileStream(filePath.c_str());
    if (!fileStream.is_open()) return L"";
    wstring line, userId;
    while (std::getline(fileStream, line)) {
         if (line.find(L"GameJoinLoadTime") != wstring::npos) {
             size_t pos = line.find(L"userid:");
             if (pos != wstring::npos) {
                 pos += 7;
                 size_t start = pos;
                 while (pos < line.size() && line[pos] != L',')
                     pos++;
                 userId = line.substr(start, pos - start);
                 break;
             }
         }
    }
    fileStream.close();
    return userId;
}

//-----------------------------------------------------------------
// Function: GetDisplayNameForUserId
// Description:
//   Performs an HTTPS GET request to "https://users.roblox.com/v1/users/<userId>"
//   and extracts the "displayName" field from the returned JSON.
//-----------------------------------------------------------------
wstring GetDisplayNameForUserId(const wstring &userId) {
    wstring urlPath = L"/v1/users/" + userId;
    HINTERNET hSession = WinHttpOpen(L"ModernUI/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return L"";
    HINTERNET hConnect = WinHttpConnect(hSession, L"users.roblox.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return L""; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath.c_str(), NULL,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return L""; }
    BOOL bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if(bResults)
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    if (!bResults) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return L"";
    }
    DWORD dwSize = 0, dwDownloaded = 0;
    std::string response;
    do {
        if(!WinHttpQueryDataAvailable(hRequest, &dwSize))
            break;
        if(dwSize == 0)
            break;
        char* buffer = new char[dwSize+1];
        ZeroMemory(buffer, dwSize+1);
        if(WinHttpReadData(hRequest, (LPVOID)buffer, dwSize, &dwDownloaded))
            response.append(buffer, dwDownloaded);
        delete[] buffer;
    } while(dwSize > 0);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    size_t pos = response.find("\"displayName\":\"");
    if (pos == std::string::npos) return L"";
    pos += strlen("\"displayName\":\"");
    size_t endPos = response.find("\"", pos);
    if (endPos == std::string::npos) return L"";
    std::string displayNameUtf8 = response.substr(pos, endPos - pos);
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, displayNameUtf8.c_str(), -1, NULL, 0);
    wchar_t* wstr = new wchar_t[size_needed];
    MultiByteToWideChar(CP_UTF8, 0, displayNameUtf8.c_str(), -1, wstr, size_needed);
    wstring displayName(wstr);
    delete[] wstr;
    return displayName;
}

//-----------------------------------------------------------------
// Function: GetRobloxProcesses
// Description:
//   Returns a vector of PIDs for all running processes named "RobloxPlayerBeta.exe".
//-----------------------------------------------------------------
vector<DWORD> GetRobloxProcesses() {
    vector<DWORD> pids;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != NULL && hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnapshot, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"RobloxPlayerBeta.exe") == 0) {
                    pids.push_back(pe.th32ProcessID);
                }
            } while (Process32NextW(hSnapshot, &pe));
        }
        CloseHandle(hSnapshot);
    }
    return pids;
}

//-----------------------------------------------------------------
// Custom Tray Menu Window (Owner-drawn)
//-----------------------------------------------------------------
LRESULT CALLBACK TrayMenuProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            HRGN rgn = CreateRoundRectRgn(0, 0, rc.right, rc.bottom, 10, 10);
            SetWindowRgn(hwnd, rgn, TRUE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH hBrush = CreateSolidBrush(RGB(32,32,32));
            FillRect(hdc, &rc, hBrush);
            DeleteObject(hBrush);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255,255,255));
            HFONT hFont = CreateFont(16, 0, 0, 0, FW_NORMAL,
                                     FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
            RECT itemRect = rc;
            itemRect.bottom = itemRect.top + 24;
            DrawTextW(hdc, L"Show", -1, &itemRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            itemRect.top += 24;
            itemRect.bottom += 24;
            DrawTextW(hdc, L"Exit", -1, &itemRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            SelectObject(hdc, hOldFont);
            DeleteObject(hFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            int itemIndex = pt.y / 24;
            if(itemIndex == 0) {
                PostMessage(GetParent(hwnd), WM_COMMAND, ID_TRAY_SHOW, 0);
            } else if(itemIndex == 1) {
                PostMessage(GetParent(hwnd), WM_COMMAND, ID_TRAY_EXIT, 0);
            }
            DestroyWindow(hwnd);
            g_hTrayMenuWnd = NULL;
            return 0;
        }
        case WM_KILLFOCUS: {
            DestroyWindow(hwnd);
            g_hTrayMenuWnd = NULL;
            return 0;
        }
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// Function to show the custom tray menu.
void ShowCustomTrayMenu(HWND parent, POINT pt) {
    if(g_hTrayMenuWnd != NULL) return;
    g_hTrayMenuWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"TrayMenuWindow", L"",
                                     WS_POPUP, pt.x, pt.y, 100, 48,
                                     parent, NULL, GetModuleHandle(NULL), NULL);
    ShowWindow(g_hTrayMenuWnd, SW_SHOWNA);
}

//-----------------------------------------------------------------
// Main Window Procedure
//-----------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static RECT mainCloseButtonRect = {0,0,0,0};
    switch(msg) {
        case WM_CREATE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            HRGN hrgn = CreateRoundRectRgn(0,0,rc.right,rc.bottom,20,20);
            SetWindowRgn(hwnd, hrgn, TRUE);
            // Load the icon from the embedded resource.
            HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON));
            if(hIcon) {
                SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
                SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
                nid.hIcon = hIcon;
            }
            // Always on top and add tray icon.
            SetWindowLong(hwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TOPMOST);
            SetLayeredWindowAttributes(hwnd, 0, 230, LWA_ALPHA);
            BOOL darkMode = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
            GdiplusStartupInput gdiplusStartupInput;
            GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);
            SetTimer(hwnd, 1, 100, NULL);
            // Register custom tray menu window class.
            WNDCLASS wc = {0};
            wc.lpfnWndProc = TrayMenuProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.lpszClassName = L"TrayMenuWindow";
            RegisterClass(&wc);
            // Setup tray icon.
            nid.cbSize = sizeof(NOTIFYICONDATA);
            nid.hWnd = hwnd;
            nid.uID = 1;
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            nid.uCallbackMessage = WM_TRAYICON;
            wcscpy_s(nid.szTip, L"Roblox Multi Client");
            Shell_NotifyIcon(NIM_ADD, &nid);
            break;
        }
        case WM_TRAYICON: {
            if(lParam == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                ShowCustomTrayMenu(hwnd, pt);
            }
            return 0;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if(wmId == ID_TRAY_SHOW) {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            } else if(wmId == ID_TRAY_EXIT) {
                DestroyWindow(hwnd);
            }
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER: {
            InvalidateRect(hwnd, NULL, TRUE);
            break;
        }
        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            RECT rcClient;
            GetClientRect(hwnd, &rcClient);
            if(pt.y < 30) {
                mainCloseButtonRect.left = rcClient.right - 30;
                mainCloseButtonRect.top = 5;
                mainCloseButtonRect.right = rcClient.right - 10;
                mainCloseButtonRect.bottom = 25;
                if(pt.x >= mainCloseButtonRect.left && pt.x < mainCloseButtonRect.right &&
                   pt.y >= mainCloseButtonRect.top && pt.y < mainCloseButtonRect.bottom)
                    return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if(pt.x >= mainCloseButtonRect.left && pt.x < mainCloseButtonRect.right &&
               pt.y >= mainCloseButtonRect.top && pt.y < mainCloseButtonRect.bottom) {
                // Clicking the X hides the window to tray.
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            for(size_t i = 0; i < g_buttonRects.size(); i++) {
                if(pt.x >= g_buttonRects[i].left && pt.x < g_buttonRects[i].right &&
                   pt.y >= g_buttonRects[i].top && pt.y < g_buttonRects[i].bottom) {
                    DWORD pid = g_buttonPIDs[i];
                    // Close process by enumerating windows and sending WM_CLOSE.
                    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                        DWORD pid;
                        GetWindowThreadProcessId(hwnd, &pid);
                        if(pid == (DWORD)lParam && IsWindowVisible(hwnd)) {
                            SendMessageTimeout(hwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 2000, NULL);
                            return FALSE;
                        }
                        return TRUE;
                    }, (LPARAM)pid);
                    break;
                }
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;
            Bitmap buffer(width, height, PixelFormat32bppARGB);
            Graphics graphicsBuffer(&buffer);
            graphicsBuffer.SetSmoothingMode(SmoothingModeAntiAlias);
            graphicsBuffer.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            SolidBrush bgBrush(Color(230,32,32,32));
            Rect fullRect(0,0,width,height);
            graphicsBuffer.FillRectangle(&bgBrush, fullRect);
            SolidBrush titleBarBrush(Color(255,28,28,28));
            Rect titleBarRect(0,0,width,30);
            graphicsBuffer.FillRectangle(&titleBarBrush, titleBarRect);
            NONCLIENTMETRICS ncm = {0};
            ncm.cbSize = sizeof(ncm);
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            FontFamily fontFamily(ncm.lfCaptionFont.lfFaceName);
            REAL fontSize = (REAL)abs(ncm.lfCaptionFont.lfHeight);
            Font titleFont(&fontFamily, fontSize, FontStyleRegular, UnitPixel);
            SolidBrush textBrush(Color(255,255,255,255));
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);
            format.SetLineAlignment(StringAlignmentCenter);
            RectF titleBarF(0,0,(REAL)width,30);
            graphicsBuffer.DrawString(L"Roblox Multi Client", -1, &titleFont, titleBarF, &format, &textBrush);
            RectF closeRectF((REAL)width - 30, 5.0f, 20.0f, 20.0f);
            float margin = 5.65f;
            margin *= 1.15f;
            PointF p1(closeRectF.X + margin, closeRectF.Y + margin);
            PointF p2(closeRectF.X + closeRectF.Width - margin, closeRectF.Y + closeRectF.Height - margin);
            PointF p3(closeRectF.X + closeRectF.Width - margin, closeRectF.Y + margin);
            PointF p4(closeRectF.X + margin, closeRectF.Y + closeRectF.Height - margin);
            Pen pen(Color(255,255,255,255), 1.0f);
            graphicsBuffer.DrawLine(&pen, p1, p2);
            graphicsBuffer.DrawLine(&pen, p3, p4);
            vector<DWORD> processes = GetRobloxProcesses();
            wchar_t counterText[64];
            swprintf_s(counterText, 64, L"Roblox Clients: %u", (unsigned int)processes.size());
            Font counterFont(&fontFamily, fontSize * 1.4f, FontStyleRegular, UnitPixel);
            RectF counterRect(0,30,(REAL)width,40);
            StringFormat counterFormat;
            counterFormat.SetAlignment(StringAlignmentCenter);
            counterFormat.SetLineAlignment(StringAlignmentCenter);
            graphicsBuffer.DrawString(counterText, -1, &counterFont, counterRect, &counterFormat, &textBrush);
            g_buttonRects.clear();
            g_buttonPIDs.clear();
            for(DWORD pid : processes) {
                auto it = g_pidUserMap.find(pid);
                if(it == g_pidUserMap.end() || it->second.length() <= 4) {
                    wstring fetched = GetUserIdForProcess(pid);
                    if(fetched.length() > 4)
                        g_pidUserMap[pid] = fetched;
                }
            }
            for(DWORD pid : processes) {
                auto it = g_pidUserMap.find(pid);
                if(it != g_pidUserMap.end() && !it->second.empty()) {
                    wstring numericId = it->second;
                    auto dispIt = g_displayNameCache.find(numericId);
                    if(dispIt == g_displayNameCache.end()) {
                        wstring displayName = GetDisplayNameForUserId(numericId);
                        if(!displayName.empty())
                            g_displayNameCache[numericId] = displayName;
                    }
                }
            }
            Font processFont(&fontFamily, fontSize * 1.2f, FontStyleRegular, UnitPixel);
            StringFormat btnFormat;
            btnFormat.SetAlignment(StringAlignmentCenter);
            btnFormat.SetLineAlignment(StringAlignmentCenter);
            btnFormat.SetTrimming(StringTrimmingNone);
            int startY = 75;
            int buttonHeight = 30;
            int buttonSpacing = 5;
            for(size_t i = 0; i < processes.size(); i++) {
                int btnY = startY + (int)i * (buttonHeight + buttonSpacing);
                RECT btnRect = { 10, btnY, width - 10, btnY + buttonHeight };
                g_buttonRects.push_back(btnRect);
                g_buttonPIDs.push_back(processes[i]);
                Rect btnRectG(btnRect.left, btnRect.top, btnRect.right - btnRect.left, btnRect.bottom - btnRect.top);
                SolidBrush btnBrush(Color(255,48,48,48));
                graphicsBuffer.FillRectangle(&btnBrush, btnRectG);
                Pen btnPen(Color(255,255,255,255), 1.0f);
                graphicsBuffer.DrawRectangle(&btnPen, btnRectG);
                wstring userId = g_pidUserMap[processes[i]];
                wstring displayName;
                if(!userId.empty()) {
                    auto it = g_displayNameCache.find(userId);
                    if(it != g_displayNameCache.end())
                        displayName = it->second;
                }
                wchar_t btnText[128];
                if(displayName.empty())
                    swprintf_s(btnText, 128, L"Close PID: %u", processes[i]);
                else
                    swprintf_s(btnText, 128, L"Close PID: %u (User: %s)", processes[i], displayName.c_str());
                RectF btnTextRect((REAL)btnRect.left, (REAL)btnRect.top, (REAL)(btnRect.right - btnRect.left), (REAL)(btnRect.bottom - btnRect.top));
                graphicsBuffer.DrawString(btnText, -1, &processFont, btnTextRect, &btnFormat, &textBrush);
            }
            Graphics graphics(hdc);
            graphics.DrawImage(&buffer, 0, 0, width, height);
            EndPaint(hwnd, &ps);
            break;
        }
        case WM_DESTROY: {
            KillTimer(hwnd, 1);
            Shell_NotifyIcon(NIM_DELETE, &nid);
            GdiplusShutdown(g_gdiplusToken);
            PostQuitMessage(0);
            break;
        }
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"ROBLOX_singletonMutex");
    if(hMutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        // MessageBoxW(NULL, L"Another instance of Roblox Multi Client is already running!", L"Error", MB_OK|MB_ICONERROR);
        return 0;
    }
    const wchar_t CLASS_NAME[] = L"ModernUIWindowClass";
    WNDCLASS wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, CLASS_NAME, L"Roblox Multi Client", WS_POPUP,
                                CW_USEDEFAULT, CW_USEDEFAULT, 400, 250,
                                NULL, NULL, hInstance, NULL);
    if(!hwnd) {
        MessageBoxW(NULL, L"Window Creation Failed!", L"Error", MB_OK);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 0;
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    MSG msg;
    while(GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return (int)msg.wParam;
}
