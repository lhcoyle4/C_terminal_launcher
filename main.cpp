#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tchar.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>

// Hotkey and Tray Icon IDs
#define IDI_TRAY_ICON   101
#define WM_TRAYICON     (WM_USER + 1)

// Menu Command IDs
#define IDM_HEADER      201
#define IDM_STARTUP     203
#define IDM_ABOUT       204
#define IDM_SETTINGS    205
#define IDM_RELOAD      206
#define IDM_EXIT        207
#define IDM_QUICK_START 3000

// Structs
struct Shortcut {
    std::string keys;
    std::string command;
};

struct ActiveHotkey {
    int id;
    UINT fsModifiers;
    UINT vk;
    std::string command;
    std::string shortcutStr;
};

// Global States
HWND g_hHelperWnd = NULL;
HICON g_hTrayIcon = NULL;
NOTIFYICONDATAA g_NID = { 0 };
std::vector<ActiveHotkey> g_ActiveHotkeys;

// Forward Declarations
LRESULT CALLBACK HelperWndProc(HWND, UINT, WPARAM, LPARAM);
HICON CreateDynamicIcon(bool enabled);
void ShowInstructionsBox(HWND hWnd);
bool IsRunAtStartupEnabled();
void SetRunAtStartup(bool enable);
bool HasStartupFlag();
std::string GetConfigFilePath();
std::vector<Shortcut> ParseConfig(const std::string& filepath);
bool ParseShortcut(const std::string& shortcutStr, UINT& fsModifiers, UINT& vk);
UINT MapKeyStringToVK(const std::string& keyStr);
void SplitCommand(const std::string& commandLine, std::string& program, std::string& args);
void LaunchCommand(std::string command);
void EnsureConfigFileExists();
void RegisterAllHotkeys(HWND hWnd);
void UnregisterAllHotkeys(HWND hWnd);

// Get the path of the config.json in the same directory as the executable
std::string GetConfigFilePath() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string dir = path;
    size_t lastSlash = dir.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        dir = dir.substr(0, lastSlash);
    }
    return dir + "\\config.json";
}

// Write default config if config.json is missing
void EnsureConfigFileExists() {
    std::string path = GetConfigFilePath();
    std::ifstream file(path);
    if (!file.is_open()) {
        std::ofstream outFile(path);
        if (outFile.is_open()) {
            outFile << "{\n"
                       "    \"shortcuts\": {\n"
                       "        \"<ctrl>+<shift>+u\": \"wsl\",\n"
                       "        \"<ctrl>+<shift>+p\": \"powershell\",\n"
                       "        \"<ctrl>+<shift>+c\": \"cmd\",\n"
                       "        \"<ctrl>+<shift>+7\": \"pwsh\",\n"
                       "        \"<ctrl>+<shift>+/\": \"pwsh -NoExit -Command agy\",\n"
                       "        \"<ctrl>+<shift>+a\": \"pwsh -Verb RunAs\"\n"
                       "    }\n"
                       "}\n";
        }
    }
}

// Unescape JSON string values (handling \", \\, \n, etc.)
std::string UnescapeJSONString(const std::string& input) {
    std::string result;
    result.reserve(input.length());
    for (size_t i = 0; i < input.length(); ++i) {
        if (input[i] == '\\' && i + 1 < input.length()) {
            char next = input[i + 1];
            if (next == '"') { result += '"'; i++; }
            else if (next == '\\') { result += '\\'; i++; }
            else if (next == '/') { result += '/'; i++; }
            else if (next == 'b') { result += '\b'; i++; }
            else if (next == 'f') { result += '\f'; i++; }
            else if (next == 'n') { result += '\n'; i++; }
            else if (next == 'r') { result += '\r'; i++; }
            else if (next == 't') { result += '\t'; i++; }
            else { result += input[i]; }
        } else {
            result += input[i];
        }
    }
    return result;
}

