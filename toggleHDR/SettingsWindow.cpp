/*
    toggleHDR - native Settings refresh support
    Copyright (C) 2025 Clayton Macleod

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "NativeHdr.h"
#include <shellapi.h>
#include <array>
#include <cwchar>
#include <stdexcept>
#include <string>

#ifdef _MSC_VER
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Shell32.lib")
#endif

namespace hdr {
namespace {

class ProcessHandle {
public:
    explicit ProcessHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~ProcessHandle() { if (handle_) CloseHandle(handle_); }
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }
private:
    HANDLE handle_;
};

bool ProcessHasName(HANDLE process, const wchar_t* expectedName) noexcept {
    if (!process) return false;
    std::array<wchar_t, 32768> path{};
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) return false;
    const wchar_t* basename = wcsrchr(path.data(), L'\\');
    basename = basename ? basename + 1 : path.data();
    return _wcsicmp(basename, expectedName) == 0;
}

bool ProcessHasName(DWORD processId, const wchar_t* expectedName) noexcept {
    if (!processId) return false;
    ProcessHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    return ProcessHasName(process.get(), expectedName);
}

DWORD WindowProcess(HWND window) noexcept {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId;
}

struct SettingsChildSearch { DWORD processId = 0; };

BOOL CALLBACK FindSettingsChild(HWND child, LPARAM context) {
    auto& result = *reinterpret_cast<SettingsChildSearch*>(context);
    const DWORD processId = WindowProcess(child);
    if (ProcessHasName(processId, L"SystemSettings.exe")) {
        result.processId = processId;
        return FALSE;
    }
    return TRUE;
}

// On some Windows versions Settings owns its top-level window. On others,
// ApplicationFrameHost owns the frame and SystemSettings owns a descendant.
DWORD SettingsProcessForWindow(HWND window) noexcept {
    const DWORD owner = WindowProcess(window);
    if (ProcessHasName(owner, L"SystemSettings.exe")) return owner;
    if (!ProcessHasName(owner, L"ApplicationFrameHost.exe")) return 0;

    SettingsChildSearch search;
    EnumChildWindows(window, FindSettingsChild, reinterpret_cast<LPARAM>(&search));
    return search.processId;
}

BOOL CALLBACK FindSettingsTopLevel(HWND window, LPARAM context) {
    if (!IsWindowVisible(window)) return TRUE;
    const DWORD settingsPid = SettingsProcessForWindow(window);
    if (!settingsPid) return TRUE;
    auto& result = *reinterpret_cast<SettingsWindow*>(context);
    result = {window, settingsPid};
    return FALSE;
}

SettingsWindow FindVisibleSettings() noexcept {
    SettingsWindow result;
    const HWND foreground = GetForegroundWindow();
    if (foreground && IsWindowVisible(foreground)) {
        const DWORD settingsPid = SettingsProcessForWindow(foreground);
        if (settingsPid) return {foreground, settingsPid};
    }
    EnumWindows(FindSettingsTopLevel, reinterpret_cast<LPARAM>(&result));
    return result;
}

bool WindowHasClosed(const SettingsWindow& window) noexcept {
    return !IsWindow(window.hwnd) || !IsWindowVisible(window.hwnd);
}

bool WaitForWindowToClose(const SettingsWindow& window, DWORD timeout) noexcept {
    const ULONGLONG start = GetTickCount64();
    do {
        if (WindowHasClosed(window)) return true;
        Sleep(50);
    } while (GetTickCount64() - start < timeout);
    return WindowHasClosed(window);
}

} // namespace

SettingsWindow OpenDisplaySettings(DWORD timeoutMilliseconds) {
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    execute.lpVerb = L"open";
    execute.lpFile = L"ms-settings:display";
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        throw std::runtime_error("Could not open Windows Display Settings (error " +
                                 std::to_string(GetLastError()) + ").");
    }

    const ULONGLONG start = GetTickCount64();
    do {
        const SettingsWindow window = FindVisibleSettings();
        if (window.hwnd) return window;
        if (GetTickCount64() - start >= timeoutMilliseconds) break;
        Sleep(50);
    } while (true);
    throw std::runtime_error("Windows Display Settings did not expose a visible window before the timeout.");
}

bool CloseDisplaySettings(const SettingsWindow& window) {
    if (!window.hwnd || WindowHasClosed(window)) return true;
    if (!window.settingsPid || SettingsProcessForWindow(window.hwnd) != window.settingsPid)
        return false;

    // Keep a handle to the captured process while waiting. If it exits, do not
    // mistake a later process reusing the numeric PID for the original target.
    ProcessHandle original(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                       FALSE, window.settingsPid));
    DWORD_PTR messageResult = 0;
    SendMessageTimeoutW(window.hwnd, WM_CLOSE, 0, 0,
                        SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 1000, &messageResult);
    if (WaitForWindowToClose(window, 2000)) return true;

    // A stale host frame may outlive the Settings process. Never terminate that
    // shared frame host; it can host unrelated Windows applications as well.
    if (!original || WaitForSingleObject(original.get(), 0) != WAIT_TIMEOUT)
        return WindowHasClosed(window);
    if (!ProcessHasName(original.get(), L"SystemSettings.exe") ||
        SettingsProcessForWindow(window.hwnd) != window.settingsPid)
        return false;

    ProcessHandle target(OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                                     FALSE, window.settingsPid));
    if (!target || !ProcessHasName(target.get(), L"SystemSettings.exe") ||
        WaitForSingleObject(original.get(), 0) != WAIT_TIMEOUT ||
        SettingsProcessForWindow(window.hwnd) != window.settingsPid)
        return false;
    if (!TerminateProcess(target.get(), 0)) return WindowHasClosed(window);
    return WaitForWindowToClose(window, 2000);
}

} // namespace hdr
