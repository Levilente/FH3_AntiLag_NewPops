// Anti-Lag for Forza Horizon 3 (FH3 Mod Platform mod).
//
// Garage > Upgrades > Engine > "Anti-Lag" (Stock / Anti-Lag). A car with it gets four rows on its Tuning > Springs page
// (pressure decay, boost retention, cutoff RPM, pop interval), saved per garage car in fh3-mods\AntiLag.ini.
//
// Version 0.1.0 is a PROBE build: it registers the upgrade and the rows, and has a recorder (hotkey F8) that dumps the
// player car's physics objects every other frame to fh3-mods\antilag-probe.bin (+ a map of the objects in
// fh3-mods\antilag-probe.txt), so the turbo's boost value and the exhaust-pop trigger can be found. The anti-lag
// itself is not active yet.
//
// Player car detection and the Car::Update / HUD controller hooks are the same as Launch Control's (read from the
// game's code; see the platform's docs/car-mods.md and docs/fh3-game-notes.md).
#include "fh3mod.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr size_t kOffIface    = 0x71E8;   // Car -> car dynamics interface (CCarDynamics + 0x4B0)
constexpr size_t kIfaceToDyn  = 0x4B0;
constexpr size_t kIfOmega     = 0x1D4;    // interface: engine angular velocity (rad/s)
constexpr size_t kIfGear      = 0xAA8;    // interface: gear (byte, low nibble)
constexpr size_t kDynSize     = 0xD600;   // CCarDynamics object size dumped by the probe
constexpr size_t kCarSize     = 0x7800;   // Car object size dumped by the probe
constexpr uintptr_t kRvaCarUpdate = 0x18B8D20;
constexpr uintptr_t kRvaHudCtrl   = 0xB26690;
constexpr float kRadPerSecToRpm   = 9.549296586f;

const FH3ModApi* g_api = nullptr;
FH3Mod* g_mod          = nullptr;
int g_handle           = -1;
char* g_game           = nullptr;
std::atomic<bool> g_stop{false};
HANDLE g_threads[2] = {};

// ---- config (fh3-mods\AntiLag.ini) -----------------------------------------------------------------------------------------------
int g_price      = 6000;
bool g_verbose   = false;
std::wstring g_cfg_path;

void say(const char* fmt, ...) {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_api && g_mod && g_verbose) g_api->log(g_mod, line);
}
void say_always(const char* fmt, ...) {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_api && g_mod) g_api->log(g_mod, line);
}

int cfg_int(const wchar_t* sec, const wchar_t* key, int def) {
    wchar_t buf[64] = L"";
    GetPrivateProfileStringW(sec, key, L"", buf, 64, g_cfg_path.c_str());
    if (buf[0]) return static_cast<int>(std::wcstol(buf, nullptr, 10));
    wchar_t v[32];
    _snwprintf(v, 32, L"%d", def);
    WritePrivateProfileStringW(sec, key, v, g_cfg_path.c_str());
    return def;
}

// ---- the four settings, per garage car ----------------------------------------------------------------------------------------------
enum Setting { kDecay, kRetention, kCutoff, kPop, kSettingCount };
struct SettingDef { const char* key; const char* label; const char* help; float min, max, step; int def; const wchar_t* ini; };
const SettingDef kDefs[kSettingCount] = {
    {"Decay", "Anti-Lag|Pressure decay (ms)",
     "How long the stored boost takes to bleed away after you lift off the throttle. Longer keeps the turbo spooled for longer.", 500, 10000, 100, 6000, L"Decay"},
    {"Retention", "Anti-Lag|Boost retention (%)",
     "How much of the boost the turbo keeps when you lift off the throttle. Higher means less lag when you get back on it.", 0, 100, 5, 85, L"Retention"},
    {"Cutoff", "Anti-Lag|Cutoff RPM",
     "Below this engine speed anti-lag does nothing.", 1000, 4000, 100, 1800, L"Cutoff"},
    {"Pop", "Anti-Lag|Pop interval (ms)",
     "Time between exhaust pops while anti-lag is working. Smaller means a faster crackle.", 60, 400, 10, 140, L"Pop"},
};
int g_defaults[kSettingCount];

int value_for(uint32_t garage, int s) {
    wchar_t sec[24];
    _snwprintf(sec, 24, L"Car%u", garage);
    wchar_t buf[32] = L"";
    GetPrivateProfileStringW(sec, kDefs[s].ini, L"", buf, 32, g_cfg_path.c_str());
    int v = g_defaults[s];
    if (buf[0]) v = static_cast<int>(std::wcstol(buf, nullptr, 10));
    const int lo = static_cast<int>(kDefs[s].min), hi = static_cast<int>(kDefs[s].max);
    return v < lo ? lo : v > hi ? hi : v;
}
void set_value(uint32_t garage, int s, int v) {
    wchar_t sec[24], val[24];
    _snwprintf(sec, 24, L"Car%u", garage);
    _snwprintf(val, 24, L"%d", v);
    WritePrivateProfileStringW(sec, kDefs[s].ini, val, g_cfg_path.c_str());
}