// Flat string-based JSON scanner for config.json
std::vector<Shortcut> ParseConfig(const std::string& filepath) {
    std::vector<Shortcut> shortcuts;
    std::ifstream file(filepath);
    if (!file.is_open()) return shortcuts;

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    size_t start = content.find("\"shortcuts\"");
    if (start == std::string::npos) return shortcuts;
    
    size_t openBrace = content.find('{', start);
    if (openBrace == std::string::npos) return shortcuts;
    
    size_t closeBrace = content.find('}', openBrace);
    if (closeBrace == std::string::npos) return shortcuts;
    
    std::string block = content.substr(openBrace + 1, closeBrace - openBrace - 1);
    
    size_t idx = 0;
    while (idx < block.length()) {
        size_t keyStart = block.find('"', idx);
        if (keyStart == std::string::npos) break;
        
        size_t keyEnd = keyStart + 1;
        for (; keyEnd < block.length(); ++keyEnd) {
            if (block[keyEnd] == '"') break;
            if (block[keyEnd] == '\\') keyEnd++;
        }
        if (keyEnd >= block.length()) break;
        
        std::string rawKey = block.substr(keyStart + 1, keyEnd - keyStart - 1);
        std::string key = UnescapeJSONString(rawKey);
        
        size_t colon = block.find(':', keyEnd + 1);
        if (colon == std::string::npos) break;
        
        size_t valStart = block.find('"', colon + 1);
        if (valStart == std::string::npos) break;
        
        size_t valEnd = valStart + 1;
        for (; valEnd < block.length(); ++valEnd) {
            if (block[valEnd] == '"') break;
            if (block[valEnd] == '\\') valEnd++;
        }
        if (valEnd >= block.length()) break;
        
        std::string rawVal = block.substr(valStart + 1, valEnd - valStart - 1);
        std::string val = UnescapeJSONString(rawVal);
        
        shortcuts.push_back({key, val});
        idx = valEnd + 1;
    }
    
    return shortcuts;
}


// Map key string to Win32 Virtual Key code
UINT MapKeyStringToVK(const std::string& keyStr) {
    if (keyStr.length() == 1) {
        char ch = keyStr[0];
        if (ch >= 'a' && ch <= 'z') {
            return (ch - 'a' + 'A');
        }
        if (ch >= 'A' && ch <= 'Z') {
            return ch;
        }
        if (ch >= '0' && ch <= '9') {
            return ch;
        }
        if (ch == '/') return VK_OEM_2;
        if (ch == '\\') return VK_OEM_5;
        if (ch == '-') return VK_OEM_MINUS;
        if (ch == '=') return VK_OEM_PLUS;
        if (ch == ',') return VK_OEM_COMMA;
        if (ch == '.') return VK_OEM_PERIOD;
        if (ch == ';') return VK_OEM_1;
        if (ch == '`') return VK_OEM_3;
        if (ch == '[') return VK_OEM_4;
        if (ch == ']') return VK_OEM_6;
        if (ch == '\'') return VK_OEM_7;
    }
    
    std::string keyLower = keyStr;
    std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);
    
    if (keyLower == "space") return VK_SPACE;
    if (keyLower == "enter" || keyLower == "return") return VK_RETURN;
    if (keyLower == "tab") return VK_TAB;
    if (keyLower == "backspace") return VK_BACK;
    if (keyLower == "escape" || keyLower == "esc") return VK_ESCAPE;
    if (keyLower == "up") return VK_UP;
    if (keyLower == "down") return VK_DOWN;
    if (keyLower == "left") return VK_LEFT;
    if (keyLower == "right") return VK_RIGHT;
    if (keyLower == "pgup" || keyLower == "pageup") return VK_PRIOR;
    if (keyLower == "pgdn" || keyLower == "pagedown") return VK_NEXT;
    if (keyLower == "home") return VK_HOME;
    if (keyLower == "end") return VK_END;
    if (keyLower == "insert" || keyLower == "ins") return VK_INSERT;
    if (keyLower == "delete" || keyLower == "del") return VK_DELETE;
    if (keyLower == "f1") return VK_F1;
    if (keyLower == "f2") return VK_F2;
    if (keyLower == "f3") return VK_F3;
    if (keyLower == "f4") return VK_F4;
    if (keyLower == "f5") return VK_F5;
    if (keyLower == "f6") return VK_F6;
    if (keyLower == "f7") return VK_F7;
    if (keyLower == "f8") return VK_F8;
    if (keyLower == "f9") return VK_F9;
    if (keyLower == "f10") return VK_F10;
    if (keyLower == "f11") return VK_F11;
    if (keyLower == "f12") return VK_F12;
    
    return 0;
}

// Parse shortcut string like "<ctrl>+<shift>+u" into Win32 modifiers and VK
bool ParseShortcut(const std::string& shortcutStr, UINT& fsModifiers, UINT& vk) {
    fsModifiers = 0;
    vk = 0;
    
    std::stringstream ss(shortcutStr);
    std::string token;
    while (std::getline(ss, token, '+')) {
        token.erase(0, token.find_first_not_of(" \t\r\n"));
        token.erase(token.find_last_not_of(" \t\r\n") + 1);
        
        std::string tokenLower = token;
        std::transform(tokenLower.begin(), tokenLower.end(), tokenLower.begin(), ::tolower);
        
        if (tokenLower == "<alt>") {
            fsModifiers |= MOD_ALT;
        } else if (tokenLower == "<ctrl>" || tokenLower == "<control>") {
            fsModifiers |= MOD_CONTROL;
        } else if (tokenLower == "<shift>") {
            fsModifiers |= MOD_SHIFT;
        } else if (tokenLower == "<win>" || tokenLower == "<super>") {
            fsModifiers |= MOD_WIN;
        } else {
            vk = MapKeyStringToVK(token);
        }
    }
    
    return (vk != 0);
}

