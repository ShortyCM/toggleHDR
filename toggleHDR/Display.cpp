#include "NativeHdr.h"
#include <icm.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace hdr {
namespace {

// Private declarations also build with SDKs that predate the Windows 11 HDR API.
// Do not test typedefs or enum constants with #if defined(...).
constexpr auto kGetColorInfo = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(9);
constexpr auto kSetAdvancedColor = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(10);
constexpr auto kGetColorInfo2 = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15);
constexpr auto kSetHdr = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(16);
struct ColorInfo {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 flags;
    UINT32 encoding;
    UINT32 bitsPerChannel;
};
struct ColorInfo2 {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 flags;
    UINT32 encoding;
    UINT32 bitsPerChannel;
    UINT32 activeMode;
};
struct ColorState {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 enabled;
};
static_assert(sizeof(DISPLAYCONFIG_DEVICE_INFO_HEADER) == 20);
static_assert(sizeof(ColorInfo) == 32);
static_assert(sizeof(ColorInfo2) == 36);
static_assert(offsetof(ColorInfo2, activeMode) == 32);
static_assert(sizeof(ColorState) == 24);

constexpr UINT32 kAdvancedSupported = 1u << 0;
constexpr UINT32 kAdvancedEnabled = 1u << 1;
constexpr UINT32 kWideColorEnforced = 1u << 2;
constexpr UINT32 kLimitedByPolicy = 1u << 3;
constexpr UINT32 kHdrSupported = 1u << 4;
constexpr UINT32 kHdrUserEnabled = 1u << 5;
constexpr int kIccProfile = 0;
constexpr int kStandardDisplayProfile = 7;
constexpr DWORD kStandardDisplayColor = 0x00020000; // ET_STANDARDDISPLAYCOLOR, newer SDKs.

std::runtime_error WinError(const char* operation, DWORD error) {
    return std::runtime_error(std::string(operation) + " (Windows error " +
                              std::to_string(error) + ").");
}

std::runtime_error ProfileError(const char* operation, HRESULT error) {
    std::ostringstream message;
    message << operation << " (HRESULT 0x" << std::hex
            << static_cast<unsigned long>(error) << ").";
    return std::runtime_error(message.str());
}