bool car_has_kit() { return g_handle >= 0 && g_api->upgrade_stage(g_mod, g_handle, 0) > 0; }

float row_get(void* u) { return static_cast<float>(value_for(g_api->upgrade_active_garage(), static_cast<int>(reinterpret_cast<intptr_t>(u)))); }
void row_set(void* u, float v) {
    const int s = static_cast<int>(reinterpret_cast<intptr_t>(u));
    set_value(g_api->upgrade_active_garage(), s, static_cast<int>(std::lround(v)));
    say("tuning: %s for garage car %u set to %d", kDefs[s].key, g_api->upgrade_active_garage(), static_cast<int>(std::lround(v)));
}
int row_visible(void*) { return car_has_kit() ? 1 : 0; }

// ---- memory helpers -----------------------------------------------------------------------------------------------------------------
bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}

double now_ms() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

// ---- input (throttle only, for the probe's notes) -------------------------------------------------------------------------------------
std::atomic<int> g_rt{0};
typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, void*);
struct PadState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };

bool game_in_front() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

DWORD WINAPI input_thread(LPVOID) {
    XInputGetStateFn get = nullptr;
    for (const wchar_t* n : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        if (HMODULE m = LoadLibraryW(n)) {
            get = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
            if (get) break;
        }
    }
    for (;;) {
        Sleep(8);
        if (g_stop.load()) return 0;
        int rt = 0;
        if (game_in_front()) {
            if (get)
                for (DWORD i = 0; i < 4; ++i) {
                    PadState st{};
                    if (get(i, &st) == ERROR_SUCCESS && st.rt > rt) rt = st.rt;
                }
            if ((GetAsyncKeyState(VK_UP) & 0x8000) || (GetAsyncKeyState('W') & 0x8000)) rt = 255;
        }
        g_rt.store(rt);
    }
}

// ---- the probe recorder -----------------------------------------------------------------------------------------------------------------
// F8 starts a recording (about 14 seconds, every other frame of the player's car); F8 again stops it early. Drive a turbo car:
// full throttle through a gear or two, lift for two seconds, back on, lift again; then stop.
std::atomic<bool> g_rec_toggle{false};
bool g_rec          = false;
double g_rec_start  = 0;
unsigned g_rec_frame = 0;
std::vector<uint8_t> g_rec_buf;
size_t g_rec_stride = 0;
const char kMagic[8] = {'A', 'L', 'P', 'R', 'O', 'B', 'E', '1'};

std::wstring probe_path(const wchar_t* name) {
    std::wstring p(g_api->azimuth_dir);
    p += L"\\fh3-mods\\";
    p += name;
    return p;
}

void write_map(char* car, char* dyn) {
    FILE* f = _wfopen(probe_path(L"antilag-probe.txt").c_str(), L"w");
    if (!f) return;
    std::fprintf(f, "game base %p\ncar %p\ndyn (CCarDynamics) %p iface %p\n", static_cast<void*>(g_game), static_cast<void*>(car), static_cast<void*>(dyn),
                 static_cast<void*>(dyn + kIfaceToDyn));
    struct Region { const char* tag; char* base; size_t size; } regs[2] = {{"car", car, kCarSize}, {"dyn", dyn, kDynSize}};
    for (const Region& r : regs) {
        for (size_t o = 0; o + 8 <= r.size; o += 8) {
            const uint64_t q = *reinterpret_cast<const uint64_t*>(r.base + o);
            if (q > 0x10000 && q < 0x7FFFFFFFFFFFull && readable(reinterpret_cast<void*>(q), 8)) {
                const uint64_t vt = *reinterpret_cast<const uint64_t*>(q);
                const uint64_t rva = vt - reinterpret_cast<uint64_t>(g_game);
                if (vt > reinterpret_cast<uint64_t>(g_game) && rva < 0x5400000)
                    std::fprintf(f, "%s+%zX -> object %llX, vtable rva %llX\n", r.tag, o, static_cast<unsigned long long>(q), static_cast<unsigned long long>(rva));
            } else if (q >= reinterpret_cast<uint64_t>(g_game) + 0x4000000 && q < reinterpret_cast<uint64_t>(g_game) + 0x5400000) {
                std::fprintf(f, "%s+%zX = vtable-like rva %llX\n", r.tag, o, static_cast<unsigned long long>(q - reinterpret_cast<uint64_t>(g_game)));
            }
        }
    }
    std::fclose(f);
}

