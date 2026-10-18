// SteeringSync Universal — source reconstruction for ATS / ETS2 1.61 (Windows x64)
// Reconstructed from the functioning SteeringSync_Universal_*_1.61_FIXED.dll files.
// The original author's source code was not available. This is NOT a byte-identical
// decompilation. Tested logic/signature against user-supplied ATS/ETS2 executables;
// execution of this newly compiled code requires testing on Windows.
//
// Hooked game instructions (ATS/ETS2 1.61):
//     mov   rax, [rbx+118h]                 ; retained, NOT replaced
//     lea   rcx, [rip+relative32]          ; replaced with mov rcx, absolute target
//     movss xmm6, dword ptr [rax+6B8h]    ; replaced with raw DirectInput steering
//     call  ...                            ; resumed unchanged
//
// This affects the visible steering animation, not the game's steering physics.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <utility>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

#include "SteeringAxisMath.h"

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "user32.lib")

namespace {

// ---- Minimal SCS Telemetry SDK ABI (64-bit) ----
// Equivalent prefixes/layout from scssdk_telemetry.h / scssdk.h.
using ScsResult = std::int32_t;
using ScsLog = void (__cdecl *)(std::uint32_t type, const char *message);
using ScsChannelCallback = void (__cdecl *)(const char *name,
                                             std::uint32_t index,
                                             const void *value,
                                             void *context);
using ScsRegisterChannel = ScsResult (__cdecl *)(const char *name,
                                                   std::uint32_t index,
                                                   std::uint32_t value_type,
                                                   std::uint32_t flags,
                                                   ScsChannelCallback callback,
                                                   void *context);

struct ScsInitParams {
    const char *game_name;                 // 0x00
    const char *game_id;                   // 0x08
    std::uint32_t game_version;           // 0x10
    std::uint32_t reserved_padding;       // 0x14 (x64 alignment)
    ScsLog log;                           // 0x18
    void *register_for_event;             // 0x20
    void *unregister_from_event;          // 0x28
    ScsRegisterChannel register_for_channel; // 0x30
    void *unregister_from_channel;        // 0x38
};
static_assert(sizeof(void *) == 8, "SteeringSync only supports Win64");
static_assert(offsetof(ScsInitParams, log) == 0x18, "Wrong SCS SDK layout");
static_assert(offsetof(ScsInitParams, register_for_channel) == 0x30,
              "Wrong SCS SDK layout");

constexpr ScsResult kScsOk = 0;
constexpr ScsResult kScsUnsupported = -1;
constexpr ScsResult kScsError = -7;
constexpr std::uint32_t kChannelFloat = 5;
constexpr std::uint32_t kEachFrame = 1;
constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;

HMODULE g_ownModule = nullptr;
ScsLog g_log = nullptr;
IDirectInput8W *g_input = nullptr;
IDirectInputDevice8W *g_wheel = nullptr;

// This is deliberately a simple 32-bit float at a permanent memory address.
// The x64 trampoline reads it with MOVSS xmm6, DWORD PTR [rax].
alignas(4) volatile float g_rawSteering = 0.0f;

std::uint8_t *g_patchAddress = nullptr;
std::array<std::uint8_t, 15> g_savedInstructions{};
void *g_trampoline = nullptr;
bool g_hookActive = false;

std::wstring g_preferredDevice;
std::wstring g_axis = L"X";
bool g_invertAxis = false;

void Log(const std::string &text, std::uint32_t level = 0) {
    if (g_log) g_log(level, text.c_str());
}

std::string ToUtf8(const std::wstring &wide) {
    if (wide.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                     static_cast<int>(wide.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                        static_cast<int>(wide.size()),
                        out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring Lower(std::wstring text) {
    for (auto &c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

bool Contains(std::wstring_view text, std::wstring_view term) {
    return text.find(term) != std::wstring::npos;
}

void LoadConfig() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_ownModule, path, MAX_PATH)) return;
    std::wstring ini(path);
    auto pos = ini.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return;
    ini.resize(pos + 1);
    ini += L"SteeringSync.ini";

    wchar_t buffer[260] = {};
    GetPrivateProfileStringW(L"SteeringSync", L"DeviceNameContains", L"",
                             buffer, 260, ini.c_str());
    g_preferredDevice = Lower(buffer);
    GetPrivateProfileStringW(L"SteeringSync", L"Axis", L"X",
                             buffer, 260, ini.c_str());
    g_axis = Lower(buffer);
    g_invertAxis = GetPrivateProfileIntW(L"SteeringSync", L"Invert", 0,
                                        ini.c_str()) != 0;
}

struct WheelCandidate {
    GUID instanceGuid{};
    std::wstring name;
    int score = 0;
};

BOOL CALLBACK CollectWheel(const DIDEVICEINSTANCEW *instance, VOID *context) {
    auto *items = static_cast<std::vector<WheelCandidate> *>(context);
    WheelCandidate candidate;
    candidate.instanceGuid = instance->guidInstance;
    candidate.name = instance->tszProductName;
    const auto lower = Lower(candidate.name);

    // Ignore accessory-only DirectInput devices if distinguishable by name.
    for (const wchar_t *word : {L"handbrake", L"hand brake", L"shifter",
                               L"gear shift", L"pedal", L"button box"}) {
        if (Contains(lower, word)) return DIENUM_CONTINUE;
    }

    const unsigned type = GET_DIDEVICE_TYPE(instance->dwDevType);
    if (type == DI8DEVTYPE_DRIVING) candidate.score += 100;
    if (Contains(lower, L"wheel") || Contains(lower, L"steering"))
        candidate.score += 90;
    if (Contains(lower, L"racing") || Contains(lower, L"direct drive"))
        candidate.score += 30;

    for (const wchar_t *brand : {L"thrustmaster", L"logitech", L"fanatec",
                                L"moza", L"simagic", L"simucube", L"vrs",
                                L"cammus", L"asetek", L"accuforce", L"conspit"}) {
        if (Contains(lower, brand)) { candidate.score += 25; break; }
    }
    if (!g_preferredDevice.empty() && Contains(lower, g_preferredDevice))
        candidate.score += 1000;
    items->push_back(std::move(candidate));
    return DIENUM_CONTINUE;
}

DWORD AxisOffset() {
    if (g_axis == L"y") return DIJOFS_Y;
    if (g_axis == L"z") return DIJOFS_Z;
    if (g_axis == L"rx") return DIJOFS_RX;
    if (g_axis == L"ry") return DIJOFS_RY;
    if (g_axis == L"rz") return DIJOFS_RZ;
    if (g_axis == L"slider0") return DIJOFS_SLIDER(0);
    if (g_axis == L"slider1") return DIJOFS_SLIDER(1);
    return DIJOFS_X;
}

LONG ReadAxisValue(const DIJOYSTATE2 &state) {
    if (g_axis == L"y") return state.lY;
    if (g_axis == L"z") return state.lZ;
    if (g_axis == L"rx") return state.lRx;
    if (g_axis == L"ry") return state.lRy;
    if (g_axis == L"rz") return state.lRz;
    if (g_axis == L"slider0") return state.rglSlider[0];
    if (g_axis == L"slider1") return state.rglSlider[1];
    return state.lX;
}

void CloseWheel() {
    if (g_wheel) {
        g_wheel->Unacquire();
        g_wheel->Release();
        g_wheel = nullptr;
    }
    if (g_input) {
        g_input->Release();
        g_input = nullptr;
    }
}

bool OpenWheel() {
    HRESULT hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                    IID_IDirectInput8W,
                                    reinterpret_cast<void **>(&g_input), nullptr);
    if (FAILED(hr) || !g_input) {
        Log("[SteeringSync SOURCE] DirectInput8Create failed", 2);
        return false;
    }
    std::vector<WheelCandidate> devices;
    hr = g_input->EnumDevices(DI8DEVCLASS_GAMECTRL, CollectWheel, &devices,
                              DIEDFL_ATTACHEDONLY);
    if (FAILED(hr) || devices.empty()) {
        Log("[SteeringSync SOURCE] No DirectInput game controllers found", 2);
        CloseWheel();
        return false;
    }
    std::stable_sort(devices.begin(), devices.end(), [](const auto &a, const auto &b) {
        return a.score > b.score;
    });

    HWND window = GetForegroundWindow();
    if (!window) window = GetDesktopWindow();
    for (const auto &candidate : devices) {
        // A user-specified name must match; don't silently pick another device.
        if (!g_preferredDevice.empty() &&
            !Contains(Lower(candidate.name), g_preferredDevice)) continue;

        IDirectInputDevice8W *device = nullptr;
        if (FAILED(g_input->CreateDevice(candidate.instanceGuid, &device, nullptr)))
            continue;
        if (FAILED(device->SetDataFormat(&c_dfDIJoystick2))) {
            device->Release();
            continue;
        }
        if (FAILED(device->SetCooperativeLevel(window,
                         DISCL_NONEXCLUSIVE | DISCL_BACKGROUND))) {
            device->Release();
            continue;
        }
        DIDEVICEOBJECTINSTANCEW object{};
        object.dwSize = sizeof(object);
        const DWORD axis = AxisOffset();
        if (FAILED(device->GetObjectInfo(&object, axis, DIPH_BYOFFSET))) {
            device->Release();
            continue;
        }
        DIPROPRANGE range{};
        range.diph.dwSize = sizeof(range);
        range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        range.diph.dwObj = axis;
        range.diph.dwHow = DIPH_BYOFFSET;
        range.lMin = 0;
        range.lMax = 65535;
        if (FAILED(device->SetProperty(DIPROP_RANGE, &range.diph))) {
            device->Release();
            continue;
        }
        device->Acquire();
        g_wheel = device;
        Log("[SteeringSync SOURCE] DirectInput wheel selected: " +
            ToUtf8(candidate.name) + " (score=" +
            std::to_string(candidate.score) + ")");
        return true;
    }
    Log("[SteeringSync SOURCE] Failed to open a steering axis; check SteeringSync.ini", 2);
    CloseWheel();
    return false;
}

bool PollWheel() {
    if (!g_wheel) return false;
    HRESULT hr = g_wheel->Poll();
    if (FAILED(hr)) {
        g_wheel->Acquire();
        hr = g_wheel->Poll();
    }
    DIJOYSTATE2 state{};
    hr = g_wheel->GetDeviceState(sizeof(state), &state);
    if (FAILED(hr)) {
        g_wheel->Acquire();
        if (FAILED(g_wheel->GetDeviceState(sizeof(state), &state))) return false;
    }
    // The working DLL NEGATES normalized DirectInput before feeding steering_w.
    // This default sign is required for the visual wheel to turn in the
    // same direction as the physical wheel on ATS and ETS2 1.61.
    // Invert=1 is an OPTIONAL override for hardware with a reversed raw axis.
    g_rawSteering = steering_sync::NormalizeVisualSteering(
        ReadAxisValue(state), g_invertAxis);
    return true;
}

// ---- 1.61 signature and visual-only x64 trampoline ----

bool SignatureMatches(const std::uint8_t *p) {
    static constexpr std::uint8_t start[] = {
        0x48,0x8B,0x83,0x18,0x01,0x00,0x00,0x48,0x8D,0x0D
    };
    static constexpr std::uint8_t middle[] = {
        0xF3,0x0F,0x10,0xB0,0xB8,0x06,0x00,0x00,0xE8
    };
    static constexpr std::uint8_t end[] = {0xF3,0x0F,0x10,0x0D};
    // +10..+13, +23..+26 and +31..+34 are relative displacements.
    return std::memcmp(p, start, sizeof(start)) == 0 &&
           std::memcmp(p + 14, middle, sizeof(middle)) == 0 &&
           std::memcmp(p + 27, end, sizeof(end)) == 0;
}

std::uint8_t *FindSteeringSignature() {
    auto *image = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
    if (!image) return nullptr;
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;

    std::uint8_t *found = nullptr;
    int count = 0;
    const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(section[i].Name, ".text", 5) != 0) continue;
        const auto *text = image + section[i].VirtualAddress;
        const std::size_t length = section[i].Misc.VirtualSize;
        for (std::size_t at = 0; at + 35 <= length; ++at) {
            if (!SignatureMatches(text + at)) continue;
            found = const_cast<std::uint8_t *>(text + at);
            ++count;
        }
    }
    if (count != 1) {
        Log("[SteeringSync SOURCE] 1.61 visual signature count=" +
            std::to_string(count) + " (expected exactly 1)", 2);
        return nullptr;
    }
    return found;
}