bool Equal(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool SameAdapter(const LUID& left, const LUID& right) {
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

DWORD WindowsBuild() {
    static const DWORD build = [] {
        using RtlGetVersionFn = LONG (WINAPI*)(OSVERSIONINFOW*);
        const auto module = GetModuleHandleW(L"ntdll.dll");
        const auto getVersion = module ? reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(module, "RtlGetVersion")) : nullptr;
        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        return getVersion && getVersion(&version) == 0 ? version.dwBuildNumber : 0;
    }();
    return build;
}

bool UnsupportedModernApi(LONG error) {
    if (error == ERROR_NOT_SUPPORTED || error == ERROR_CALL_NOT_IMPLEMENTED)
        return true;
    // Older Windows rejects an unknown request type with INVALID_PARAMETER.
    // On a modern OS the same error can indicate a stale display identifier.
    const DWORD build = WindowsBuild();
    return error == ERROR_INVALID_PARAMETER && build != 0 && build < 26100;
}

DISPLAYCONFIG_DEVICE_INFO_HEADER Header(DISPLAYCONFIG_DEVICE_INFO_TYPE type,
                                       UINT32 size, const Display& display) {
    return {type, size, display.adapter, display.targetId};
}

struct State {
    bool modern = false;
    bool supported = false;
    bool requested = false;
    bool limited = false;
    Mode mode = Mode::Unknown;
};

State ReadState(const Display& display, bool requireModern = false) {
    ColorInfo2 modern{};
    modern.header = Header(kGetColorInfo2, sizeof(modern), display);
    const LONG modernError = DisplayConfigGetDeviceInfo(&modern.header);
    if (modernError == ERROR_SUCCESS) {
        State state;
        state.modern = true;
        state.supported = (modern.flags & kHdrSupported) != 0;
        state.requested = (modern.flags & kHdrUserEnabled) != 0;
        state.limited = (modern.flags & kLimitedByPolicy) != 0;
        if (modern.activeMode == 2)
            state.mode = Mode::HDR;
        else if (modern.activeMode == 0 || modern.activeMode == 1)
            state.mode = Mode::SDR; // WCG / automatic color management is SDR.
        return state;
    }
    if (requireModern || !UnsupportedModernApi(modernError))
        throw WinError("Cannot read the display's current HDR state", modernError);

    ColorInfo legacy{};
    legacy.header = Header(kGetColorInfo, sizeof(legacy), display);
    const LONG error = DisplayConfigGetDeviceInfo(&legacy.header);
    if (error != ERROR_SUCCESS)
        throw WinError("Cannot read the display's legacy HDR state", error);
    State state;
    // wideColorEnforced denotes an SDR-only advanced-color display in the old API.
    state.supported = (legacy.flags & kAdvancedSupported) != 0 &&
                      (legacy.flags & kWideColorEnforced) == 0;
    state.requested = state.supported && (legacy.flags & kAdvancedEnabled) != 0;
    state.limited = (legacy.flags & kLimitedByPolicy) != 0;
    state.mode = state.requested ? Mode::HDR : Mode::SDR;
    return state;
}

struct Paths {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

Paths ActivePaths() {
    for (int attempt = 0; attempt < 5; ++attempt) {
        UINT32 pathCount = 0, modeCount = 0;
        LONG error = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (error != ERROR_SUCCESS)
            throw WinError("Cannot enumerate displays", error);
        if (pathCount == 0)
            return {};
        Paths config;
        config.paths.resize(pathCount);
        config.modes.resize(modeCount);
        error = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, config.paths.data(),
                                   &modeCount, config.modes.data(), nullptr);
        if (error == ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (error != ERROR_SUCCESS)
            throw WinError("Cannot read the display configuration", error);
        config.paths.resize(pathCount);
        config.modes.resize(modeCount);
        return config;
    }
    throw std::runtime_error("The display configuration kept changing; try again.");
}

Display CurrentDisplay(const Display& expected) {
    for (const auto& current : EnumerateDisplays()) {
        if (SameAdapter(current.adapter, expected.adapter) && current.targetId == expected.targetId) {
            if (!expected.name.empty() && !Equal(current.name, expected.name))
                throw std::runtime_error("The selected display has changed; select it again.");
            return current;
        }
    }
    throw std::runtime_error("The selected display is no longer connected.");
}

// Load modern profile exports at runtime so the ordinary HDR toggle can still
// run on Windows versions where those exports are unavailable.
class Profiles {
public:
    using GetScopeFn = HRESULT (WINAPI*)(LUID, UINT32, int*);
    using GetDefaultFn = HRESULT (WINAPI*)(int, LUID, UINT32, int, int, LPWSTR*);
    using EnumProfilesFn = BOOL (WINAPI*)(PCWSTR, PENUMTYPEW, PBYTE, PDWORD, PDWORD);
    using SetDefaultFn = HRESULT (WINAPI*)(int, PCWSTR, int, int, LUID, UINT32);
    GetScopeFn getScope;
    GetDefaultFn getDefault;
    EnumProfilesFn enumerate;
    SetDefaultFn setDefault;

    Profiles() {
        module_ = LoadLibraryExW(L"mscms.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_)
            throw WinError("Cannot load Windows color management", GetLastError());
        getScope = reinterpret_cast<GetScopeFn>(GetProcAddress(module_, "ColorProfileGetDisplayUserScope"));
        getDefault = reinterpret_cast<GetDefaultFn>(GetProcAddress(module_, "ColorProfileGetDisplayDefault"));
        enumerate = reinterpret_cast<EnumProfilesFn>(GetProcAddress(module_, "EnumColorProfilesW"));
        setDefault = reinterpret_cast<SetDefaultFn>(GetProcAddress(module_, "ColorProfileSetDisplayDefaultAssociation"));
        if (!getScope || !getDefault || !enumerate || !setDefault) {
            FreeLibrary(module_);
            module_ = nullptr;
            throw std::runtime_error("This Windows version does not support display profile management.");
        }
    }
    ~Profiles() { if (module_) FreeLibrary(module_); }
    Profiles(const Profiles&) = delete;
    Profiles& operator=(const Profiles&) = delete;

    int Scope(const Display& display) const {
        int scope = -1;
        const HRESULT result = getScope(display.adapter, display.sourceId, &scope);
        if (FAILED(result))
            throw ProfileError("Cannot read the display profile scope", result);
        if (scope != 0 && scope != 1)
            throw std::runtime_error("Windows returned an unknown display profile scope.");
        return scope;
    }

    std::wstring Default(const Display& display, int scope) const {
        LPWSTR name = nullptr;
        const HRESULT result = getDefault(scope, display.adapter, display.sourceId,
                                          kIccProfile, kStandardDisplayProfile, &name);
        const std::unique_ptr<void, decltype(&LocalFree)> memory(name, LocalFree);
        if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            return {};
        if (FAILED(result))
            throw ProfileError("Cannot read the SDR profile", result);
        return name ? name : L"";
    }

    std::vector<std::wstring> Associated(const Display& display, int scope) const {
        // ColorProfileGetDisplayList can return an empty list on current Windows
        // even when GetDisplayDefault returns a profile. EnumColorProfilesW also
        // lets us restrict validation to SDR associations, excluding HDR-only ones.
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header = Header(DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), display);
        const LONG targetError = DisplayConfigGetDeviceInfo(&target.header);
        if (targetError != ERROR_SUCCESS)
            throw WinError("Cannot identify the monitor's profile associations", targetError);
        std::wstring deviceKey;
        for (DWORD index = 0;; ++index) {
            DISPLAY_DEVICEW monitor{};
            monitor.cb = sizeof(monitor);
            if (!EnumDisplayDevicesW(display.device.c_str(), index, &monitor,
                                     EDD_GET_DEVICE_INTERFACE_NAME))
                break;
            if (Equal(monitor.DeviceID, target.monitorDevicePath)) {
                deviceKey = monitor.DeviceKey;
                break;
            }
        }
        if (deviceKey.empty())
            throw std::runtime_error("Cannot identify the selected monitor's color profile list.");
        if (Scope(display) != scope)
            throw std::runtime_error("The display profile scope changed; try again.");
        ENUMTYPEW filter{};
        filter.dwSize = sizeof(filter);
        filter.dwVersion = ENUM_TYPE_VERSION;
        filter.dwFields = ET_DEVICENAME | kStandardDisplayColor;
        filter.pDeviceName = deviceKey.c_str();
        filter.dwDeviceClass = CLASS_MONITOR;
        DWORD bytes = 0, count = 0;
        SetLastError(ERROR_SUCCESS);
        const BOOL measured = enumerate(nullptr, &filter, nullptr, &bytes, &count);
        const DWORD measureError = GetLastError();
        if (!measured && measureError != ERROR_INSUFFICIENT_BUFFER)
            throw WinError("Cannot measure the SDR profile list", measureError);
        if (bytes == 0) return {};
        for (int attempt = 0; attempt < 3; ++attempt) {
            std::vector<wchar_t> names((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t), L'\0');
            bytes = static_cast<DWORD>(names.size() * sizeof(wchar_t));
            if (!enumerate(nullptr, &filter, reinterpret_cast<PBYTE>(names.data()), &bytes, &count)) {
                const DWORD error = GetLastError();
                if (error == ERROR_INSUFFICIENT_BUFFER) continue;
                throw WinError("Cannot read the SDR profile list", error);
            }
            std::vector<std::wstring> profiles;
            auto name = names.cbegin();
            for (DWORD index = 0; index < count; ++index) {
                const auto end = std::find(name, names.cend(), L'\0');
                if (name == names.cend() || end == names.cend() || name == end)
                    throw std::runtime_error("Windows returned an invalid SDR profile list.");
                profiles.emplace_back(name, end);
                name = end + 1;
            }
            if (Scope(display) != scope)
                throw std::runtime_error("The display profile scope changed; try again.");
            return profiles;
        }
        throw std::runtime_error("The SDR profile list kept changing; try again.");
    }

    HRESULT Set(const Display& display, int scope, const std::wstring& name) const noexcept {
        // Profile APIs require the target adapter LUID and SOURCE ID, whereas
        // the display-state API above requires the target ID.
        return setDefault(scope, name.c_str(), kIccProfile, kStandardDisplayProfile,
                          display.adapter, display.sourceId);
    }

private:
    HMODULE module_ = nullptr;
};

std::wstring FileName(const std::wstring& name) {
    const auto slash = name.find_last_of(L"\\/");
    return slash == std::wstring::npos ? name : name.substr(slash + 1);
}

std::wstring FindAssociated(const std::vector<std::wstring>& profiles,
                            const std::wstring& requested, const char* error) {
    std::wstring found;
    for (const auto& profile : profiles) {
        if (Equal(profile, requested) || Equal(FileName(profile), FileName(requested))) {
            if (!found.empty() && !Equal(found, profile))
                throw std::runtime_error("The configured profile name is ambiguous.");
            found = profile;
        }
    }
    if (found.empty())
        throw std::runtime_error(error);
    return found;
}

bool SameProfile(const std::wstring& left, const std::wstring& right) {
    return Equal(FileName(left), FileName(right));
}

// Once a temporary profile is attempted, always attempt to restore the desired
// SDR association, including if the temporary switch or subsequent work fails.
class RestoreProfile {
public:
    RestoreProfile(const Profiles& profiles, const Display& display, int scope,
                   const std::wstring& desired)
        : profiles_(profiles), display_(display), scope_(scope), desired_(desired) {}
    ~RestoreProfile() { if (armed_) profiles_.Set(display_, scope_, desired_); }
    RestoreProfile(const RestoreProfile&) = delete;
    RestoreProfile& operator=(const RestoreProfile&) = delete;
    HRESULT Restore() noexcept {
        const HRESULT result = profiles_.Set(display_, scope_, desired_);
        // If an explicit restore fails, the destructor gets one more attempt.
        if (SUCCEEDED(result)) armed_ = false;
        return result;
    }
private:
    const Profiles& profiles_;
    const Display& display_;
    int scope_;
    const std::wstring& desired_;
    bool armed_ = true;
};

} // namespace

const wchar_t* ModeName(Mode mode) {
    switch (mode) {
    case Mode::SDR: return L"SDR";
    case Mode::HDR: return L"HDR";
    default: return L"Unknown";
    }
}

std::vector<Display> EnumerateDisplays() {
    const auto config = ActivePaths();
    std::wstring primaryDevice;
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, index, &device, 0))
            break;
        if (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            primaryDevice = device.DeviceName;
            break;
        }
    }

    std::vector<Display> displays;
    for (const auto& path : config.paths) {
        Display display;
        display.adapter = path.targetInfo.adapterId;
        display.targetId = path.targetInfo.id;
        display.sourceId = path.sourceInfo.id;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                         path.sourceInfo.adapterId, path.sourceInfo.id};
        const LONG sourceError = DisplayConfigGetDeviceInfo(&source.header);
        if (sourceError != ERROR_SUCCESS)
            throw WinError("Cannot read the display device name", sourceError);
        display.device = source.viewGdiDeviceName;
        display.primary = !primaryDevice.empty() && Equal(display.device, primaryDevice);
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header = Header(DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), display);
        const LONG targetError = DisplayConfigGetDeviceInfo(&target.header);
        if (targetError != ERROR_SUCCESS)
            throw WinError("Cannot read the monitor name", targetError);
        display.name = target.monitorFriendlyDeviceName;
        if (display.name.empty()) display.name = display.device;
        try {
            const State state = ReadState(display);
            display.hdrSupported = state.supported;
            display.modern = state.modern;
            display.mode = state.mode;
        } catch (const std::runtime_error&) {
            // A virtual, remote, or otherwise unreadable target must not prevent
            // selecting another working HDR display. Keep this target Unknown;
            // mutation paths query their selected target again and report errors.
        }
        displays.push_back(std::move(display));
    }
    return displays;
}

