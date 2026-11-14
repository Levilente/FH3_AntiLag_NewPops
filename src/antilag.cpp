// Anti-Lag for Forza Horizon 3 (FH3 Mod Platform mod).
//
// A new entry in the game's own turbo upgrade list (Garage > Upgrades > Engine > Turbo / Twin Turbo): after Street, Sport and Race
// comes "Race Turbo w/ Anti-Lag" (the Race turbo's data with a marker value in TorqueDropOffRPM0, which the mod finds in the car's
// physics objects to know the car has it). A car with it gets four rows on its Tuning > Springs page (pressure decay, boost
// retention, cutoff RPM, pop interval), saved per garage car in fh3-mods\AntiLag.ini.
//
// How the boost works in the game (found from a probe recording and the code): the turbo model's step (game+0x1D82090, rcx = CCarDynamics,
// xmm1 = dt) keeps a shaft/compressor state at dyn+0x618 (0 to the limit at dyn+0xEF0), whose rate is dyn+0x61C, and the boost pressure
// at dyn+0x620 (dyn+0xEF4 to dyn+0xEF8) follows from it. When the throttle (dyn+0x1BD0) is lifted the state decays at once. Anti-lag
// hooks the step and, while the throttle is lifted above the cutoff RPM, keeps the state from falling below the retained level.
//
// A recorder (hotkey F8, development) dumps the
// player car's physics objects every other frame to fh3-mods\antilag-probe.bin (+ a map of the objects in
// fh3-mods\antilag-probe.txt), so the turbo's boost value and the exhaust-pop trigger can be found. The anti-lag
// itself helps to find more of the game's turbo/backfire state.
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
#include <map>
#include <string>
#include <vector>

bool sound_init(const char* type, float volume);
void sound_play(int strength, bool pos_ok, float lx, float ly, float lz);
void sound_fmod_attach(char* game, bool enable);
void sound_fmod_system(void* sys);
bool sound_fmod_ready();
int sound_fmod_state();
typedef bool (*SoundRelPosFn)(int tag, float out[3]);
bool sound_play_fmod(int strength, int tag, void (*log)(const char*, int));
void sound_fmod_set_relpos(SoundRelPosFn f);
void sound_fmod_update();
void* sound_studio_detour();
void** sound_studio_orig();
void sound_mark_pop();
void sound_native_setup(bool replace, void (*log)(const char*));
void sound_forget_learned();

void sound_shutdown();
void sound_set_type(const char* type);
void sound_set_reverb(float amount);
#include "car_engines.inc"

