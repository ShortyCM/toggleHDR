// Standalone regression tests; all state queries and profile writes used below
// are replaced with fakes. These tests never change a real display or profile.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../toggleHDR/NativeHdr.h"
#include <cassert>
#include <iostream>
#include <stdexcept>

LONG WINAPI FakeGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_HEADER* header);
LONG WINAPI FakeGetDisplayConfigBufferSizes(UINT32, UINT32*, UINT32*);
LONG WINAPI FakeQueryDisplayConfig(UINT32, UINT32*, DISPLAYCONFIG_PATH_INFO*,
                                  UINT32*, DISPLAYCONFIG_MODE_INFO*, DISPLAYCONFIG_TOPOLOGY_ID*);
BOOL WINAPI FakeEnumDisplayDevicesW(LPCWSTR, DWORD, PDISPLAY_DEVICEW, DWORD);
#define DisplayConfigGetDeviceInfo FakeGetDeviceInfo
#define GetDisplayConfigBufferSizes FakeGetDisplayConfigBufferSizes
#define QueryDisplayConfig FakeQueryDisplayConfig
#define EnumDisplayDevicesW FakeEnumDisplayDevicesW
#include "../toggleHDR/Display.cpp"
#undef DisplayConfigGetDeviceInfo
#undef GetDisplayConfigBufferSizes
#undef QueryDisplayConfig
#undef EnumDisplayDevicesW

namespace {
LONG modernResult = ERROR_SUCCESS;
UINT32 modernFlags = 0;
UINT32 modernMode = 0;
UINT32 legacyFlags = 0;
unsigned legacyReads = 0;
std::vector<std::wstring> selectedProfiles;
HRESULT setResult = S_OK;
bool enumerationFixture = false;

HRESULT WINAPI FakeSetProfile(int, PCWSTR name, int type, int subtype, LUID, UINT32) {
    assert(type == 0);
    assert(subtype == 7); // No test path may write an HDR profile association.
    selectedProfiles.emplace_back(name);
    return setResult;
}

template<class Function> bool Throws(Function function) {
    try { function(); } catch (const std::runtime_error&) { return true; }
    return false;
}
} // namespace

LONG WINAPI FakeGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_HEADER* header) {
    if (enumerationFixture) {
        if (header->type == DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME) {
            auto* source = reinterpret_cast<DISPLAYCONFIG_SOURCE_DEVICE_NAME*>(header);
            wcscpy_s(source->viewGdiDeviceName,
                     header->id == 0 ? L"\\\\.\\DISPLAY1" : L"\\\\.\\DISPLAY2");
            return ERROR_SUCCESS;
        }
        if (header->type == DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME) {
            auto* target = reinterpret_cast<DISPLAYCONFIG_TARGET_DEVICE_NAME*>(header);
            wcscpy_s(target->monitorFriendlyDeviceName,
                     header->id == 10 ? L"Unreadable Monitor" : L"Working HDR Monitor");
            return ERROR_SUCCESS;
        }
        if (header->type == static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15)) {
            if (header->id == 10) return ERROR_ACCESS_DENIED;
            auto* info = reinterpret_cast<hdr::ColorInfo2*>(header);
            info->flags = hdr::kHdrSupported | hdr::kHdrUserEnabled;
            info->activeMode = 2;
            return ERROR_SUCCESS;
        }
        throw std::runtime_error("Unexpected device query in the display enumeration fixture.");
    }
    if (header->type == static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15)) {
        assert(header->size == 36);
        if (modernResult != ERROR_SUCCESS) return modernResult;
        auto* info = reinterpret_cast<hdr::ColorInfo2*>(header);
        info->flags = modernFlags;
        info->activeMode = modernMode;
        return ERROR_SUCCESS;
    }
    if (header->type == static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(9)) {
        ++legacyReads;
        assert(header->size == 32);
        reinterpret_cast<hdr::ColorInfo*>(header)->flags = legacyFlags;
        return ERROR_SUCCESS;
    }
    throw std::runtime_error("Unexpected native display query in a fixture test.");
}

LONG WINAPI FakeGetDisplayConfigBufferSizes(UINT32 flags, UINT32* paths, UINT32* modes) {
    assert(enumerationFixture && flags == QDC_ONLY_ACTIVE_PATHS);
    *paths = 2;
    *modes = 0;
    return ERROR_SUCCESS;
}

LONG WINAPI FakeQueryDisplayConfig(UINT32 flags, UINT32* pathCount,
                                  DISPLAYCONFIG_PATH_INFO* paths, UINT32* modeCount,
                                  DISPLAYCONFIG_MODE_INFO*, DISPLAYCONFIG_TOPOLOGY_ID*) {
    assert(enumerationFixture && flags == QDC_ONLY_ACTIVE_PATHS);
    assert(*pathCount >= 2);
    *pathCount = 2;
    *modeCount = 0;
    for (UINT32 index = 0; index < 2; ++index) {
        paths[index] = {};
        paths[index].sourceInfo.adapterId.LowPart = 1;
        paths[index].sourceInfo.id = index;
        paths[index].targetInfo.adapterId.LowPart = 1;
        paths[index].targetInfo.id = index == 0 ? 10 : 20;
        paths[index].flags = DISPLAYCONFIG_PATH_ACTIVE;
    }
    return ERROR_SUCCESS;
}

