#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>

namespace hdr {
enum class Mode { Unknown, SDR, HDR };
struct Display {
    LUID adapter{};
    UINT32 sourceId = 0;
    UINT32 targetId = 0;
    std::wstring name;
    std::wstring device;
    bool hdrSupported = false;
    bool modern = false;
    bool primary = false;
    Mode mode = Mode::Unknown;
};
std::vector<Display> EnumerateDisplays();
Display SelectDisplay(const std::wstring& selector);
void SetHdr(const Display& display, bool enabled);
std::wstring GetSdrProfile(const Display& display);
// Empty desired profile means no explicit SDR profile override.
// Alternate, if specified, must already be associated with this display.
void RestoreSdrProfile(const Display& display, const std::wstring& desired,
                       const std::wstring& alternate);
const wchar_t* ModeName(Mode mode);

struct SettingsWindow {
    HWND hwnd = nullptr;
    DWORD settingsPid = 0;
};
SettingsWindow OpenDisplaySettings(DWORD timeoutMilliseconds = 5000);
bool CloseDisplaySettings(const SettingsWindow& window);
}