namespace {

constexpr size_t kOffIface    = 0x71E8;   // Car -> car dynamics interface (CCarDynamics + 0x4B0)
constexpr size_t kIfaceToDyn  = 0x4B0;
constexpr size_t kDynSize     = 0xD600;   // CCarDynamics object size dumped by the probe
constexpr size_t kCarSize     = 0x7800;   // Car object size dumped by the probe
constexpr uintptr_t kRvaCarUpdate = 0x18B8D20;
constexpr uintptr_t kRvaTurboStep = 0x1D82090;   // the turbo model's step (rcx = CCarDynamics, xmm1 = dt)
constexpr size_t kDynTurboState = 0x618;         // compressor/shaft state, 0 .. dyn+0xEF0
constexpr size_t kDynTurboRate  = 0x61C;
constexpr size_t kDynBoost      = 0x620;         // boost pressure, dyn+0xEF4 .. dyn+0xEF8
constexpr size_t kDynTurboMax   = 0xEF0;
constexpr size_t kDynThrottleIn = 0x1BD0;        // throttle 0..1 the engine torque is blended with
constexpr size_t kDynEngineOmega = 0x5F8;        // engine speed, rad/s
constexpr float kRadToRpm = 9.549296586f;
constexpr uintptr_t kRvaHudCtrl   = 0xB26690;
constexpr float kMarker = 9001.0f;   // TorqueDropOffRPM0 of the anti-lag turbo rows (the game's own rows have 9000)

const FH3ModApi* g_api = nullptr;
FH3Mod* g_mod          = nullptr;
char* g_game           = nullptr;
std::atomic<bool> g_stop{false};
HANDLE g_threads[2] = {};

// ---- config (fh3-mods\AntiLag.ini) -----------------------------------------------------------------------------------------------
int g_extra_price = 2000;   // what the anti-lag turbo costs more than the Race turbo
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

std::string cfg_str(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[64] = L"";
    GetPrivateProfileStringW(sec, key, L"", buf, 64, g_cfg_path.c_str());
    if (!buf[0]) { WritePrivateProfileStringW(sec, key, def, g_cfg_path.c_str()); lstrcpynW(buf, def, 64); }
    char o[64];
    WideCharToMultiByte(CP_ACP, 0, buf, -1, o, sizeof o, nullptr, nullptr);
    return o;
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

// Does the car (garage id) have the anti-lag turbo? Learnt while the car is driven (the marker is found in its physics objects);
// a car not seen yet shows the rows, so the player can always reach them.
std::atomic<char*> g_al_dyn{nullptr};
std::atomic<bool> g_al_holding{false};   // the turbo hook is holding the boost right now
constexpr size_t kCarBurble = 0x7478;          // BurbleBackfirePhysics object inside the Car (0xA8 bytes); +0xC = the pop event this frame (0 none, 1..3)
constexpr uintptr_t kRvaListener = 0x27BFD30;   // FMOD System::set3DListenerAttributes (system, listener, pos, vel, forward, up): the real camera
constexpr uintptr_t kRvaBurbleStep = 0x18BBB80; // its update (rcx = object, xmm1 = dt, r8 = pointer to the dynamics interface)
std::atomic<char*> g_al_car{nullptr};
std::atomic<bool> g_al_lifted_pop{false};      // lifted, above the cutoff and inside the hold time: pops wanted
   // the player's CCarDynamics, set while its car has the anti-lag turbo
std::atomic<int> g_al_decay{6000}, g_al_retention{85}, g_al_cutoff{1800}, g_al_pop{140};
CRITICAL_SECTION g_cs;
std::map<uint32_t, bool> g_has;
bool car_has_kit() {
    const uint32_t garage = g_api->upgrade_active_garage();
    EnterCriticalSection(&g_cs);
    auto it = g_has.find(garage);
    const bool r = it == g_has.end() ? true : it->second;
    LeaveCriticalSection(&g_cs);
    return r;
}

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
    static unsigned mapn = 0;
    wchar_t mname[48];
    _snwprintf(mname, 48, L"antilag-probe-map-%u.txt", ++mapn);
    FILE* f = _wfopen(probe_path(mname).c_str(), L"w");
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
    static unsigned n = 0;
    wchar_t name[64];
    _snwprintf(name, 64, L"antilag-probe-%u-%llu.bin", ++n, static_cast<unsigned long long>(GetTickCount64() / 1000));   // never overwrite an earlier recording
    FILE* f = _wfopen(probe_path(name).c_str(), L"wb");
    if (f) {
        std::fwrite(kMagic, 1, 8, f);
        const uint32_t hdr[4] = {g_rec_frame, static_cast<uint32_t>(kDynSize), static_cast<uint32_t>(kCarSize), static_cast<uint32_t>(g_rec_stride)};
        std::fwrite(hdr, sizeof hdr, 1, f);
        std::fwrite(g_rec_buf.data(), 1, g_rec_buf.size(), f);
        std::fclose(f);
    }
    say_always("probe: recording written, %u frames, %zu bytes (fh3-mods\\antilag-probe-*.bin)", g_rec_frame, g_rec_buf.size());
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

// Finds the marker float in the car's physics objects (once every half second). Returns the offset found (>= 0) or -1.
long scan_marker(char* car, char* dyn) {
    struct Region { char* base; size_t size; long tag; } regs[2] = {{dyn, kDynSize, 0x100000}, {car, kCarSize, 0x200000}};
    for (const Region& r : regs)
        for (size_t o = 0; o + 4 <= r.size; o += 4)
            if (*reinterpret_cast<const float*>(r.base + o) == kMarker) return r.tag + static_cast<long>(o);
    return -1;
}
ULONGLONG g_scan_t = 0;
void watch_car(char* car, char* iface) {
    const ULONGLONG t = GetTickCount64();
    if (t - g_scan_t < 500) return;
    g_scan_t = t;
    long at = -1;
    __try { at = scan_marker(car, iface - kIfaceToDyn); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const uint32_t garage = g_api->upgrade_active_garage();
    if (!garage) { g_al_dyn.store(nullptr); return; }
    if (at >= 0) {
        g_al_decay.store(value_for(garage, kDecay));
        g_al_retention.store(value_for(garage, kRetention));
        g_al_cutoff.store(value_for(garage, kCutoff));
        g_al_pop.store(value_for(garage, kPop));
        {
            static uint32_t last_garage = 0;
            const int ordinal = g_api->upgrade_car_model ? g_api->upgrade_car_model(static_cast<int32_t>(garage)) : 0;
            static int last_ordinal = -1;
            if (garage != last_garage || ordinal != last_ordinal) {
                last_garage = garage; last_ordinal = ordinal;
                const char* t = "";
                for (const auto& e : kCarEngines) if (e.ordinal == ordinal) { t = e.type; break; }
                sound_set_type(t);
                sound_forget_learned();
                say_always("anti-lag: pop sound for car %d: %s", ordinal, *t ? t : "all");
            }
        }
        g_al_dyn.store(iface - kIfaceToDyn);
        g_al_car.store(car);
    } else {
        g_al_dyn.store(nullptr);
    }
    EnterCriticalSection(&g_cs);
    auto it = g_has.find(garage);
    const bool changed = it == g_has.end() || it->second != (at >= 0);
    g_has[garage] = at >= 0;
    LeaveCriticalSection(&g_cs);
    if (changed) say_always("anti-lag: garage car %u %s (marker %s0x%lX)", garage, at >= 0 ? "HAS the anti-lag turbo" : "has no anti-lag turbo",
                            at >= 0x200000 ? "car+" : "dyn+", at >= 0 ? (at & 0xFFFFF) : 0);
}

// ---- the anti-lag itself ------------------------------------------------------------------------------------------------------------------
float g_peak        = 0;      // the turbo state while the throttle was down
float g_peak_boost  = 0;      // the boost pressure while the throttle was down
double g_lift_ms    = 0;      // when the throttle was lifted (0 = down)
bool g_lifted       = false;
int g_al_logged     = 0;

typedef uint64_t (*TurboStepFn)(void*, float);
TurboStepFn g_orig_turbo = nullptr;
uint64_t detour_turbo(void* self, float dt) {
    const uint64_t r = g_orig_turbo(self, dt);
    if (self != g_al_dyn.load() || g_stop.load()) return r;
    g_al_holding.store(false);
    g_al_lifted_pop.store(false);
    __try {
        char* d       = static_cast<char*>(self);
        float* state  = reinterpret_cast<float*>(d + kDynTurboState);
        const float thr = *reinterpret_cast<const float*>(d + kDynThrottleIn);
        const float rpm = *reinterpret_cast<const float*>(d + kDynEngineOmega) * kRadToRpm;
        const float lim = *reinterpret_cast<const float*>(d + kDynTurboMax);
        if (thr >= 0.5f) {                       // on the throttle: remember how spooled the turbo is
            g_peak       = *state;
            g_peak_boost = *reinterpret_cast<const float*>(d + kDynBoost);
            g_lifted     = false;
        } else if (g_peak > 0.0f && lim > 0.0f) {
            if (thr < 0.1f && !g_lifted) { g_lifted = true; g_lift_ms = now_ms(); }
            // once lifted the hold goes on while the throttle comes back up to half, so the game cannot pull the pressure down on a partial throttle
            const double since = g_lifted ? now_ms() - g_lift_ms : 1e9;
            const float cutoff = static_cast<float>(g_al_cutoff.load());
            if (g_lifted && rpm >= cutoff && since < g_al_decay.load()) {
                if (thr < 0.1f) g_al_lifted_pop.store(true);
                const double x = since / g_al_decay.load();   // 0..1; stays near the peak, sags slowly, then collapses at the end
                const double shape = (1.0 - 0.3 * x) * (1.0 - std::pow(x, 8.0));
                const float k = (g_al_retention.load() / 100.0f) * static_cast<float>(shape);
                g_al_holding.store(true);
                const float keep = g_peak * k;
                float* boost         = reinterpret_cast<float*>(d + kDynBoost);
                const float bmin     = *reinterpret_cast<const float*>(d + 0xEF4);   // boost at rest
                const float keep_b   = bmin + (g_peak_boost - bmin) * k;
                // The game pulls both the state (dyn+0x618) and the boost (dyn+0x620) towards rest as soon as the throttle is lifted.
                if (*state < keep) {
                    *state = keep;
                    *reinterpret_cast<float*>(d + kDynTurboRate) = 0.0f;
                }
                if (*boost < keep_b) *boost = keep_b;
                if (g_al_logged < 6 && g_verbose) {
                    ++g_al_logged;
                    say("anti-lag: holding the turbo at %.0f / boost %.1f (peak %.0f / %.1f, %.0f rpm, %.0f ms after lift)", static_cast<double>(keep),
                        static_cast<double>(keep_b), static_cast<double>(g_peak), static_cast<double>(g_peak_boost), static_cast<double>(rpm), since);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

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
        if (iface && (static_cast<void*>(car) == hud || static_cast<void*>(iface) == hud) && GetTickCount64() - g_hud_tick.load() < 600) { watch_car(car, iface); rec_frame(car, iface); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int logged = 0;
        if (logged++ < 5) say_always("anti-lag: a fault in the Car update step");
    }
    return r;
}


// ---- the pops: the game's own BurbleBackfire update; we add events while the anti-lag works and at the rev limiter --------------------------
typedef void (*BurbleFn)(void*, float, void**);
BurbleFn g_orig_burble = nullptr;
double g_last_pop = 0;
double g_next_gap = 0;
uint64_t g_rng = 0x9E3779B97F4A7C15ull;
double rnd01() {                                   // xorshift64*, seeded from the high resolution clock (GetTickCount64 steps by 15 ms: that made the pops look like a metronome)
    if (g_rng == 0x9E3779B97F4A7C15ull) { LARGE_INTEGER q; QueryPerformanceCounter(&q); g_rng ^= static_cast<uint64_t>(q.QuadPart) * 0xD6E8FEB86659FD93ull; }
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return static_cast<double>((g_rng * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
}
// Time to the next pop: mostly uneven (0.5x to 2x the setting), sometimes a quick burst of two or three, sometimes a gap.
double next_gap(double base) {
    const double r = rnd01();
    if (r < 0.20) return base * (0.15 + 0.25 * rnd01());
    if (r < 0.32) return base * (2.2 + 2.8 * rnd01());
    const double u = rnd01();
    return base * (0.5 + 1.6 * u * u);
}
// ---- the listener (the game's camera, as the game gives it to FMOD) and the exhaust pipes' positions -------------------------------------
struct Pose { float pos[3], fwd[3], up[3]; };
Pose g_listener{};
std::atomic<bool> g_listener_ok{false};
typedef int (*ListenerFn)(void*, int, const float*, const float*, const float*, const float*);
ListenerFn g_orig_listener = nullptr;
int detour_listener(void* sys, int idx, const float* pos, const float* vel, const float* fwd, const float* up) {
    sound_fmod_system(sys);
    if (idx == 0 && pos && fwd && up) {
        __try {
            Pose p; for (int i = 0; i < 3; ++i) { p.pos[i] = pos[i]; p.fwd[i] = fwd[i]; p.up[i] = up[i]; }
            g_listener = p; g_listener_ok.store(true);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        sound_fmod_update();   // our voices follow the pipe and the camera
    }
    return g_orig_listener(sys, idx, pos, vel, fwd, up);
}
static bool finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
int g_flip_pan = 0;
// The world position of one of the car's pipes (car + 0x88: eight weak pointers to the exhaust nodes, each node's vtable +0x10 gives its 4x4 matrix).
// ---- the pipes: the game's own exhaust locators (car + 0x88: eight weak pointers, 0x10 apart; each node's vtable +0x10 gives its 4x4 matrix in the car's frame) --
// The game builds its backfire message from exactly these (Car update at 0x18B8F42): 8 entries of 0x50 bytes, the matrix and a "fire" byte at +0x40.
constexpr int kTipSize = 0x50, kTipBytes = 0x280;
uint32_t gather_tips(char* car, uint8_t* msg, float locs[8][3]) {
    std::memset(msg, 0, kTipBytes);
    uint32_t mask = 0;
    for (int attempt = 0; attempt < 2 && !mask; ++attempt) {
        for (int i = 0; i < 8; ++i) {
            char* el = car + 0x88 + i * 0x10;
            void* obj = *reinterpret_cast<void**>(el);
            char* ctrl = *reinterpret_cast<char**>(el + 8);
            if (!obj || !ctrl || *reinterpret_cast<volatile int*>(ctrl + 8) <= 0) continue;
            auto getm = reinterpret_cast<const float* (*)(void*)>((*reinterpret_cast<void***>(obj))[2]);
            const float* m = getm(obj);
            if (!m || !readable(m, 0x40)) continue;
            std::memcpy(msg + i * kTipSize, m, 0x40);
            msg[i * kTipSize + 0x40] = 1;
            locs[i][0] = m[12]; locs[i][1] = m[13]; locs[i][2] = m[14];
            mask |= 1u << i;
        }
        if (!mask && attempt == 0) {
            // the game resolves the locators (car vtable +0x270) only when it posts a backfire itself; we post instead of it, so we ask for them
            static uint64_t last = 0; const uint64_t t = GetTickCount64();
            if (t - last < 500) break;
            last = t;
            reinterpret_cast<void (*)(void*)>((*reinterpret_cast<void***>(car))[0x270 / 8])(car);
        }
    }
    return mask;
}
// The flames: the very message the game posts for its own backfire, to the car's deferred presentation (car + 0x73A0, vtable +0x2E8).
bool g_flames = true;
bool g_native = true;   // the game plays the pop (its sound event, with our samples swapped in) and its own flames
bool post_flames(char* car, const uint8_t* msg) {
    if (!*reinterpret_cast<uint8_t*>(car + 0x7269)) return false;     // the game doesn't post for this car either
    void* deferred = *reinterpret_cast<void**>(car + 0x73A0);
    if (!deferred || !readable(deferred, 8)) return false;
    alignas(16) uint8_t buf[0x290] = {};
    std::memcpy(buf, msg, kTipBytes);
    reinterpret_cast<void (*)(void*, void*)>((*reinterpret_cast<void***>(deferred))[0x2E8 / 8])(deferred, buf);
    return true;
}

// Sources of the sounds playing: a pipe (in the car's frame) and the car's dynamics; followed every listener update.
struct Src { char* dyn = nullptr; float loc[3] = {0, 0, 0}; };
Src g_src[16];
int g_src_next = 0;
bool car_world(char* dyn, const float loc[3], float out[3]) {
    if (!dyn || !readable(dyn, 0xA0)) return false;
    const float* T = reinterpret_cast<const float*>(dyn + 0x30);      // CCarDynamics: position +0x30, rows right/up/forward at +0x70/+0x80/+0x90
    const float* R = reinterpret_cast<const float*>(dyn + 0x70);
    if (!finite3(T) || !finite3(R)) return false;
    for (int k = 0; k < 3; ++k) out[k] = loc[0] * R[k] + loc[1] * R[4 + k] + loc[2] * R[8 + k] + T[k];
    return true;
}
bool to_listener(const float w[3], float out[3]) {
    if (!g_listener_ok.load()) return false;
    const Pose L = g_listener;
    const float d[3] = { w[0] - L.pos[0], w[1] - L.pos[1], w[2] - L.pos[2] };
    float r[3] = { L.up[1] * L.fwd[2] - L.up[2] * L.fwd[1], L.up[2] * L.fwd[0] - L.up[0] * L.fwd[2], L.up[0] * L.fwd[1] - L.up[1] * L.fwd[0] };   // FMOD frame: right = up x forward
    if (g_flip_pan % 100) for (float& x : r) x = -x;
    out[0] = d[0] * r[0] + d[1] * r[1] + d[2] * r[2];
    out[1] = d[0] * L.up[0] + d[1] * L.up[1] + d[2] * L.up[2];
    out[2] = d[0] * L.fwd[0] + d[1] * L.fwd[1] + d[2] * L.fwd[2];
    return true;
}
bool src_relpos(int tag, float out[3]) {
    if (tag < 0 || tag >= 16) return false;
    float w[3];
    __try { if (!car_world(g_src[tag].dyn, g_src[tag].loc, w)) return false; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return to_listener(w, out);
}

void fmod_log(const char* what, int r) {
    static int n = 0;
    if (n < 12 || r != 0) { ++n; if (n < 40) say_always("anti-lag: FMOD %s -> %d", what, r); }
}
void play_pop(char* car, int strength) {
    alignas(16) uint8_t msg[kTipBytes];
    float locs[8][3] = {};
    const uint32_t mask = gather_tips(car, msg, locs);
    if (g_flames && mask) {
        const bool ok = post_flames(car, msg);
        static int logged = 0;
        if (logged < 3) { ++logged; say_always("anti-lag: flames posted on the pipes %02X: %s", mask, ok ? "yes" : "no (the car has no backfire presentation)"); }
    }
    if (!mask) { static int c = 0; if (c++ < 3) say_always("anti-lag: no exhaust locator on this car"); sound_play(strength, false, 0, 0, 0); return; }
    int pipes[8], n = 0;
    for (int i = 0; i < 8; ++i) if (mask & (1u << i)) pipes[n++] = i;
    const int pick = pipes[static_cast<size_t>(rnd01() * n) % n];
    const int tag = g_src_next++ & 15;
    g_src[tag].dyn = g_al_dyn.load();
    for (int k = 0; k < 3; ++k) g_src[tag].loc[k] = locs[pick][k];
    if (sound_fmod_ready() && sound_play_fmod(strength, tag, fmod_log)) return;
    float r[3];
    if (src_relpos(tag, r)) sound_play(strength, true, r[0], r[1], r[2]);
    else sound_play(strength, false, 0, 0, 0);
}

void detour_burble(void* self, float dt, void** iface) {
    g_orig_burble(self, dt, iface);
    char* car = g_al_car.load();
    if (!car || self != car + kCarBurble || g_stop.load() || !g_al_dyn.load()) return;
    __try {
        int* ev = reinterpret_cast<int*>(static_cast<char*>(self) + 0xC);
        bool want = g_al_lifted_pop.load();
        if (!want && iface && *iface) {                       // rev limiter on the throttle: the machine gun
            void* obj = *iface;
            auto limiter = reinterpret_cast<bool (*)(void*)>((*reinterpret_cast<void***>(obj))[0x4C8 / 8]);
            want = limiter(obj);
        }
        if (want && *ev == 0) {
            const double now = now_ms();
            const double base = static_cast<double>(g_al_pop.load());
            if (g_next_gap <= 0) g_next_gap = next_gap(base);
            if (now - g_last_pop >= g_next_gap) {
                g_last_pop  = now;
                g_next_gap  = next_gap(base);
                const double k = rnd01();
                *ev = k < 0.12 ? 1 : (k < 0.62 ? 2 : 3);   // soft pop, normal, big flame
            }
        }
        if (*ev != 0) {                                  // a pop this frame (the game's own or ours): our sound, and the game's effect if wanted
            sound_mark_pop();       // the game's backfire sound samples start right after this: that's how they are recognised and swapped for ours
            if (!g_native) { play_pop(car, *ev); *ev = 0; }   // the old way (Native=0): our own voice, our flame post
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
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

// ---- research: the tuning page's tab list (9 fixed tabs built by the scene constructor at 0x16DF700) -----------------------------------------------------------
thread_local void* t_tune_scene = nullptr;
typedef void* (*TuneSceneFn)(void*);
TuneSceneFn g_orig_tune_scene = nullptr;
typedef void* (*SetValueFn)(void*, void*, void*);
SetValueFn g_orig_setvalue = nullptr;
std::atomic<int> g_sv_log{0};
static uintptr_t rva_of(const void* p) { const uintptr_t a = reinterpret_cast<uintptr_t>(p), b = reinterpret_cast<uintptr_t>(g_game); return a >= b && a < b + 0x6000000 ? a - b : 0; }
void* detour_setvalue(void* el, void* dp, void* val) {
    if (t_tune_scene && g_sv_log.load() < 60) {
        g_sv_log.fetch_add(1);
        __try {
            const void* vte = readable(el, 8) ? *reinterpret_cast<void**>(el) : nullptr;
            const void* vtv = (val && readable(val, 8)) ? *reinterpret_cast<void**>(val) : nullptr;
            say_always("anti-lag: tuning scene SetValue(el %p vt+%llX, dp %p, val %p vt+%llX)%s", el, static_cast<unsigned long long>(rva_of(vte)), dp, val,
                       static_cast<unsigned long long>(rva_of(vtv)), el == *reinterpret_cast<void**>(static_cast<char*>(t_tune_scene) + 0x488) ? "  <- the tab list element" : "");
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return g_orig_setvalue(el, dp, val);
}
void* detour_tune_scene(void* self) {
    t_tune_scene = self;
    say_always("anti-lag: tuning scene constructor starts (this %p)", self);
    void* r = g_orig_tune_scene(self);
    __try {
        void* el = *reinterpret_cast<void**>(static_cast<char*>(self) + 0x488);
        say_always("anti-lag: tuning scene built: tab list element %p; object words:", el);
        for (int i = 0; i < 0x40 && readable(static_cast<char*>(el) + i * 8, 8); ++i) {
            const uintptr_t w = *reinterpret_cast<uintptr_t*>(static_cast<char*>(el) + i * 8);
            if (w) say_always("   el+0x%X = %llX (game+%llX)", i * 8, static_cast<unsigned long long>(w), static_cast<unsigned long long>(rva_of(reinterpret_cast<void*>(w))));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    t_tune_scene = nullptr;
    return r;
}

DWORD WINAPI install_hooks(LPVOID) {
    g_game = static_cast<char*>(g_api->wait_for_code(0xFFFFFFFFu));
    if (!g_game) { say_always("the game's code never became ready: no hooks"); return 0; }
    sound_fmod_attach(g_game, cfg_int(L"AntiLag", L"FMOD", 1) != 0);
    FH3Hook* h1 = g_api->hook_function(g_mod, "Car update (Anti-Lag)", g_game + kRvaCarUpdate, reinterpret_cast<void*>(&detour_car_update),
                                       reinterpret_cast<void**>(&g_orig_car_update));
    FH3Hook* h2 = g_api->hook_function(g_mod, "HUD controller (Anti-Lag: the player's car)", g_game + kRvaHudCtrl, reinterpret_cast<void*>(&detour_hud),
                                       reinterpret_cast<void**>(&g_orig_hud));
    FH3Hook* h3 = g_api->hook_function(g_mod, "Turbo model step (Anti-Lag)", g_game + kRvaTurboStep, reinterpret_cast<void*>(&detour_turbo),
                                       reinterpret_cast<void**>(&g_orig_turbo));
    FH3Hook* h4 = g_api->hook_function(g_mod, "Burble/backfire update (Anti-Lag)", g_game + kRvaBurbleStep, reinterpret_cast<void*>(&detour_burble),
                                       reinterpret_cast<void**>(&g_orig_burble));
    FH3Hook* h5 = g_api->hook_function(g_mod, "FMOD listener (Anti-Lag: 3D sound)", g_game + kRvaListener, reinterpret_cast<void*>(&detour_listener),
                                       reinterpret_cast<void**>(&g_orig_listener));
    say("hook: listener %s", h5 && g_api->enable_hooks(g_mod, h5) ? "on" : "FAILED");
    FH3Hook* h8 = g_api->hook_function(g_mod, "Tuning scene constructor (research)", g_game + 0x16DF700, reinterpret_cast<void*>(&detour_tune_scene), reinterpret_cast<void**>(&g_orig_tune_scene));
    FH3Hook* h9 = g_api->hook_function(g_mod, "SetValue (research)", g_game + 0x2145760, reinterpret_cast<void*>(&detour_setvalue), reinterpret_cast<void**>(&g_orig_setvalue));
    say("hook: tuning scene %s, SetValue %s", h8 && g_api->enable_hooks(g_mod, h8) ? "on" : "FAILED", h9 && g_api->enable_hooks(g_mod, h9) ? "on" : "FAILED");
    FH3Hook* hs = g_api->hook_function(g_mod, "Studio sound instrument play (Anti-Lag: our pop samples)", g_game + 0x29823B0, sound_studio_detour(), sound_studio_orig());
    say_always("anti-lag: native pop sound hook %s", hs && g_api->enable_hooks(g_mod, hs) ? "on" : "FAILED");
    say("hook: burble %s", h4 && g_api->enable_hooks(g_mod, h4) ? "on" : "FAILED");
    say("hook: turbo step %s", h3 && g_api->enable_hooks(g_mod, h3) ? "on" : "FAILED");
    const bool a = h1 && g_api->enable_hooks(g_mod, h1);
    const bool b = h2 && g_api->enable_hooks(g_mod, h2);
    say("hooks: Car update %s, HUD controller %s", a ? "on" : "FAILED", b ? "on" : "FAILED");
    if (!a || !b) say_always("anti-lag: could not hook the game (Car update %d, HUD %d)", a ? 1 : 0, b ? 1 : 0);
    return 0;
}

// ---- the turbo entries (pure data: SQL on the game's database copy + new texts) --------------------------------------------------------
// UpgradeTypes 8 = single turbo (List_UpgradeEngineTurboSingle), 51 = twin turbo (List_UpgradeEngineTurboTwin); Level 1 to 3 are Street, Sport
// and Race. Level 4 is new: a copy of every engine's Race row, Price + extra, TorqueDropOffRPM0 = 9001 (the marker; the game's rows have 9000).
constexpr int kLevel          = 4;
constexpr int kUpgradeIdSingle = 9000020;
constexpr int kUpgradeIdTwin   = 9000021;
constexpr uint32_t kKeyNameSingle = 0xD4A60201u;
constexpr uint32_t kKeyDescSingle = 0xD4A60202u;
constexpr uint32_t kKeyNameTwin   = 0xD4A60203u;
constexpr uint32_t kKeyDescTwin   = 0xD4A60204u;
const char kIconSingle[]  = "GAME:\\Media\\UI\\Textures\\Data_Bound\\Upgrade_Parts\\Engine_Power\\turbo_single_3.swatchbin";
const char kImageSingle[] = "GAME:\\Media\\UI\\Textures\\Data_Bound\\Upgrade_Parts\\Engine_Power\\turbo_single_LG_3.swatchbin";
const char kIconTwin[]    = "GAME:\\Media\\UI\\Textures\\Data_Bound\\Upgrade_Parts\\Engine_Power\\turbo_twin_3.swatchbin";
const char kImageTwin[]   = "GAME:\\Media\\UI\\Textures\\Data_Bound\\Upgrade_Parts\\Engine_Power\\turbo_twin_LG_3.swatchbin";

std::string text_ref(uint32_t key) { return "_&" + std::to_string((static_cast<unsigned long long>(FH3_TABLE_UPGRADES) << 32) | key); }

std::string rows_sql(const char* table) {
    char b[1500];
    std::snprintf(b, sizeof b,
                  "INSERT OR IGNORE INTO %s (Id,EngineID,Level,IsStock,ManufacturerID,MassDiff,WeightDistDiff,Price,MomentInertia,MaxScale,PowerMaxScale,MinScale,"
                  "PowerMinScale,RobScale,TorqueDropOffRPM0,TorqueDropOffRPM1,TorqueDropOffScale0,TorqueDropOffScale1) "
                  "SELECT EngineID*1000+%d,EngineID,%d,0,ManufacturerID,MassDiff,WeightDistDiff,Price+%d,MomentInertia,MaxScale,PowerMaxScale,MinScale,"
                  "PowerMinScale,RobScale,%.1f,TorqueDropOffRPM1,TorqueDropOffScale0,TorqueDropOffScale1 FROM %s r WHERE r.Level=3 "
                  "AND NOT EXISTS (SELECT 1 FROM %s x WHERE x.EngineID=r.EngineID AND x.Level=%d);",
                  table, kLevel, kLevel, g_extra_price, static_cast<double>(kMarker), table, table, kLevel);
    return b;
}

void add_turbo_entries() {
    g_api->add_string_key(g_mod, "Upgrades.str", kKeyNameSingle, L"Race Turbo w/ Anti-Lag");
    g_api->add_string_key(g_mod, "Upgrades.str", kKeyDescSingle,
                          L"The Race turbo with anti-lag: it stays spooled when you lift off the throttle, with exhaust pops and flames. "
                          L"Tune it on the car's Tuning, Springs page.");
    g_api->add_string_key(g_mod, "Upgrades.str", kKeyNameTwin, L"Race Twin Turbo w/ Anti-Lag");
    g_api->add_string_key(g_mod, "Upgrades.str", kKeyDescTwin,
                          L"The Race twin turbo with anti-lag: it stays spooled when you lift off the throttle, with exhaust pops and flames. "
                          L"Tune it on the car's Tuning, Springs page.");
    std::string sql;
    char b[900];
    std::snprintf(b, sizeof b, "INSERT OR REPLACE INTO Upgrades (id,TypeId,IsException,Name,Level,Description,IconPath,ImagePath) VALUES (%d,'8',0,'%s',%d,'%s','%s','%s');",
                  kUpgradeIdSingle, text_ref(kKeyNameSingle).c_str(), kLevel, text_ref(kKeyDescSingle).c_str(), kIconSingle, kImageSingle);
    sql += b;
    std::snprintf(b, sizeof b, "INSERT OR REPLACE INTO Upgrades (id,TypeId,IsException,Name,Level,Description,IconPath,ImagePath) VALUES (%d,'51',0,'%s',%d,'%s','%s','%s');",
                  kUpgradeIdTwin, text_ref(kKeyNameTwin).c_str(), kLevel, text_ref(kKeyDescTwin).c_str(), kIconTwin, kImageTwin);
    sql += b;
    sql += rows_sql("List_UpgradeEngineTurboSingle");
    sql += rows_sql("List_UpgradeEngineTurboTwin");
    g_api->add_sql(g_mod, sql.c_str());
}

}  // namespace

FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api) {
    if (api->api_version < 13) return 1;
    g_api = api;
    InitializeCriticalSection(&g_cs);

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
    g_extra_price = cfg_int(L"AntiLag", L"ExtraPrice", g_extra_price);
    g_verbose  = cfg_int(L"AntiLag", L"Log", 0) != 0;
    g_flames = cfg_int(L"AntiLag", L"Flames", 1) != 0;
    g_native = cfg_int(L"AntiLag", L"Native", 1) != 0;
    sound_native_setup(cfg_int(L"AntiLag", L"PopSound", 1) != 0, [](const char* m) { say_always("%s", m); });
    sound_fmod_set_relpos(&src_relpos);
    {
        const std::string type = cfg_str(L"AntiLag", L"Sound", L"Auto");
        const int vol = cfg_int(L"AntiLag", L"SoundVolume", 55);
        g_flip_pan = cfg_int(L"AntiLag", L"FlipPan", 0);
        sound_set_reverb(static_cast<float>(cfg_int(L"AntiLag", L"Reverb", 8)) / 100.0f);
        if (cfg_int(L"AntiLag", L"PopSound", 1) && !sound_init(type.c_str(), static_cast<float>(vol) / 100.0f)) say_always("anti-lag: the pop sound could not start");
    }
    for (int s = 0; s < kSettingCount; ++s) g_defaults[s] = cfg_int(L"Defaults", kDefs[s].ini, kDefs[s].def);

    if (api->api_version >= 10) add_turbo_entries();
    else say_always("anti-lag: this platform (API %u) can not add the turbo entries (needs API 10)", api->api_version);

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
    g_al_dyn.store(nullptr);
    sound_shutdown();
    for (HANDLE& h : g_threads)
        if (h) {
            WaitForSingleObject(h, 3000);
            CloseHandle(h);
            h = nullptr;
        }
}