// Split command line into executable and arguments, handling quotes
void SplitCommand(const std::string& commandLine, std::string& program, std::string& args) {
    std::string cmd = commandLine;
    cmd.erase(0, cmd.find_first_not_of(" \t\r\n"));
    cmd.erase(cmd.find_last_not_of(" \t\r\n") + 1);
    
    if (cmd.empty()) {
        program = "";
        args = "";
        return;
    }
    
    if (cmd[0] == '"') {
        size_t endQuote = cmd.find('"', 1);
        if (endQuote != std::string::npos) {
            program = cmd.substr(1, endQuote - 1);
            args = cmd.substr(endQuote + 1);
            args.erase(0, args.find_first_not_of(" \t\r\n"));
            return;
        }
    }
    
    size_t space = cmd.find_first_of(" \t");
    if (space != std::string::npos) {
        program = cmd.substr(0, space);
        args = cmd.substr(space + 1);
        args.erase(0, args.find_first_not_of(" \t\r\n"));
    } else {
        program = cmd;
        args = "";
    }
}

// Launch the command relative to USERPROFILE directory
void LaunchCommand(std::string command) {
    bool runAsAdmin = false;
    std::string runAsSuffix = " -verb runas";
    std::string cmdLower = command;
    std::transform(cmdLower.begin(), cmdLower.end(), cmdLower.begin(), ::tolower);
    
    if (cmdLower.length() > runAsSuffix.length() && 
        cmdLower.substr(cmdLower.length() - runAsSuffix.length()) == runAsSuffix) {
        runAsAdmin = true;
        command = command.substr(0, command.length() - runAsSuffix.length());
    }
    
    std::string program;
    std::string args;
    SplitCommand(command, program, args);
    
    if (program.empty()) return;
    
    char userProfile[MAX_PATH];
    if (GetEnvironmentVariableA("USERPROFILE", userProfile, sizeof(userProfile)) == 0) {
        strcpy_s(userProfile, "C:\\Users\\lhcoy");
    }
    
    const char* verb = runAsAdmin ? "runas" : "open";
    ShellExecuteA(NULL, verb, program.c_str(), args.empty() ? NULL : args.c_str(), userProfile, SW_SHOWNORMAL);
}

// Load and register all hotkeys
void RegisterAllHotkeys(HWND hWnd) {
    for (const auto& hk : g_ActiveHotkeys) {
        UnregisterHotKey(hWnd, hk.id);
    }
    g_ActiveHotkeys.clear();
    
    EnsureConfigFileExists();
    auto parsed = ParseConfig(GetConfigFilePath());
    
    int id = IDM_QUICK_START;
    for (const auto& item : parsed) {
        UINT modifiers = 0;
        UINT vk = 0;
        if (ParseShortcut(item.keys, modifiers, vk)) {
            if (RegisterHotKey(hWnd, id, modifiers, vk)) {
                g_ActiveHotkeys.push_back({ id, modifiers, vk, item.command, item.keys });
                id++;
            }
        }
    }
}

// Cleanup all hotkeys
void UnregisterAllHotkeys(HWND hWnd) {
    for (const auto& hk : g_ActiveHotkeys) {
        UnregisterHotKey(hWnd, hk.id);
    }
    g_ActiveHotkeys.clear();
}