void Store64(std::uint8_t *p, std::uintptr_t value) {
    static_assert(sizeof(std::uintptr_t) == 8, "64-bit process required");
    std::memcpy(p, &value, 8);
}

bool ChangeMemory(void *address, const void *bytes, std::size_t length) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(address, length, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    std::memcpy(address, bytes, length);
    FlushInstructionCache(GetCurrentProcess(), address, length);
    DWORD unused = 0;
    VirtualProtect(address, length, oldProtect, &unused);
    return true;
}

bool InstallVisualHook() {
    if (g_hookActive) return true;
    auto *start = FindSteeringSignature();
    if (!start) return false;

    // The retained MOV (7 B) is followed by LEA (7 B) and MOVSS (8 B).
    auto *patch = start + 7;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, patch + 3, sizeof(displacement));
    const std::uintptr_t originalLeaTarget =
        reinterpret_cast<std::uintptr_t>(patch + 7 + displacement);
    const std::uintptr_t sampleAddress =
        reinterpret_cast<std::uintptr_t>(&g_rawSteering);
    const std::uintptr_t resumeAddress =
        reinterpret_cast<std::uintptr_t>(patch + 15);

    // x64 executable trampoline, byte-for-byte same *logic* as working DLL:
    // mov rcx, originalLeaTarget
    // mov rax, &g_rawSteering
    // movss xmm6, dword ptr [rax]
    // mov rax, resumeAddress
    // jmp rax
    std::array<std::uint8_t, 36> stub{};
    stub[0] = 0x48; stub[1] = 0xB9; Store64(stub.data() + 2, originalLeaTarget);
    stub[10] = 0x48; stub[11] = 0xB8; Store64(stub.data() + 12, sampleAddress);
    stub[20] = 0xF3; stub[21] = 0x0F; stub[22] = 0x10; stub[23] = 0x30;
    stub[24] = 0x48; stub[25] = 0xB8; Store64(stub.data() + 26, resumeAddress);
    stub[34] = 0xFF; stub[35] = 0xE0;

    void *page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                              PAGE_EXECUTE_READWRITE);
    if (!page) return false;
    std::memcpy(page, stub.data(), stub.size());
    FlushInstructionCache(GetCurrentProcess(), page, stub.size());

    // Absolute RIP-indirect jump FF 25 00 00 00 00 <destination 64-bit>, NOP.
    std::array<std::uint8_t, 15> detour{};
    detour[0] = 0xFF; detour[1] = 0x25;
    Store64(detour.data() + 6, reinterpret_cast<std::uintptr_t>(page));
    detour[14] = 0x90;
    std::memcpy(g_savedInstructions.data(), patch, g_savedInstructions.size());
    if (!ChangeMemory(patch, detour.data(), detour.size())) {
        VirtualFree(page, 0, MEM_RELEASE);
        return false;
    }
    g_patchAddress = patch;
    g_trampoline = page;
    g_hookActive = true;
    Log("[SteeringSync SOURCE] 1.61 steering_w visual hook ACTIVE");
    return true;
}

