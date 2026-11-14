// The pop sounds: FH6's backfire samples (packed into sounds.pak, linked into the DLL) mixed by a small waveOut mixer of our own.
// Every pop picks a random sample (of the chosen engine type, or all of them), with a random pitch, volume and pan.
#include <windows.h>
#include <xmmintrin.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

__asm__(".section .rdata,\"dr\"\n.global al_sounds_start\nal_sounds_start:\n.incbin \"" ANTILAG_SOUNDS_PAK "\"\n.global al_sounds_end\nal_sounds_end:\n.byte 0\n.text\n");
extern "C" const uint8_t al_sounds_start[];
extern "C" const uint8_t al_sounds_end[];

namespace {
constexpr uint32_t kMaxFrames = 96000;   // the whole sample (the longest is 0.74 s): cutting the tails off made every pop sound like a creaking chest
struct Sample { std::string type; const int16_t* data; uint32_t frames; };
std::vector<Sample> g_samples;
std::vector<int> g_pick;                       // indices of the samples in use
std::string g_type_cfg = "Auto";                // Auto (by the car), All, or one type

struct Voice { const int16_t* data = nullptr; uint32_t frames = 0; double pos = 0, step = 1; float gl = 0, gr = 0; double dl = 0, dr = 0; float lpl = 0, lpr = 0, al = 1, ar = 1; };
constexpr int kVoices = 6, kRate = 48000, kBuf = 480, kBufs = 6;   // 10 ms buffers; the samples are 48 kHz too, so Windows has nothing to resample
Voice g_voices[kVoices];
SRWLOCK g_lock = SRWLOCK_INIT;
HWAVEOUT g_wo = nullptr;
WAVEHDR g_hdr[kBufs];
int16_t g_pcm[kBufs][kBuf * 2];
HANDLE g_ev = nullptr, g_thread = nullptr;
std::atomic<bool> g_run{false};
std::atomic<float> g_volume{0.8f};
std::atomic<float> g_reverb{0.1f};
uint64_t g_rng = 88172645463325252ull;
double rnd() { g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27; return static_cast<double>((g_rng * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0; }

// A small reverb (Schroeder: parallel combs into two allpasses) gives the pops a room to be in.
struct Comb { std::vector<float> b; size_t i = 0; float lp = 0; float proc(float x, float fb, float damp) { float y = b[i]; lp = y * (1 - damp) + lp * damp; b[i] = x + lp * fb; if (++i >= b.size()) i = 0; return y; } };
struct Allpass { std::vector<float> b; size_t i = 0; float proc(float x) { float y = b[i]; b[i] = x + y * 0.5f; float o = y - x; if (++i >= b.size()) i = 0; return o; } };
Comb g_combL[4], g_combR[4];
Allpass g_apL[2], g_apR[2];
void reverb_init() {
    const size_t cl[4] = {1116, 1188, 1277, 1356};
    for (int i = 0; i < 4; ++i) { g_combL[i].b.assign(cl[i], 0.0f); g_combR[i].b.assign(cl[i] + 23, 0.0f); }
    const size_t al[2] = {556, 441};
    for (int i = 0; i < 2; ++i) { g_apL[i].b.assign(al[i], 0.0f); g_apR[i].b.assign(al[i] + 23, 0.0f); }
}

void fill(int16_t* out) {
    float mix[kBuf * 2] = {};
    AcquireSRWLockExclusive(&g_lock);
    for (Voice& v : g_voices) {
        if (!v.data) continue;
        for (int i = 0; i < kBuf; ++i) {
            // each ear reads the sample a little later (the far ear: interaural delay) and a little duller (the head shadows high notes)
            const double pl = v.pos - v.dl, pr = v.pos - v.dr;
            auto at = [&](double p) -> float {
                if (p < 0) return 0.0f;
                const uint32_t k = static_cast<uint32_t>(p);
                if (k + 1 >= v.frames) return 0.0f;
                const float f = static_cast<float>(p - k);
                float e = 1.0f;                                             // short fades: no click at the start or the end of a sample
                if (k < 24) e = static_cast<float>(k) / 24.0f;
                else if (k + 6240 > v.frames) e = static_cast<float>(v.frames - k) / 6240.0f;
                return e * (static_cast<float>(v.data[k]) * (1.0f - f) + static_cast<float>(v.data[k + 1]) * f) / 32768.0f;
            };
            v.lpl += (at(pl) - v.lpl) * v.al;
            v.lpr += (at(pr) - v.lpr) * v.ar;
            mix[i * 2] += v.lpl * v.gl;
            mix[i * 2 + 1] += v.lpr * v.gr;
            v.pos += v.step;
            if (v.pos >= v.frames + 40) { v.data = nullptr; break; }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (int i = 0; i < kBuf; ++i) {
        const float dry_l = mix[i * 2], dry_r = mix[i * 2 + 1], in = (dry_l + dry_r) * 0.18f;
        float wl = 0, wr = 0;
        for (int c = 0; c < 4; ++c) { wl += g_combL[c].proc(in, 0.80f, 0.35f); wr += g_combR[c].proc(in, 0.80f, 0.35f); }
        for (int a = 0; a < 2; ++a) { wl = g_apL[a].proc(wl); wr = g_apR[a].proc(wr); }
        const float rv = g_reverb.load();
        float l = dry_l + wl * rv, r = dry_r + wr * rv;
        l = l / (1.0f + std::fabs(l) * 0.6f) * 1.3f;            // soft clip
        r = r / (1.0f + std::fabs(r) * 0.6f) * 1.3f;
        l = l > 1.0f ? 1.0f : (l < -1.0f ? -1.0f : l);
        r = r > 1.0f ? 1.0f : (r < -1.0f ? -1.0f : r);
        out[i * 2] = static_cast<int16_t>(l * 32760.0f);
        out[i * 2 + 1] = static_cast<int16_t>(r * 32760.0f);
    }
}

DWORD WINAPI mixer(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    _mm_setcsr(_mm_getcsr() | 0x8040);                          // flush denormals to zero: the reverb tail would otherwise slow the thread down
    while (g_run.load()) {
        WaitForSingleObject(g_ev, 50);
        for (int b = 0; b < kBufs; ++b)
            if (g_hdr[b].dwFlags & WHDR_DONE) { fill(g_pcm[b]); g_hdr[b].dwFlags &= ~WHDR_DONE; waveOutWrite(g_wo, &g_hdr[b], sizeof(WAVEHDR)); }
    }
    return 0;
}
}  // namespace

// The pop sound type for the car being driven ("" or an unknown name = all of them). With Sound=<type> in the ini that type always wins.
void sound_set_type(const char* type) {
    if (_stricmp(g_type_cfg.c_str(), "Auto") != 0 && _stricmp(g_type_cfg.c_str(), "All") != 0) type = g_type_cfg.c_str();
    else if (_stricmp(g_type_cfg.c_str(), "All") == 0) type = "";
    std::vector<int> pick;
    for (size_t i = 0; i < g_samples.size(); ++i)
        if (!type || !*type || _stricmp(g_samples[i].type.c_str(), type) == 0) pick.push_back(static_cast<int>(i));
    if (pick.empty()) for (size_t i = 0; i < g_samples.size(); ++i) pick.push_back(static_cast<int>(i));
    AcquireSRWLockExclusive(&g_lock);
    g_pick.swap(pick);
    ReleaseSRWLockExclusive(&g_lock);
}

void sound_set_reverb(float amount) { g_reverb.store(amount); }

bool sound_init(const char* type, float volume) {
    if (g_run.load()) return true;
    const uint8_t* p = al_sounds_start;
    if (std::memcmp(p, "ALSP", 4) != 0) return false;
    uint32_t n;
    std::memcpy(&n, p + 4, 4);
    p += 8;
    g_samples.clear();
    for (uint32_t i = 0; i < n && p < al_sounds_end; ++i) {
        char t[13] = {};
        std::memcpy(t, p, 12);
        uint32_t frames;
        std::memcpy(&frames, p + 12, 4);
        g_samples.push_back({t, reinterpret_cast<const int16_t*>(p + 16), frames < kMaxFrames ? frames : kMaxFrames});
        p += 16 + static_cast<size_t>(frames) * 2;
    }
    g_type_cfg = (type && *type) ? type : "Auto";
    sound_set_type(_stricmp(g_type_cfg.c_str(), "Auto") == 0 ? "" : g_type_cfg.c_str());
    reverb_init();
    g_volume.store(volume);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    g_rng ^= static_cast<uint64_t>(q.QuadPart) * 0x9E3779B97F4A7C15ull;
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = kRate; wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4; wf.nAvgBytesPerSec = kRate * 4;
    g_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, reinterpret_cast<DWORD_PTR>(g_ev), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) return false;
    for (int b = 0; b < kBufs; ++b) {
        std::memset(g_pcm[b], 0, sizeof g_pcm[b]);
        g_hdr[b] = {};
        g_hdr[b].lpData = reinterpret_cast<LPSTR>(g_pcm[b]);
        g_hdr[b].dwBufferLength = sizeof g_pcm[b];
        waveOutPrepareHeader(g_wo, &g_hdr[b], sizeof(WAVEHDR));
        waveOutWrite(g_wo, &g_hdr[b], sizeof(WAVEHDR));
    }
    g_run.store(true);
    g_thread = CreateThread(nullptr, 0, mixer, nullptr, 0, nullptr);
    return g_thread != nullptr;
}


// ---- the game's own FMOD: the samples become FMOD sounds and play as 3D channels at the pipe's world position ------------------------------
// The functions are the public (static linked) FMOD core API of the game's exe; the System handle is the one the game gives to set3DListenerAttributes.
typedef bool (*SoundRelPosFn)(int tag, float out[3]);
namespace {
typedef int (*CreateSoundFn)(void*, const void*, unsigned, void*, void**);
typedef int (*PlaySoundFn)(void*, void*, void*, bool, void**);
typedef int (*SetFloatFn)(void*, float);
typedef int (*SetPausedFn)(void*, bool);
constexpr uintptr_t kCreateSound = 0x27BE3F0, kPlaySound = 0x27BFA40, kSetVolume = 0x27C3450, kSetPitch = 0x27C3160, kSetPaused = 0x27C3080;
char* g_fgame = nullptr;
std::atomic<void*> g_fsys{nullptr};
std::atomic<bool> g_fmod_on{false};
std::vector<void*> g_fsound;                  // one FMOD sound per sample, made on first use
std::vector<std::vector<int16_t>> g_fpcm;
int g_fmod_err = 0;
}
void sound_fmod_attach(char* game, bool enable) { g_fgame = game; g_fmod_on.store(enable && game); }
void sound_fmod_system(void* sys) { if (sys) g_fsys.store(sys); }
int sound_fmod_state() { return (g_fmod_on.load() ? 1 : 0) | (g_fsys.load() ? 2 : 0) | (g_fmod_err ? 4 : 0); }
bool sound_fmod_ready() { return g_fmod_on.load() && g_fsys.load() && !g_fmod_err; }

// The game runs FMOD with more than one listener, and FMOD's plain 3D channels don't pan then (they come out in the middle of the head).
// So our channels are 2D, and their speaker mix (direction) and volume (distance) are set by us from the listener the game sets, every update,
// like the game's own spatialiser does for its events. They play in the game's own mixer, on its master bus.
typedef int (*SetMixFn)(void*, float*, int, int, int);
typedef int (*SwFmtFn)(void*, int*, int*, int*);
typedef int (*StopFn)(void*);
constexpr uintptr_t kSetMix = 0x27C2DB0, kSwFmt = 0x27BF4F0, kStop = 0x27C3620;
namespace {
struct FVoice { void* ch = nullptr; int tag = -1; uint64_t end = 0; float vol = 0; };
FVoice g_fv[16];
SRWLOCK g_fvlock = SRWLOCK_INIT;
SoundRelPosFn g_relpos = nullptr;
int g_spk_mode = -1, g_spk_n = 2;
struct Spk { int idx; float ang; };
std::vector<Spk> g_spk;
void speakers_init(void* sys) {
    int rate = 0, mode = 0, raw = 0;
    if (reinterpret_cast<SwFmtFn>(g_fgame + kSwFmt)(sys, &rate, &mode, &raw) != 0) mode = 3;
    g_spk_mode = mode;
    g_spk.clear();
    switch (mode) {                                          // FMOD_SPEAKERMODE: 2 mono, 3 stereo, 4 quad, 5 surround, 6 5.1, 7 7.1 (angles: degrees, + right)
    case 2: g_spk_n = 1; g_spk = {{0, 0}}; break;
    case 4: g_spk_n = 4; g_spk = {{0, -45}, {1, 45}, {2, -135}, {3, 135}}; break;
    case 5: g_spk_n = 5; g_spk = {{0, -30}, {1, 30}, {2, 0}, {3, -110}, {4, 110}}; break;
    case 6: g_spk_n = 6; g_spk = {{0, -30}, {1, 30}, {2, 0}, {4, -110}, {5, 110}}; break;
    case 7: g_spk_n = 8; g_spk = {{0, -30}, {1, 30}, {2, 0}, {4, -90}, {5, 90}, {6, -150}, {7, 150}}; break;
    default: g_spk_n = 2; g_spk = {{0, -90}, {1, 90}}; break;
    }
}
// gains for a source at listener-relative (x right, y up, z ahead): constant power between the two speakers around its direction; very close sources spread out
void mix_for(const float r[3], float* m, float* att) {
    for (int i = 0; i < 8; ++i) m[i] = 0;
    const float d = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    *att = d <= 3.0f ? 1.0f : 3.0f / (3.0f + (d - 3.0f) * 0.85f);      // full up to 3 m, then falls off like the game's car sounds
    if (g_spk_n == 1) { m[0] = 1; return; }
    const float spread = d < 1.0f ? 1.0f - d : 0.0f;                    // inside a metre the sound is around you
    if (g_spk_n == 2) {
        const float p = d > 0.01f ? r[0] / (d > 0.5f ? d : 0.5f) : 0.0f; // -1 left .. +1 right (the side, front and back alike: headphones/stereo)
        const float pp = p * (1.0f - spread);
        const float a = (pp + 1.0f) * 0.785398f;
        m[0] = std::cos(a); m[1] = std::sin(a);
        return;
    }
    float az = std::atan2(r[0], r[2]) * 57.29578f;                      // 0 ahead, +90 right, +-180 behind
    const size_t n = g_spk.size();
    std::vector<Spk> s = g_spk;
    std::sort(s.begin(), s.end(), [](const Spk& x, const Spk& y) { return x.ang < y.ang; });
    size_t lo = n - 1, hi = 0;
    for (size_t i = 0; i < n; ++i) { if (s[i].ang <= az) lo = i; }
    hi = (lo + 1) % n;
    float a0 = s[lo].ang, a1 = s[hi].ang;
    if (a1 <= a0) a1 += 360.0f;
    float x = az; if (x < a0) x += 360.0f;
    const float t = a1 > a0 ? (x - a0) / (a1 - a0) : 0.0f;
    m[s[lo].idx] = std::cos(t * 1.570796f) * (1.0f - spread);
    m[s[hi].idx] += std::sin(t * 1.570796f) * (1.0f - spread);
    if (spread > 0) { const float e = spread / std::sqrt(static_cast<float>(n)); for (const Spk& k : g_spk) m[k.idx] += e; }
}
void apply(FVoice& v) {
    float r[3];
    if (!g_relpos || !g_relpos(v.tag, r)) return;
    float m[8], att;
    mix_for(r, m, &att);
    reinterpret_cast<SetMixFn>(g_fgame + kSetMix)(v.ch, m, g_spk_n, 1, 1);
    reinterpret_cast<SetFloatFn>(g_fgame + kSetVolume)(v.ch, v.vol * att);
}
}
void sound_fmod_set_relpos(SoundRelPosFn f) { g_relpos = f; }

// Every listener update: the voices still playing follow the pipe and the camera.
void sound_fmod_update() {
    if (!sound_fmod_ready()) return;
    const uint64_t now = GetTickCount64();
    AcquireSRWLockExclusive(&g_fvlock);
    __try {
        for (FVoice& v : g_fv) {
            if (!v.ch) continue;
            if (now > v.end) { v.ch = nullptr; continue; }
            apply(v);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ReleaseSRWLockExclusive(&g_fvlock);
}

bool sound_play_fmod(int strength, int tag, void (*log)(const char*, int)) {
    if (!sound_fmod_ready() || g_pick.empty()) return false;
    __try {
        void* sys = g_fsys.load();
        if (g_spk_mode < 0) { speakers_init(sys); if (log) log("speaker mode", g_spk_mode); }
        AcquireSRWLockExclusive(&g_lock);
        const int idx = g_pick[static_cast<size_t>(rnd() * static_cast<double>(g_pick.size())) % g_pick.size()];
        ReleaseSRWLockExclusive(&g_lock);
        if (g_fsound.size() < g_samples.size()) { g_fsound.resize(g_samples.size(), nullptr); g_fpcm.resize(g_samples.size()); }
        const Sample& sm = g_samples[static_cast<size_t>(idx)];
        if (!g_fsound[static_cast<size_t>(idx)]) {
            std::vector<int16_t>& pcm = g_fpcm[static_cast<size_t>(idx)];
            pcm.assign(sm.data, sm.data + sm.frames);
            const uint32_t fade = 480;                                  // 10 ms fade out: the end must not click
            for (uint32_t i = 0; i < fade && i < sm.frames; ++i) pcm[sm.frames - 1 - i] = static_cast<int16_t>(pcm[sm.frames - 1 - i] * (static_cast<float>(i) / fade));
            alignas(8) unsigned char ex[0x20] = {};
            *reinterpret_cast<int*>(ex + 0x00) = 0x18;
            *reinterpret_cast<unsigned*>(ex + 0x04) = sm.frames * 2;
            *reinterpret_cast<int*>(ex + 0x0C) = 1;
            *reinterpret_cast<int*>(ex + 0x10) = 48000;
            *reinterpret_cast<int*>(ex + 0x14) = 2;                     // PCM16
            void* snd = nullptr;
            const unsigned mode = 0x1 | 0x8 | 0x100 | 0x800 | 0x1000;   // LOOP_OFF | 2D | CREATESAMPLE | OPENMEMORY | OPENRAW
            const int r = reinterpret_cast<CreateSoundFn>(g_fgame + kCreateSound)(sys, pcm.data(), mode, ex, &snd);
            if (log) log("createSound", r);
            if (r != 0 || !snd) { g_fmod_err = r ? r : -1; return false; }
            g_fsound[static_cast<size_t>(idx)] = snd;
        }
        void* ch = nullptr;
        int r = reinterpret_cast<PlaySoundFn>(g_fgame + kPlaySound)(sys, g_fsound[static_cast<size_t>(idx)], nullptr, true, &ch);
        if (log) log("playSound", r);
        if (r != 0 || !ch) return false;
        const float base = strength >= 3 ? 1.0f : (strength == 2 ? 0.72f : 0.45f);
        const float pitch = static_cast<float>(0.97 + 0.06 * rnd());
        reinterpret_cast<SetFloatFn>(g_fgame + kSetPitch)(ch, pitch);
        AcquireSRWLockExclusive(&g_fvlock);
        FVoice* slot = &g_fv[0];
        for (FVoice& v : g_fv) { if (!v.ch) { slot = &v; break; } if (v.end < slot->end) slot = &v; }
        if (slot->ch) reinterpret_cast<StopFn>(g_fgame + kStop)(slot->ch);   // all 16 busy: the oldest makes room
        slot->ch = ch;
        slot->tag = tag;
        slot->vol = base * g_volume.load() * static_cast<float>(0.9 + 0.2 * rnd());
        slot->end = GetTickCount64() + static_cast<uint64_t>(1000.0 * sm.frames / 48000.0 / pitch) + 60;
        apply(*slot);
        ReleaseSRWLockExclusive(&g_fvlock);
        r = reinterpret_cast<SetPausedFn>(g_fgame + kSetPaused)(ch, false);
        if (log) log("start (2D, mixed by us)", r);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fmod_err = -2;
        return false;
    }
}


// ---- the native way: the game's car sound event plays its own backfire samples through Studio's sound-instrument player (0x29823B0);
// those samples are recognised (they start right after the game's pops and only then) and our sample is handed to the player instead.
// Everything else (where it sounds from, distance, muffling inside the car, reverb, the mix) stays the game's own.
typedef int (*StudioPlayFn)(void* snd, void* sys, void* grp, bool paused, int a5, void** ch);
typedef int (*GetLenFn)(void*, unsigned*, unsigned);
namespace {
StudioPlayFn g_orig_studio_play = nullptr;
std::atomic<double> g_pop_ms{-1e9};
SRWLOCK g_stlock = SRWLOCK_INIT;
std::atomic<bool> g_replace{true};
void (*g_slog)(const char*) = nullptr;
std::vector<void*> g_rsound;             // our samples as FMOD sounds (2D, like the bank's), made on first use
double ms_now() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return 1000.0 * static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart); }
void* our_sound(void* sys, int idx) {
    if (g_rsound.size() < g_samples.size()) { g_rsound.resize(g_samples.size(), nullptr); g_fpcm.resize(g_samples.size()); }
    if (g_rsound[static_cast<size_t>(idx)]) return g_rsound[static_cast<size_t>(idx)];
    const Sample& sm = g_samples[static_cast<size_t>(idx)];
    std::vector<int16_t>& pcm = g_fpcm[static_cast<size_t>(idx)];
    pcm.assign(sm.data, sm.data + sm.frames);
    const float g = g_volume.load() * 1.8f;                               // SoundVolume 55 = the samples as they are
    for (uint32_t i = 0; i < sm.frames; ++i) {
        float v = static_cast<float>(pcm[i]) * g;
        if (i + 480 > sm.frames) v *= static_cast<float>(sm.frames - i) / 480.0f;   // 10 ms fade at the end
        pcm[i] = static_cast<int16_t>(v > 32767.f ? 32767.f : (v < -32768.f ? -32768.f : v));
    }
    alignas(8) unsigned char ex[0x20] = {};
    *reinterpret_cast<int*>(ex + 0x00) = 0x18;
    *reinterpret_cast<unsigned*>(ex + 0x04) = sm.frames * 2;
    *reinterpret_cast<int*>(ex + 0x0C) = 1;
    *reinterpret_cast<int*>(ex + 0x10) = 48000;
    *reinterpret_cast<int*>(ex + 0x14) = 2;
    void* snd = nullptr;
    const unsigned mode = 0x1 | 0x8 | 0x100 | 0x800 | 0x1000;            // LOOP_OFF | 2D | CREATESAMPLE | OPENMEMORY | OPENRAW (the bank's instruments are 2D: Studio places them)
    if (reinterpret_cast<CreateSoundFn>(g_fgame + kCreateSound)(sys, pcm.data(), mode, ex, &snd) != 0) return nullptr;
    g_rsound[static_cast<size_t>(idx)] = snd;
    return snd;
}
// Which of a bank's samples are its backfires is read from the game's own banks (tools/backfire_table.py: the "Backfire" parameter of each car's
// sound event, down through its nested events and multi instruments to the waveforms): bank_backfires.inc, one row per bank.
struct BankBf { uint32_t n; uint8_t cnt; uint16_t idx[16]; uint32_t len[16]; };
#include "bank_backfires.inc"
std::unordered_map<void*, int> g_parents;            // sample banks seen: 1 = has backfires (its samples are in g_bf), 0 = not a car bank we know
std::unordered_map<void*, bool> g_bf;                // the game's backfire samples
unsigned sub_len(void* sub) { unsigned l = 0; return reinterpret_cast<GetLenFn>(g_fgame + 0x27C0F50)(sub, &l, 2) == 0 ? l : 0; }   // in PCM samples
// The bank a sample belongs to, by reading memory only (no FMOD call that could change a sound: getSubSound seeks streams, that crashed the game):
// the sample's parent (+0x310, its subsound parent) holds an array of its samples; the sample's place in it is its index in the bank.
std::unordered_set<void*> g_seen;
int g_arr_off = -1;                                     // where the parent keeps the array (found once)
// Only memory that is committed, readable and not a guard page (a stack's guard page touched by a stray read stops that thread's stack from growing).
size_t readable_span(const void* p) {
    MEMORY_BASIC_INFORMATION mb;
    if (!p || !VirtualQuery(p, &mb, sizeof mb) || mb.State != MEM_COMMIT) return 0;
    const DWORD pr = mb.Protect & 0xFF;
    if ((mb.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) return 0;
    return static_cast<size_t>(static_cast<const char*>(mb.BaseAddress) + mb.RegionSize - static_cast<const char*>(p));
}
template <class T> bool rd(const void* p, T* out) {
    if (readable_span(p) < sizeof(T)) return false;
    *out = *static_cast<const T*>(p);
    return true;
}
int index_in(void** arr, void* snd, int maxn) {
    const size_t span = readable_span(arr) / sizeof(void*);
    const int n = static_cast<int>(span < static_cast<size_t>(maxn) ? span : static_cast<size_t>(maxn));
    for (int k = 0; k < n; ++k) if (arr[k] == snd) return k;
    return -1;
}
// research: the sample object and everything it points to (one level, 0x800 bytes each) to fh3-mods\antilag-snd.bin
wchar_t g_dump_path[MAX_PATH] = L"";
void dump_sound(void* snd) {
    static int n = 0;
    if (n >= 6 || !g_dump_path[0]) return;
    ++n;
    FILE* f = _wfopen(g_dump_path, n == 1 ? L"wb" : L"ab");
    if (!f) return;
    auto put = [&](const void* p, size_t sz) {
        uint64_t a = reinterpret_cast<uint64_t>(p); uint32_t z = static_cast<uint32_t>(sz);
        std::fwrite(&a, 8, 1, f); std::fwrite(&z, 4, 1, f); std::fwrite(p, 1, sz, f);
    };
    const uint32_t mark = 0x534E4421; std::fwrite(&mark, 4, 1, f);
    const unsigned len = sub_len(snd); std::fwrite(&len, 4, 1, f);
    if (readable_span(snd) < 0x800) { std::fclose(f); return; }
    put(snd, 0x800);
    for (int o = 0; o < 0x800; o += 8) {
        void* q = *reinterpret_cast<void**>(static_cast<char*>(snd) + o);
        if (!q || (reinterpret_cast<uintptr_t>(q) & 7)) continue;
        const size_t sp = readable_span(q);
        if (sp >= 0x40) put(q, sp < 0x800 ? sp : 0x800);
    }
    std::fclose(f);
}
void find_parent(void* snd) {
    const unsigned len = sub_len(snd);
    if (len == 0 || len > 72000) return;                 // backfires are short: nothing else is looked at
    void* parent = nullptr; uintptr_t vt = 0;
    if (!rd(static_cast<char*>(snd) + 0x310, &parent) || !parent || !rd(snd, &vt)) return;
    if (g_parents.count(parent)) { return; }
    g_parents[parent] = 0;                               // decided once per bank, found or not
    void** arr = nullptr; int k = -1;
    if (g_arr_off >= 0) { if (rd(static_cast<char*>(parent) + g_arr_off, &arr) && arr) k = index_in(arr, snd, 4096); }
    else {
        for (int o = 8; o < 0x400 && k < 0; o += 8) {
            void** a = nullptr;
            if (!rd(static_cast<char*>(parent) + o, &a) || !a || (reinterpret_cast<uintptr_t>(a) & 7) || reinterpret_cast<uintptr_t>(a) < 0x10000) continue;
            const int kk = index_in(a, snd, 4096);
            if (kk >= 0) { arr = a; k = kk; g_arr_off = o; if (g_slog) { char m[96]; std::snprintf(m, sizeof m, "anti-lag: a bank's sample list is at +0x%X", o); g_slog(m); } }
        }
    }
    if (k < 0 || !arr) {                                // not a bank's sample (or not found): leave this bank undecided
        dump_sound(snd);
        static int miss = 0;
        if (miss < 3 && g_slog) { ++miss; g_slog("anti-lag: a played sample's place in its bank was not found"); }
        return;
    }
    // which car bank: the one whose backfire samples (index and length) all match
    for (const BankBf& b : kBankBf) {
        bool ok = true;
        void* subs[16] = {};
        for (int j = 0; j < b.cnt && ok; ++j) {
            uintptr_t v2 = 0;
            ok = b.idx[j] < b.n && rd(&arr[b.idx[j]], &subs[j]) && subs[j] && rd(subs[j], &v2) && v2 == vt && sub_len(subs[j]) == b.len[j];
        }
        if (!ok) continue;
        // the bank's size must agree too: the entry after the last sample is not a sample of the same kind
        for (int j = 0; j < b.cnt; ++j) g_bf[subs[j]] = true;
        g_parents[parent] = 1;
        char m[160];
        std::snprintf(m, sizeof m, "anti-lag: a car's sample bank is playing: its %d backfire samples play ours", b.cnt);
        if (g_slog) g_slog(m);
        return;
    }
    g_parents[parent] = 0;
}
int detour_studio_play(void* snd, void* sys, void* grp, bool paused, int a5, void** ch) {
    void* use = snd;
    __try {
        AcquireSRWLockExclusive(&g_stlock);
        if (g_seen.insert(snd).second) find_parent(snd);       // a sample not seen yet: which bank is it from
        const bool bf = g_bf.find(snd) != g_bf.end();
        ReleaseSRWLockExclusive(&g_stlock);
        static int calls = 0;
        if (++calls == 1 && g_slog) g_slog("anti-lag: the game's sound-instrument player runs through us");
        if (bf && g_replace.load() && !g_pick.empty()) {
            AcquireSRWLockExclusive(&g_lock);
            const int idx = g_pick[static_cast<size_t>(rnd() * static_cast<double>(g_pick.size())) % g_pick.size()];
            ReleaseSRWLockExclusive(&g_lock);
            if (void* o = our_sound(sys, idx)) use = o;
            static int logged = 0;
            if (logged < 3 && g_slog) { ++logged; g_slog("anti-lag: the game plays a backfire sample: ours instead"); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { use = snd; }
    return g_orig_studio_play(use, sys, grp, paused, a5, ch);
}
}
void* sound_studio_detour() { return reinterpret_cast<void*>(&detour_studio_play); }
void** sound_studio_orig() { return reinterpret_cast<void**>(&g_orig_studio_play); }
void sound_set_dump_path(const wchar_t* p) { wcsncpy(g_dump_path, p, MAX_PATH - 1); }
void sound_mark_pop() { g_pop_ms.store(ms_now()); }
void sound_native_setup(bool replace, void (*log)(const char*)) { g_replace.store(replace); g_slog = log; }
void sound_forget_learned() {}

// strength 1 = soft, 2 = normal, 3 = big. When pos_ok, (lx, ly, lz) is the pipe's position in the listener's own frame (x right, y up, z ahead, metres);
// otherwise the pop comes from a random pipe a bit behind the listener.
void sound_play(int strength, bool pos_ok, float lx, float ly, float lz) {
    if (!g_run.load()) return;
    const float base = strength >= 3 ? 1.0f : (strength == 2 ? 0.72f : 0.45f);
    double side = (rnd() < 0.5 ? -1.0 : 1.0) * (0.25 + 0.55 * rnd());   // -1 left .. +1 right
    float dist_g = 1.0f, dist_lp = 1.0f;
    if (pos_ok) {
        const float d = std::sqrt(lx * lx + ly * ly + lz * lz);
        const float dd = d < 0.3f ? 0.3f : d;
        side = lx / dd;                                              // sine of the angle off the nose
        dist_g = 1.0f / (1.0f + 0.35f * (dd - 1.0f > 0 ? dd - 1.0f : 0.0f));
        if (dist_g < 0.06f) dist_g = 0.06f;
        float lp = 1.0f - 0.035f * dd;                                // air absorbs the highs
        if (lz < 0) lp -= 0.25f * (-lz / dd);                         // behind the head: duller
        dist_lp = lp < 0.25f ? 0.25f : lp;
    }
    const float gain = base * g_volume.load() * dist_g * static_cast<float>(0.9 + 0.2 * rnd());
    AcquireSRWLockExclusive(&g_lock);
    if (g_pick.empty()) { ReleaseSRWLockExclusive(&g_lock); return; }
    const Sample& s = g_samples[static_cast<size_t>(g_pick[static_cast<size_t>(rnd() * static_cast<double>(g_pick.size())) % g_pick.size()])];
    Voice* slot = &g_voices[0];
    bool found = false;
    for (Voice& v : g_voices) if (!v.data) { slot = &v; found = true; break; }
    if (!found) for (Voice& v : g_voices) if (v.pos > slot->pos) slot = &v;   // all busy: take the one furthest along
    slot->data = s.data;
    slot->frames = s.frames;
    slot->pos = 0;
    slot->step = (0.97 + 0.06 * rnd());
    const double far_side = side;                                   // >0: the source is on the right, the left ear is the far one
    const double as = std::fabs(side) > 1.0 ? 1.0 : std::fabs(side);
    const float near_g = 1.0f, far_g = static_cast<float>(1.0 - 0.45 * as);
    slot->gl = gain * (far_side > 0 ? far_g : near_g);
    slot->gr = gain * (far_side > 0 ? near_g : far_g);
    slot->dl = far_side > 0 ? 24.0 * as : 0.0;                       // up to about 0.5 ms later at the far ear
    slot->dr = far_side > 0 ? 0.0 : 24.0 * as;
    const float fa = static_cast<float>(1.0 - 0.65 * as);
    slot->al = (far_side > 0 ? fa : 1.0f) * dist_lp;
    slot->ar = (far_side > 0 ? 1.0f : fa) * dist_lp;
    slot->lpl = slot->lpr = 0.0f;
    ReleaseSRWLockExclusive(&g_lock);
}

void sound_shutdown() {
    if (!g_run.exchange(false)) return;
    SetEvent(g_ev);
    if (g_thread) { WaitForSingleObject(g_thread, 500); CloseHandle(g_thread); g_thread = nullptr; }
    waveOutReset(g_wo);
    for (WAVEHDR& h : g_hdr) waveOutUnprepareHeader(g_wo, &h, sizeof h);
    waveOutClose(g_wo);
    g_wo = nullptr;
}