// Generate custom modern system tray icon showing ">_" GDI vector
HICON CreateDynamicIcon(bool enabled) {
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    
    HDC hdc = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdc, cx, cy);
    
    HGDIOBJ hOld = SelectObject(hdcMem, hBitmap);
    
    COLORREF bgColor = enabled ? RGB(30, 41, 59) : RGB(100, 116, 139);
    COLORREF fgColor = RGB(34, 197, 94);
    COLORREF fgColor2 = RGB(255, 255, 255);
    
    RECT rc = { 0, 0, cx, cy };
    FillRect(hdcMem, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    
    HBRUSH hBgBrush = CreateSolidBrush(bgColor);
    HGDIOBJ hOldBrush = SelectObject(hdcMem, hBgBrush);
    HPEN hBgPen = CreatePen(PS_SOLID, 1, bgColor);
    HGDIOBJ hOldPen = SelectObject(hdcMem, hBgPen);
    
    Ellipse(hdcMem, 0, 0, cx, cy);
    
    HPEN hPromptPen = CreatePen(PS_SOLID, 2, fgColor2);
    SelectObject(hdcMem, hPromptPen);
    
    int x1 = (int)(cx * 0.25);
    int x2 = (int)(cx * 0.45);
    int y1 = (int)(cy * 0.3);
    int y2 = (int)(cy * 0.5);
    int y3 = (int)(cy * 0.7);
    
    MoveToEx(hdcMem, x1, y1, NULL);
    LineTo(hdcMem, x2, y2);
    LineTo(hdcMem, x1, y3);
    
    HPEN hCursorPen = CreatePen(PS_SOLID, 2, fgColor);
    SelectObject(hdcMem, hCursorPen);
    int cx1 = (int)(cx * 0.55);
    int cx2 = (int)(cx * 0.75);
    int cyBottom = (int)(cy * 0.7);
    MoveToEx(hdcMem, cx1, cyBottom, NULL);
    LineTo(hdcMem, cx2, cyBottom);
    
    SelectObject(hdcMem, hOldPen);
    DeleteObject(hCursorPen);
    DeleteObject(hPromptPen);
    DeleteObject(hBgPen);
    SelectObject(hdcMem, hOldBrush);
    DeleteObject(hBgBrush);
    SelectObject(hdcMem, hOld);
    
    HBITMAP hMonoBitmap = CreateBitmap(cx, cy, 1, 1, NULL);
    HDC hdcMask = CreateCompatibleDC(hdc);
    HGDIOBJ hOldMask = SelectObject(hdcMask, hMonoBitmap);
    
    FillRect(hdcMask, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    HBRUSH hBlackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
    SelectObject(hdcMask, hBlackBrush);
    HPEN hBlackPen = (HPEN)GetStockObject(BLACK_PEN);
    SelectObject(hdcMask, hBlackPen);
    
    Ellipse(hdcMask, 0, 0, cx, cy);
    
    SelectObject(hdcMask, hOldMask);
    DeleteDC(hdcMask);
    
    ICONINFO ii = { 0 };
    ii.fIcon = TRUE;
    ii.hbmMask = hMonoBitmap;
    ii.hbmColor = hBitmap;
    
    HICON hIcon = CreateIconIndirect(&ii);
    
    DeleteObject(hBitmap);
    DeleteObject(hMonoBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdc);
    
    return hIcon;
}

// Show helper dialog for shortcuts info
void ShowInstructionsBox(HWND hWnd) {
    std::string msg = "Terminal Launcher is running in the background!\n\nShortcuts:\n";
    for (const auto& hk : g_ActiveHotkeys) {
        msg += "• " + hk.shortcutStr + ": " + hk.command + "\n";
    }
    msg += "\nYou can edit these in Settings (config.json) and then click 'Reload Config' in the system tray menu.";
    
    MessageBoxA(hWnd, msg.c_str(), "Terminal Launcher Instructions", MB_OK | MB_ICONINFORMATION);
}

// Registry Run-at-Startup checks
bool IsRunAtStartupEnabled() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type;
        char value[MAX_PATH];
        DWORD size = sizeof(value);
        LONG res = RegQueryValueExA(hKey, "TerminalLauncher", NULL, &type, (LPBYTE)value, &size);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS);
    }
    return false;
}

void SetRunAtStartup(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            char path[MAX_PATH];
            GetModuleFileNameA(NULL, path, MAX_PATH);
            std::string runCmd = std::string("\"") + path + "\" --startup";
            RegSetValueExA(hKey, "TerminalLauncher", 0, REG_SZ, (const BYTE*)runCmd.c_str(), runCmd.length() + 1);
        } else {
            RegDeleteValueA(hKey, "TerminalLauncher");
        }
        RegCloseKey(hKey);
    }
}

bool HasStartupFlag() {
    int numArgs = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &numArgs);
    if (argv) {
        for (int i = 1; i < numArgs; ++i) {
            if (wcscmp(argv[i], L"--startup") == 0 || wcscmp(argv[i], L"-s") == 0) {
                LocalFree(argv);
                return true;
            }
        }
        LocalFree(argv);
    }
    return false;
}