void RemoveVisualHook() {
    if (!g_hookActive) return;
    if (g_patchAddress)
        ChangeMemory(g_patchAddress, g_savedInstructions.data(),
                     g_savedInstructions.size());
    // Game callbacks/shutdown normally run on its main thread, so no concurrent
    // trampoline execution is expected. Never unload the DLL while game is running.
    if (g_trampoline) VirtualFree(g_trampoline, 0, MEM_RELEASE);
    g_patchAddress = nullptr;
    g_trampoline = nullptr;
    g_hookActive = false;
}

void __cdecl OnSteeringChannel(const char *, std::uint32_t, const void *, void *) {
    // Called every telemetry frame even when the game's input value is unchanged.
    // We deliberately ignore game input; reading raw DirectInput bypasses nonlinear.
    PollWheel();
}

bool IsSupportedGame() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"amtrucks.exe") == 0 ||
           _wcsicmp(name, L"eurotrucks2.exe") == 0;
}

void ShutdownInternal() {
    RemoveVisualHook();
    CloseWheel();
    g_rawSteering = 0.0f;
    g_log = nullptr;
}

} // namespace

extern "C" __declspec(dllexport)
ScsResult __cdecl scs_telemetry_init(std::uint32_t version, const void *params) {
    // The working DLL accepts 1.02 and 1.03; the older 1.00/1.01 ABI prefix
    // is identical for the fields used here. Unsupported future versions fall back.
    if ((version >> 16) != 1 || (version & 0xFFFFu) > 3)
        return kScsUnsupported;
    if (!params) return kScsError;
    const auto *sdk = static_cast<const ScsInitParams *>(params);
    if (!sdk->register_for_channel) return kScsError;
    g_log = sdk->log;
    if (!IsSupportedGame()) {
        Log("[SteeringSync SOURCE] Executable is not ATS or ETS2", 2);
        g_log = nullptr;
        return kScsUnsupported;
    }

    Log("[SteeringSync SOURCE] Starting raw visual wheel synchronization");
    LoadConfig();
    if (!OpenWheel()) {
        ShutdownInternal();
        return kScsError;
    }
    PollWheel();
    if (!InstallVisualHook()) {
        Log("[SteeringSync SOURCE] Hook failed - game code remains unmodified", 2);
        ShutdownInternal();
        return kScsError;
    }
    if (sdk->register_for_channel("truck.input.steering", kNoIndex,
                                  kChannelFloat, kEachFrame,
                                  OnSteeringChannel, nullptr) != kScsOk) {
        Log("[SteeringSync SOURCE] Could not register telemetry callback", 2);
        ShutdownInternal();
        return kScsError;
    }
    Log("[SteeringSync SOURCE] Active; game steering physics unchanged");
    return kScsOk;
}

extern "C" __declspec(dllexport)
void __cdecl scs_telemetry_shutdown() {
    if (g_log) Log("[SteeringSync SOURCE] Unloading; restoring game instructions");
    ShutdownInternal();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_ownModule = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