void rec_stop() {
    g_rec = false;
    FILE* f = _wfopen(probe_path(L"antilag-probe.bin").c_str(), L"wb");
    if (f) {
        std::fwrite(kMagic, 1, 8, f);
        const uint32_t hdr[4] = {g_rec_frame, static_cast<uint32_t>(kDynSize), static_cast<uint32_t>(kCarSize), static_cast<uint32_t>(g_rec_stride)};
        std::fwrite(hdr, sizeof hdr, 1, f);
        std::fwrite(g_rec_buf.data(), 1, g_rec_buf.size(), f);
        std::fclose(f);
    }
    say_always("probe: recording written, %u frames, %zu bytes (fh3-mods\\antilag-probe.bin)", g_rec_frame, g_rec_buf.size());
    g_rec_buf.clear();
    g_rec_buf.shrink_to_fit();
}

void rec_frame(char* car, char* iface) {
    char* dyn = iface - kIfaceToDyn;
    if (g_rec_toggle.exchange(false)) {
        if (!g_rec) {
            g_rec_stride = 32 + kDynSize + kCarSize;
            g_rec_buf.clear();
            g_rec_buf.reserve(g_rec_stride * 450);
            g_rec_frame = 0;
            g_rec_start = now_ms();
            write_map(car, dyn);
            g_rec = true;
            say_always("probe: recording started (F8 again to stop, 14 s at most)");
            return;
        }
        rec_stop();
        return;
    }
    if (!g_rec) return;
    static unsigned skip = 0;
    if ((skip++ & 1) == 0) {
        const size_t at = g_rec_buf.size();
        g_rec_buf.resize(at + g_rec_stride);
        uint8_t* p = g_rec_buf.data() + at;
        std::memset(p, 0, 32);
        const double t = now_ms() - g_rec_start;
        std::memcpy(p, &t, 8);
        const int rt = g_rt.load();
        std::memcpy(p + 8, &rt, 4);
        __try {
            std::memcpy(p + 32, dyn, kDynSize);
            std::memcpy(p + 32 + kDynSize, car, kCarSize);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        ++g_rec_frame;
    }
    if (now_ms() - g_rec_start > 14000.0) rec_stop();
}

void on_rec_key(void*, const char*) { g_rec_toggle.store(true); }

// ---- hooks ------------------------------------------------------------------------------------------------------------------------------
std::atomic<void*> g_hud_player{nullptr};
std::atomic<ULONGLONG> g_hud_tick{0};

typedef uint64_t (*CarUpdateFn)(void*, float);
CarUpdateFn g_orig_car_update = nullptr;
uint64_t detour_car_update(void* self, float dt) {
    const uint64_t r = g_orig_car_update(self, dt);
    char* car        = static_cast<char*>(self);
    void* hud        = g_hud_player.load();
    if (!car || !hud || g_stop.load()) return r;
    __try {
        char* iface = *reinterpret_cast<char**>(car + kOffIface);
        if (iface && (static_cast<void*>(car) == hud || static_cast<void*>(iface) == hud) && GetTickCount64() - g_hud_tick.load() < 600) rec_frame(car, iface);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int logged = 0;
        if (logged++ < 5) say_always("anti-lag: a fault in the Car update step");
    }
    return r;
}

typedef uint64_t (*HudCtrlFn)(void*, void*, void*, void*);
HudCtrlFn g_orig_hud = nullptr;
uint64_t detour_hud(void* c, void* a, void* b, void* d) {
    const uint64_t r = g_orig_hud(c, a, b, d);
    if (c && readable(c, 0x130)) {
        void* obj = *reinterpret_cast<void**>(static_cast<char*>(c) + 0x120);
        void* cb  = *reinterpret_cast<void**>(static_cast<char*>(c) + 0x128);
        g_hud_player.store(cb ? obj : nullptr);
        g_hud_tick.store(GetTickCount64());
    }
    return r;
}

DWORD WINAPI install_hooks(LPVOID) {
    g_game = static_cast<char*>(g_api->wait_for_code(0xFFFFFFFFu));
    if (!g_game) { say_always("the game's code never became ready: no hooks"); return 0; }
    FH3Hook* h1 = g_api->hook_function(g_mod, "Car update (Anti-Lag)", g_game + kRvaCarUpdate, reinterpret_cast<void*>(&detour_car_update),
                                       reinterpret_cast<void**>(&g_orig_car_update));
    FH3Hook* h2 = g_api->hook_function(g_mod, "HUD controller (Anti-Lag: the player's car)", g_game + kRvaHudCtrl, reinterpret_cast<void*>(&detour_hud),
                                       reinterpret_cast<void**>(&g_orig_hud));
    const bool a = h1 && g_api->enable_hooks(g_mod, h1);
    const bool b = h2 && g_api->enable_hooks(g_mod, h2);
    say("hooks: Car update %s, HUD controller %s", a ? "on" : "FAILED", b ? "on" : "FAILED");
    if (!a || !b) say_always("anti-lag: could not hook the game (Car update %d, HUD %d)", a ? 1 : 0, b ? 1 : 0);
    return 0;
}

// ---- the upgrade -------------------------------------------------------------------------------------------------------------------------
int add_upgrade() {
    static const wchar_t* names[2] = {L"Stock", L"Anti-Lag"};
    static const wchar_t* descs[2] = {L"No anti-lag.",
                                      L"Keeps the turbo spooled when you lift off the throttle, with exhaust pops. Tune it on the car's Tuning, Springs page, in the Anti-Lag rows."};
    static FH3UpgradeStage stages[2];
    for (int i = 0; i < 2; ++i) stages[i] = {names[i], descs[i], i ? g_price : 0, nullptr, nullptr};
    FH3UpgradeDef d{};
    d.size           = sizeof d;
    d.id             = "antilag.system";
    d.name           = L"Anti-Lag";
    d.description    = L"Keeps the turbo spooled when you lift off the throttle, with exhaust pops and flames.";
    d.area           = 1;   // Engine
    d.stage_count    = 2;
    d.stages         = stages;
    d.preferred_slot = -1;
    return g_api->register_upgrade(g_mod, &d);
}

}  // namespace

FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api) {
    if (api->api_version < 13) return 1;
    g_api = api;

    FH3ModInfo info{};
    info.size             = sizeof info;
    info.name             = "Anti-Lag";
    info.version          = ANTILAG_VERSION;
    info.github_repo      = "Levilente/FH3_AntiLag";
    info.platform_version = FH3MOD_PLATFORM_VERSION;
    info.check_updates    = 1;
    info.description      = "Anti-lag for turbo cars: boost kept on lift-off, with exhaust pops. A tuning menu for decay, retention, cutoff and pop interval.";
    info.offline_only     = 1;
    g_mod                 = api->register_mod(&info);
    if (!g_mod) return 1;

    g_cfg_path = std::wstring(api->azimuth_dir) + L"\\fh3-mods\\AntiLag.ini";
    g_price    = cfg_int(L"AntiLag", L"Price", g_price);
    g_verbose  = cfg_int(L"AntiLag", L"Log", 0) != 0;
    for (int s = 0; s < kSettingCount; ++s) g_defaults[s] = cfg_int(L"Defaults", kDefs[s].ini, kDefs[s].def);

    g_handle = add_upgrade();
    say("registered: upgrade handle %d (price %d)", g_handle, g_price);
    if (g_handle < 0) return 1;

    for (int s = 0; s < kSettingCount; ++s) {
        FH3TuningRow row{};
        row.size     = sizeof row;
        row.key      = kDefs[s].key;
        row.label    = kDefs[s].label;
        row.help     = kDefs[s].help;
        row.min      = kDefs[s].min;
        row.max      = kDefs[s].max;
        row.step     = kDefs[s].step;
        row.decimals = 0;
        row.get      = row_get;
        row.set      = row_set;
        row.visible  = row_visible;
        row.user     = reinterpret_cast<void*>(static_cast<intptr_t>(s));
        say("tuning row %s: %s", kDefs[s].key, api->add_tuning_row(g_mod, &row) ? "added" : "NOT added");
    }

    if (api->api_version >= 9 && api->add_hotkey) {
        FH3Hotkey hk{};
        hk.size     = sizeof hk;
        hk.key      = "Probe";
        hk.label    = "Record the probe (development)";
        hk.vk       = VK_F8;
        hk.on_press = on_rec_key;
        api->add_hotkey(g_mod, &hk);
    }

    g_threads[0] = CreateThread(nullptr, 0, install_hooks, nullptr, 0, nullptr);
    g_threads[1] = CreateThread(nullptr, 0, input_thread, nullptr, 0, nullptr);
    return 0;
}

FH3MOD_EXPORT void fh3mod_shutdown(void) {
    g_stop.store(true);
    for (HANDLE& h : g_threads)
        if (h) {
            WaitForSingleObject(h, 3000);
            CloseHandle(h);
            h = nullptr;
        }
}