// Window Procedure
LRESULT CALLBACK HelperWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            RegisterAllHotkeys(hWnd);
            
            g_NID.cbSize = sizeof(NOTIFYICONDATAA);
            g_NID.hWnd = hWnd;
            g_NID.uID = IDI_TRAY_ICON;
            g_NID.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            g_NID.uCallbackMessage = WM_TRAYICON;
            g_hTrayIcon = CreateDynamicIcon(true);
            g_NID.hIcon = g_hTrayIcon;
            strcpy_s(g_NID.szTip, "Terminal Launcher");
            
            Shell_NotifyIconA(NIM_ADD, &g_NID);
            break;
        }
        case WM_DESTROY: {
            UnregisterAllHotkeys(hWnd);
            Shell_NotifyIconA(NIM_DELETE, &g_NID);
            if (g_hTrayIcon) {
                DestroyIcon(g_hTrayIcon);
            }
            PostQuitMessage(0);
            break;
        }
        case WM_HOTKEY: {
            int id = (int)wParam;
            for (const auto& hk : g_ActiveHotkeys) {
                if (hk.id == id) {
                    LaunchCommand(hk.command);
                    break;
                }
            }
            break;
        }
        case WM_TRAYICON: {
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
                POINT curPoint;
                GetCursorPos(&curPoint);
                
                HMENU hMenu = CreatePopupMenu();
                if (hMenu) {
                    AppendMenuA(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_HEADER, "Terminal Launcher v1.0");
                    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                    
                    HMENU hSubMenu = CreatePopupMenu();
                    if (hSubMenu) {
                        for (size_t i = 0; i < g_ActiveHotkeys.size(); ++i) {
                            std::string label = g_ActiveHotkeys[i].command + " (" + g_ActiveHotkeys[i].shortcutStr + ")";
                            AppendMenuA(hSubMenu, MF_STRING, IDM_QUICK_START + i, label.c_str());
                        }
                        AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hSubMenu, "Quick Launch");
                    }
                    
                    UINT startupFlags = MF_STRING;
                    if (IsRunAtStartupEnabled()) startupFlags |= MF_CHECKED;
                    AppendMenuA(hMenu, startupFlags, IDM_STARTUP, "Run at Startup");
                    
                    AppendMenuA(hMenu, MF_STRING, IDM_ABOUT, "Instructions...");
                    AppendMenuA(hMenu, MF_STRING, IDM_SETTINGS, "Settings...");
                    AppendMenuA(hMenu, MF_STRING, IDM_RELOAD, "Reload Config");
                    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                    AppendMenuA(hMenu, MF_STRING, IDM_EXIT, "Exit");
                    
                    SetForegroundWindow(hWnd);
                    TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, curPoint.x, curPoint.y, 0, hWnd, NULL);
                    DestroyMenu(hMenu);
                }
            }
            break;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId >= IDM_QUICK_START && wmId < IDM_QUICK_START + (int)g_ActiveHotkeys.size()) {
                size_t idx = wmId - IDM_QUICK_START;
                LaunchCommand(g_ActiveHotkeys[idx].command);
            }
            else {
                switch (wmId) {
                    case IDM_STARTUP: {
                        bool startup = IsRunAtStartupEnabled();
                        SetRunAtStartup(!startup);
                        break;
                    }
                    case IDM_ABOUT: {
                        ShowInstructionsBox(hWnd);
                        break;
                    }
                    case IDM_SETTINGS: {
                        ShellExecuteA(NULL, "open", GetConfigFilePath().c_str(), NULL, NULL, SW_SHOWNORMAL);
                        break;
                    }
                    case IDM_RELOAD: {
                        RegisterAllHotkeys(hWnd);
                        MessageBoxA(hWnd, "Configuration reloaded successfully!", "Terminal Launcher", MB_OK | MB_ICONINFORMATION);
                        break;
                    }
                    case IDM_EXIT: {
                        DestroyWindow(hWnd);
                        break;
                    }
                }
            }
            break;
        }
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Entry Point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    HANDLE hMutex = CreateMutexA(NULL, TRUE, "Global\\TerminalLauncherMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxA(NULL, "Terminal Launcher is already running in the system tray.", "Terminal Launcher", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    WNDCLASSEXA wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = HelperWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "TerminalLauncherHelperClass";
    
    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "Failed to register helper window class.", "Error", MB_OK | MB_ICONERROR);
        CloseHandle(hMutex);
        return 1;
    }

    g_hHelperWnd = CreateWindowExA(0, "TerminalLauncherHelperClass", "Terminal Launcher Helper", 
                                  0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
    
    if (!g_hHelperWnd) {
        MessageBoxA(NULL, "Failed to create helper window.", "Error", MB_OK | MB_ICONERROR);
        CloseHandle(hMutex);
        return 1;
    }

    if (!HasStartupFlag()) {
        ShowInstructionsBox(g_hHelperWnd);
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CloseHandle(hMutex);
    return (int)msg.wParam;
}
