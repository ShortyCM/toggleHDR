/*
    toggleHDR
    Copyright (C) 2025  Clayton Macleod
    Native tray-mode additions, 2026.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "NativeHdr.h"
#include "resource.h"
#include <shellapi.h>
#include <objbase.h>
#include <cstdio>
#include <exception>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
constexpr wchar_t kWindowClass[] = L"ToggleHDR.NativeTray.v1";
constexpr wchar_t kTrayMutex[] = L"Local\\ToggleHDR.NativeTray.v1";
constexpr wchar_t kOperationMutex[] = L"Local\\ToggleHDR.Operation.v1";
constexpr UINT kTrayMessage = WM_APP + 1, kNativeShortcut = WM_APP + 2;
constexpr UINT kWorkDone = WM_APP + 3, kQuitMessage = WM_APP + 4;
constexpr UINT_PTR kStatusTimer = 1, kRefreshTimer = 2;
constexpr UINT kToggleMenu = 1001, kRefreshMenu = 1002, kExitMenu = 1003;

enum class Action { Toggle, On, Off, Refresh, Status, Help, Quit };
struct Options {
    bool tray = false, refreshProfiles = true;
    Action action = Action::Toggle;
    std::wstring display, sdrProfile, resetProfile;
};
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
void Output(const std::string& text, bool error = false) {
    HANDLE stream = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (!stream || stream == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            stream = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (stream != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(stream, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
                CloseHandle(stream);
            }
        }
        return;
    }
    DWORD written = 0;
    WriteFile(stream, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}
std::string Json(const std::wstring& value) {
    std::string result = "\"";
    for (unsigned char c : Utf8(value)) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 0x20) {
            char escape[7]{};
            std::snprintf(escape, sizeof escape, "\\u%04x", static_cast<unsigned>(c));
            result += escape;
        } else result += static_cast<char>(c);
    }
    return result + '"';
}
void Log(const std::string& text) noexcept {
    try {
        wchar_t localAppData[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH)) return;
        std::wstring directory = std::wstring(localAppData) + L"\\toggleHDR";
        CreateDirectoryW(directory.c_str(), nullptr);
        HANDLE file = CreateFileW((directory + L"\\last-action.log").c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        SYSTEMTIME time{}; GetLocalTime(&time);
        char stamp[64]{};
        std::snprintf(stamp, sizeof stamp, "%04u-%02u-%02u %02u:%02u:%02u\r\n",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
        std::string content = std::string(stamp) + text + "\r\n";
        DWORD written = 0;
        WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
        CloseHandle(file);
    } catch (...) {}
}
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct OperationLock {
    HANDLE handle = CreateMutexW(nullptr, FALSE, kOperationMutex);
    bool owned = false;
    OperationLock() {
        if (!handle) throw std::runtime_error("Could not create the HDR operation lock.");
        DWORD wait = WaitForSingleObject(handle, 15000);
        owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        if (!owned) { CloseHandle(handle); handle = nullptr; throw std::runtime_error("Another HDR operation is still running."); }
    }
    ~OperationLock() { if (owned) ReleaseMutex(handle); if (handle) CloseHandle(handle); }
};
Options Parse(int argc, wchar_t** argv) {
    Options options;
    bool explicitAction = false;
    auto action = [&](Action value) {
        if (explicitAction) throw std::runtime_error("Choose only one action: --on, --off, --refresh, --status, --quit, or --help.");
        options.action = value; explicitAction = true;
    };
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        auto value = [&]() -> std::wstring {
            if (++i >= argc || std::wstring(argv[i]).rfind(L"--", 0) == 0)
                throw std::runtime_error("Missing value for " + Utf8(arg));
            return argv[i];
        };
        if (arg == L"--tray") options.tray = true;
        else if (arg == L"--display") options.display = value();
        else if (arg == L"--sdr-profile") options.sdrProfile = value();
        else if (arg == L"--sdr-reset-profile") options.resetProfile = value();
        else if (arg == L"--no-icc") options.refreshProfiles = false;
        else if (arg == L"--on") action(Action::On);
        else if (arg == L"--off") action(Action::Off);
        else if (arg == L"--refresh") action(Action::Refresh);
        else if (arg == L"--status") action(Action::Status);
        else if (arg == L"--quit") action(Action::Quit);
        else if (arg == L"--help" || arg == L"-h") action(Action::Help);
        else throw std::runtime_error("Unknown option: " + Utf8(arg) + ". Use --help.");
    }
    if (options.tray && explicitAction) throw std::runtime_error("--tray cannot be combined with a one-shot action.");
    if (!options.resetProfile.empty() && options.sdrProfile.empty())
        throw std::runtime_error("--sdr-reset-profile requires --sdr-profile.");
    if (!options.refreshProfiles && !options.sdrProfile.empty())
        throw std::runtime_error("--no-icc cannot be combined with an SDR profile override.");
    return options;
}

std::string RunAction(const Options& options, Action action) {
    ComScope com;
    OperationLock lock;
    hdr::Display display = hdr::SelectDisplay(options.display);
    if (action != Action::Refresh) {
        if (display.mode == hdr::Mode::Unknown) throw std::runtime_error("The current display mode is unknown.");
        const bool enabled = action == Action::On || (action == Action::Toggle && display.mode != hdr::Mode::HDR);
        hdr::SetHdr(display, enabled);
        Sleep(2000);
    }
    hdr::SettingsWindow settings;
    std::exception_ptr failure;
    if (options.refreshProfiles) {
        try { settings = hdr::OpenDisplaySettings(); Sleep(1500); }
        catch (...) { failure = std::current_exception(); }
        try {
            display = hdr::SelectDisplay(options.display);
            if (display.mode == hdr::Mode::SDR)
                hdr::RestoreSdrProfile(display, options.sdrProfile, options.resetProfile);
        } catch (...) { failure = std::current_exception(); }
        if (!hdr::CloseDisplaySettings(settings) && !failure)
            failure = std::make_exception_ptr(std::runtime_error("The profile was refreshed, but Windows Settings did not close."));
    }
    if (failure) std::rethrow_exception(failure);
    display = hdr::SelectDisplay(options.display);
    std::string result = "Display: " + Utf8(display.name) + "; mode: " + Utf8(hdr::ModeName(display.mode));
    if (display.mode == hdr::Mode::SDR && options.refreshProfiles) {
        // Profile metadata is informational here. An unavailable newer color
        // API must not turn a successful legacy toggle into a reported failure.
        try { result += "; SDR profile: " + Utf8(hdr::GetSdrProfile(display)); }
        catch (...) { result += "; SDR profile: unavailable"; }
    }
    Log(result);
    return result;
}
void Status() {
    const auto displays = hdr::EnumerateDisplays();
    std::ostringstream out;
    out << "{\"displays\":[";
    for (size_t i = 0; i < displays.size(); ++i) {
        const auto& display = displays[i];
        if (i) out << ',';
        out << "{\"name\":" << Json(display.name) << ",\"device\":" << Json(display.device)
            << ",\"mode\":" << Json(hdr::ModeName(display.mode))
            << ",\"hdrSupported\":" << (display.hdrSupported ? "true" : "false")
            << ",\"primary\":" << (display.primary ? "true" : "false")
            << ",\"modernApi\":" << (display.modern ? "true" : "false");
        std::string profile = "null";
        try { profile = Json(hdr::GetSdrProfile(display)); } catch (...) {}
        out << ",\"sdrProfile\":" << profile << '}';
    }
    out << "]}\n";
    Output(out.str());
}

class TrayApp {
public:
    explicit TrayApp(Options options) : options_(std::move(options)) {}
    ~TrayApp() {
        if (hook_) UnhookWindowsHookEx(hook_);
        if (worker_.joinable()) worker_.join();
        if (window_) { RemoveIcon(); DestroyWindow(window_); }
        if (icon_) DestroyIcon(icon_);
        if (instance_) CloseHandle(instance_);
        active_ = nullptr;
    }
    int Run(HINSTANCE module) {
        instance_ = CreateMutexW(nullptr, TRUE, kTrayMutex);
        if (!instance_) throw std::runtime_error("Could not create the tray instance lock.");
        if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
        module_ = module;
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc; wc.hInstance = module; wc.lpszClassName = kWindowClass;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Could not register the tray window.");
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"toggleHDR native tray", WS_OVERLAPPED,
            0, 0, 0, 0, nullptr, nullptr, module, this);
        if (!window_) throw std::runtime_error("Could not create the tray window.");
        active_ = this;
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        UpdateIcon();
        hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, module_, 0);
        if (!hook_) throw std::runtime_error("Could not observe the native HDR keyboard shortcut.");
        if (!SetTimer(window_, kStatusTimer, 1000, nullptr)) throw std::runtime_error("Could not start the display status timer.");
        MSG msg{};
        BOOL result;
        while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        return result < 0 ? 1 : 0;
    }
private:
    static inline TrayApp* active_ = nullptr;
    Options options_;
    HINSTANCE module_ = nullptr;
    HANDLE instance_ = nullptr;
    HWND window_ = nullptr;
    HHOOK hook_ = nullptr;
    HICON icon_ = nullptr;
    UINT taskbarCreated_ = 0;
    bool iconAdded_ = false, busy_ = false, pendingRefresh_ = false, exitPending_ = false;
    bool bDown_ = false;
    int lastIcon_ = -1;
    std::wstring lastTip_;
    hdr::Mode mode_ = hdr::Mode::Unknown;
    std::thread worker_;
    std::mutex resultMutex_;
    std::string workError_;

    static LRESULT CALLBACK KeyboardProc(int code, WPARAM message, LPARAM data) {
        // Observe, never consume or inject. Windows/Game Bar performs the native toggle.
        // Keep the hook fast: display queries and profile operations run elsewhere.
        if (code == HC_ACTION && active_) {
            const auto* key = reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
            if (key->vkCode == 'B') {
                bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                if (down && !active_->bDown_) {
                    bool win = (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
                    bool alt = (key->flags & LLKHF_ALTDOWN) || (GetAsyncKeyState(VK_MENU) & 0x8000);
                    if (win && alt && !(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000))
                        PostMessageW(active_->window_, kNativeShortcut, 0, 0);
                }
                active_->bDown_ = down;
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        TrayApp* self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<TrayApp*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wp, lp);
        try { return self->HandleMessage(window, message, wp, lp); }
        catch (const std::exception& error) { self->Notify(Wide(error.what())); return 0; }
        catch (...) { self->Notify(L"An unexpected error occurred."); return 0; }
    }
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (taskbarCreated_ && message == taskbarCreated_) {
            iconAdded_ = false; lastIcon_ = -1; UpdateIcon(); return 0;
        }
        switch (message) {
        case WM_TIMER:
            if (wp == kStatusTimer) UpdateIcon();
            else if (wp == kRefreshTimer) { KillTimer(window_, kRefreshTimer); StartWork(Action::Refresh); }
            return 0;
        case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE:
            UpdateIcon(); return 0;
        case kNativeShortcut:
            SetTimer(window_, kRefreshTimer, 2000, nullptr); return 0;
        case kWorkDone: {
            if (worker_.joinable()) worker_.join();
            busy_ = false;
            std::string error;
            { std::lock_guard<std::mutex> guard(resultMutex_); error = workError_; }
            if (!error.empty()) Notify(Wide(error));
            UpdateIcon();
            if (exitPending_) { PostQuitMessage(0); return 0; }
            if (pendingRefresh_) { pendingRefresh_ = false; SetTimer(window_, kRefreshTimer, 500, nullptr); }
            return 0;
        }
        case kTrayMessage:
            if (LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == WM_RBUTTONUP) ShowMenu();
            else if (LOWORD(lp) == WM_LBUTTONDBLCLK) StartWork(Action::Toggle);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kToggleMenu) StartWork(Action::Toggle);
            else if (LOWORD(wp) == kRefreshMenu) StartWork(Action::Refresh);
            else if (LOWORD(wp) == kExitMenu) RequestExit();
            return 0;
        case kQuitMessage: case WM_CLOSE:
            RequestExit(); return 0;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION:
            if (wp) { RemoveIcon(); PostQuitMessage(0); }
            return 0;
        default: return DefWindowProcW(window, message, wp, lp);
        }
    }
    void RequestExit() {
        KillTimer(window_, kRefreshTimer);
        if (busy_) exitPending_ = true;
        else PostQuitMessage(0);
    }
    void StartWork(Action action) {
        if (exitPending_) return;
        if (busy_) { if (action == Action::Refresh) pendingRefresh_ = true; return; }
        busy_ = true;
        { std::lock_guard<std::mutex> guard(resultMutex_); workError_.clear(); }
        try {
            worker_ = std::thread([this, action] {
                try { RunAction(options_, action); }
                catch (const std::exception& error) {
                    Log(std::string("ERROR: ") + error.what());
                    std::lock_guard<std::mutex> guard(resultMutex_); workError_ = error.what();
                } catch (...) {
                    std::lock_guard<std::mutex> guard(resultMutex_); workError_ = "Unexpected HDR refresh failure.";
                }
                PostMessageW(window_, kWorkDone, 0, 0);
            });
        } catch (...) { busy_ = false; throw; }
    }
    void UpdateIcon() {
        std::wstring displayName = options_.display;
        try {
            auto display = hdr::SelectDisplay(options_.display);
            mode_ = display.mode; displayName = display.name;
        } catch (...) { mode_ = hdr::Mode::Unknown; }
        DWORD light = 0, size = sizeof light;
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
        int id = mode_ == hdr::Mode::HDR ? IDI_HDR : mode_ == hdr::Mode::SDR ? IDI_SDR : IDI_UNKNOWN;
        if (light) id = mode_ == hdr::Mode::HDR ? IDI_HDR_DARK : mode_ == hdr::Mode::SDR ? IDI_SDR_DARK : IDI_UNKNOWN_DARK;
        std::wstring tip = std::wstring(hdr::ModeName(mode_)) + L" - " + displayName + L"\nWin + Alt + B";
        if (iconAdded_ && id == lastIcon_ && tip == lastTip_) return;
        HICON replacement = nullptr;
        if (!icon_ || id != lastIcon_) {
            int iconSize = GetSystemMetrics(SM_CXSMICON);
            replacement = static_cast<HICON>(LoadImageW(module_, MAKEINTRESOURCEW(id), IMAGE_ICON,
                iconSize, iconSize, LR_DEFAULTCOLOR));
            if (!replacement) throw std::runtime_error("Could not load the HDR/SDR tray icon.");
        }
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof data; data.hWnd = window_; data.uID = 1;
        data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = kTrayMessage; data.hIcon = replacement ? replacement : icon_;
        wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
        if (!Shell_NotifyIconW(iconAdded_ ? NIM_MODIFY : NIM_ADD, &data)) {
            if (replacement) DestroyIcon(replacement);
            iconAdded_ = false; return;
        }
        if (!iconAdded_) { data.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &data); }
        iconAdded_ = true;
        if (replacement) { if (icon_) DestroyIcon(icon_); icon_ = replacement; }
        lastIcon_ = id; lastTip_ = tip;
    }
    void RemoveIcon() {
        if (!iconAdded_) return;
        NOTIFYICONDATAW data{}; data.cbSize = sizeof data; data.hWnd = window_; data.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &data); iconAdded_ = false;
    }
    void Notify(const std::wstring& text) {
        NOTIFYICONDATAW data{}; data.cbSize = sizeof data; data.hWnd = window_; data.uID = 1;
        data.uFlags = NIF_INFO; data.dwInfoFlags = NIIF_WARNING;
        wcsncpy_s(data.szInfoTitle, L"toggleHDR", _TRUNCATE);
        wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    void ShowMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        std::wstring label = mode_ == hdr::Mode::HDR ? L"Switch to SDR" : L"Switch to HDR";
        AppendMenuW(menu, MF_STRING | (busy_ || mode_ == hdr::Mode::Unknown ? MF_GRAYED : 0), kToggleMenu, label.c_str());
        AppendMenuW(menu, MF_STRING | (busy_ ? MF_GRAYED : 0), kRefreshMenu, L"Refresh color profile");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kExitMenu, L"Exit");
        POINT cursor{}; GetCursorPos(&cursor); SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, cursor.x, cursor.y, 0, window_, nullptr);
        PostMessageW(window_, WM_NULL, 0, 0); DestroyMenu(menu);
    }
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    Options options;
    try { options = Parse(argc, argv); }
    catch (const std::exception& error) { LocalFree(argv); Output(std::string(error.what()) + "\n", true); return 2; }
    LocalFree(argv);
    try {
        ComScope com;
        if (options.action == Action::Help) {
            Output("toggleHDR [--tray | --on | --off | --refresh | --status | --quit | --help]\n"
                   "          [--display NAME] [--no-icc]\n"
                   "          [--sdr-profile FILE] [--sdr-reset-profile FILE]\n"
                   "No action: toggle once. --tray starts the native HDR/SDR status icon.\n"
                   "Win+Alt+B keeps Windows' native toggle and refreshes the profile afterward.\n"
                   "--status prints read-only JSON. Profile overrides are optional and SDR-only.\n");
            return 0;
        }
        if (options.action == Action::Quit) {
            HWND tray = FindWindowW(kWindowClass, nullptr);
            if (tray) PostMessageW(tray, kQuitMessage, 0, 0);
            return 0;
        }
        if (options.action == Action::Status) { Status(); return 0; }
        if (options.tray) return TrayApp(std::move(options)).Run(instance);
        Output(RunAction(options, options.action) + "\n");
        return 0;
    } catch (const std::exception& error) {
        Log(std::string("ERROR: ") + error.what());
        Output(std::string(error.what()) + "\n", true);
        if (options.tray) MessageBoxW(nullptr, Wide(error.what()).c_str(), L"toggleHDR could not start", MB_OK | MB_ICONERROR);
        return 1;
    }
}