Display SelectDisplay(const std::wstring& selector) {
    const auto displays = EnumerateDisplays();
    if (!selector.empty()) {
        const Display* found = nullptr;
        for (const auto& display : displays) {
            if (Equal(selector, display.name) || Equal(selector, display.device)) {
                if (found)
                    throw std::runtime_error("The display name is ambiguous; select its device name instead.");
                found = &display;
            }
        }
        if (!found)
            throw std::runtime_error("The selected display is not connected.");
        if (!found->hdrSupported)
            throw std::runtime_error("The selected display does not support HDR.");
        return *found;
    }
    const Display* firstCapable = nullptr;
    for (const auto& display : displays) {
        if (display.hdrSupported) {
            if (display.primary) return display;
            if (!firstCapable) firstCapable = &display;
        }
    }
    if (firstCapable) return *firstCapable;
    throw std::runtime_error("No connected display supports HDR.");
}

void SetHdr(const Display& display, bool enabled) {
    const Display current = CurrentDisplay(display);
    const State before = ReadState(current);
    if (!before.supported)
        throw std::runtime_error("The selected display does not support HDR.");
    if (before.mode == Mode::Unknown)
        throw std::runtime_error("Windows did not report a known display color mode.");
    if (enabled && before.limited)
        throw std::runtime_error("Windows policy currently prevents enabling HDR.");
    const Mode wanted = enabled ? Mode::HDR : Mode::SDR;
    if (before.mode == wanted && (!before.modern || before.requested == enabled))
        return;

    ColorState state{};
    state.header = Header(before.modern ? kSetHdr : kSetAdvancedColor, sizeof(state), current);
    state.enabled = enabled ? 1u : 0u;
    LONG error = DisplayConfigSetDeviceInfo(&state.header);
    if (before.modern && UnsupportedModernApi(error)) {
        state.header.type = kSetAdvancedColor;
        error = DisplayConfigSetDeviceInfo(&state.header);
    }
    if (error != ERROR_SUCCESS)
        throw WinError("Cannot change HDR mode", error);

    const ULONGLONG deadline = GetTickCount64() + 5000;
    std::string lastReadError;
    do {
        try {
            // A mode switch can temporarily invalidate the path/source mapping.
            const State after = ReadState(CurrentDisplay(current));
            if (after.mode == wanted && (!after.modern || after.requested == enabled))
                return;
            lastReadError.clear();
        } catch (const std::runtime_error& failure) {
            lastReadError = failure.what();
        }
        Sleep(100);
    } while (GetTickCount64() < deadline);
    if (!lastReadError.empty())
        throw std::runtime_error("Windows accepted the HDR change, but verification failed: " + lastReadError);
    throw std::runtime_error("Windows accepted the HDR change but did not reach the requested mode.");
}