BOOL WINAPI FakeEnumDisplayDevicesW(LPCWSTR deviceName, DWORD index,
                                   PDISPLAY_DEVICEW display, DWORD) {
    assert(enumerationFixture);
    if (deviceName || index > 0) return FALSE;
    wcscpy_s(display->DeviceName, L"\\\\.\\DISPLAY1");
    display->StateFlags = DISPLAY_DEVICE_PRIMARY_DEVICE;
    return TRUE;
}

int main() {
    const hdr::Display display{};

    // Regression: Advanced Color can be enabled while the display remains SDR.
    modernFlags = hdr::kAdvancedSupported | hdr::kAdvancedEnabled | hdr::kHdrSupported;
    modernMode = 1;
    auto state = hdr::ReadState(display);
    assert(state.modern && state.supported && state.mode == hdr::Mode::SDR && !state.requested);

    modernMode = 2;
    modernFlags |= hdr::kHdrUserEnabled;
    state = hdr::ReadState(display);
    assert(state.mode == hdr::Mode::HDR && state.requested);

    // A pending user request is separate from the actual active mode.
    modernMode = 1;
    state = hdr::ReadState(display);
    assert(state.mode == hdr::Mode::SDR && state.requested);
    modernMode = 99;
    assert(hdr::ReadState(display).mode == hdr::Mode::Unknown);

    modernResult = ERROR_ACCESS_DENIED;
    legacyReads = 0;
    assert(Throws([&] { hdr::ReadState(display); }));
    assert(legacyReads == 0); // Real failures must not silently use the old API.

    modernResult = ERROR_NOT_SUPPORTED;
    legacyFlags = hdr::kAdvancedSupported | hdr::kAdvancedEnabled;
    state = hdr::ReadState(display);
    assert(!state.modern && state.supported && state.mode == hdr::Mode::HDR);
    legacyFlags |= hdr::kWideColorEnforced;
    state = hdr::ReadState(display);
    assert(!state.supported && state.mode == hdr::Mode::SDR);
    legacyReads = 0;
    assert(Throws([&] { hdr::ReadState(display, true); }));
    assert(legacyReads == 0); // SDR profile changes require unambiguous modern state.

    // An unreadable primary display must not hide a working secondary HDR display.
    enumerationFixture = true;
    const auto displays = hdr::EnumerateDisplays();
    assert(displays.size() == 2);
    assert(displays[0].primary && displays[0].mode == hdr::Mode::Unknown);
    assert(!displays[0].hdrSupported);
    assert(!displays[1].primary && displays[1].mode == hdr::Mode::HDR);
    assert(displays[1].hdrSupported);
    assert(hdr::SelectDisplay(L"").targetId == 20);
    assert(hdr::SelectDisplay(L"\\\\.\\DISPLAY2").targetId == 20);
    assert(Throws([] { hdr::SelectDisplay(L"Unreadable Monitor"); }));
    enumerationFixture = false;

    const std::vector<std::wstring> associated{L"Display_sRGB.icm", L"Display_P3.icm"};
    assert(hdr::FindAssociated(associated, L"display_SRGB.ICM", "missing") == associated[0]);
    assert(Throws([&] { hdr::FindAssociated(associated, L"Other.icm", "missing"); }));
    assert(hdr::SameProfile(L"C:\\Windows\\System32\\spool\\drivers\\color\\Display_sRGB.icm",
                            L"display_SRGB.ICM"));

    hdr::Profiles profiles;
    profiles.setDefault = FakeSetProfile;
    const std::wstring desired = L"Display_sRGB.icm";
    selectedProfiles.clear();
    try {
        hdr::RestoreProfile rollback(profiles, display, 1, desired);
        throw std::runtime_error("Simulated failure after selecting a temporary profile");
    } catch (const std::runtime_error&) {}
    assert(selectedProfiles == std::vector<std::wstring>{desired});

    selectedProfiles.clear();
    {
        hdr::RestoreProfile rollback(profiles, display, 1, desired);
        assert(SUCCEEDED(rollback.Restore()));
    }
    assert(selectedProfiles.size() == 1); // Successful restore disarms the guard.

    selectedProfiles.clear();
    setResult = E_FAIL;
    {
        hdr::RestoreProfile rollback(profiles, display, 1, desired);
        assert(FAILED(rollback.Restore()));
        setResult = S_OK;
    }
    assert(selectedProfiles.size() == 2); // Failed explicit restore gets a final retry.
    std::cout << "Display state and SDR profile rollback regression tests passed.\n";
}