std::wstring GetSdrProfile(const Display& display) {
    const Display current = CurrentDisplay(display);
    const Profiles profiles;
    return profiles.Default(current, profiles.Scope(current));
}

void RestoreSdrProfile(const Display& display, const std::wstring& desired,
                       const std::wstring& alternate) {
    if (desired.empty()) return;
    const Display current = CurrentDisplay(display);
    const State initial = ReadState(current, true);
    // An HDR request may already be pending even before the mode becomes HDR.
    if (initial.mode == Mode::HDR || initial.requested) return;
    if (initial.mode != Mode::SDR)
        throw std::runtime_error("Cannot restore an SDR profile while the display mode is unknown.");

    const Profiles profiles;
    const int scope = profiles.Scope(current);
    const auto associated = profiles.Associated(current, scope);
    const auto wanted = FindAssociated(associated, desired,
        "The desired SDR profile is not already associated with the selected display.");
    std::wstring temporary;
    if (!alternate.empty()) {
        temporary = FindAssociated(associated, alternate,
            "The alternate SDR profile is not already associated with the selected display.");
        if (SameProfile(temporary, wanted))
            throw std::runtime_error("The alternate SDR profile must differ from the desired profile.");
    }
    const auto before = profiles.Default(current, scope);
    const State latest = ReadState(current, true);
    if (latest.mode == Mode::HDR || latest.requested) return;
    if (latest.mode != Mode::SDR)
        throw std::runtime_error("The display mode changed before restoring the SDR profile.");
    if (profiles.Scope(current) != scope)
        throw std::runtime_error("The display profile scope changed; try again.");

    HRESULT temporaryResult = S_OK;
    RestoreProfile restore(profiles, current, scope, wanted);
    if (!temporary.empty() && SameProfile(before, wanted)) {
        temporaryResult = profiles.Set(current, scope, temporary);
        if (SUCCEEDED(temporaryResult)) Sleep(200);
    }
    // If HDR is switched externally during the temporary selection, still restore
    // the SDR association: subtype 7 does not replace the HDR profile (subtype 8).
    const HRESULT result = restore.Restore();
    if (FAILED(result))
        throw ProfileError("Cannot restore the desired SDR profile", result);
    if (!SameProfile(profiles.Default(current, scope), wanted))
        throw std::runtime_error("Windows did not retain the requested SDR profile.");
    if (FAILED(temporaryResult))
        throw ProfileError("The SDR profile was selected, but its forced refresh failed", temporaryResult);
}

} // namespace hdr
