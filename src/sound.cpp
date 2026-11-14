// The pop sounds: FH6's backfire samples (packed into sounds.pak, linked into the DLL) mixed by a small waveOut mixer of our own.
// Every pop picks a random sample (of the chosen engine type, or all of them), with a random pitch, volume and pan.
#include <windows.h>
#include <xmmintrin.h>
#include <mmsystem.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

__asm__(".section .rdata,\"dr\"\n.global al_sounds_start\nal_sounds_start:\n.incbin \"" ANTILAG_SOUNDS_PAK "\"\n.global al_sounds_end\nal_sounds_end:\n.byte 0\n.text\n");
extern "C" const uint8_t al_sounds_start[];
extern "C" const uint8_t al_sounds_end[];

namespace {
constexpr uint32_t kMaxFrames = 13440;   // 280 ms: the long tails of overlapping pops turned into a wash of echoes
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
namespace {
typedef int (*CreateSoundFn)(void*, const void*, unsigned, void*, void**);
typedef int (*PlaySoundFn)(void*, void*, void*, bool, void**);
typedef int (*Set3DFn)(void*, const float*, const float*, const void*);
typedef int (*SetFloatFn)(void*, float);
typedef int (*SetPausedFn)(void*, bool);
constexpr uintptr_t kCreateSound = 0x27BE3F0, kPlaySound = 0x27BFA40, kSet3D = 0x27C28F0, kSetVolume = 0x27C3450, kSetPitch = 0x27C3160, kSetPaused = 0x27C3080;
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

bool sound_play_fmod(int strength, const float pos[3], void (*log)(const char*, int)) {
    if (!sound_fmod_ready() || g_pick.empty()) return false;
    __try {
        void* sys = g_fsys.load();
        AcquireSRWLockExclusive(&g_lock);
        const int idx = g_pick[static_cast<size_t>(rnd() * static_cast<double>(g_pick.size())) % g_pick.size()];
        ReleaseSRWLockExclusive(&g_lock);
        if (g_fsound.size() < g_samples.size()) { g_fsound.resize(g_samples.size(), nullptr); g_fpcm.resize(g_samples.size()); }
        if (!g_fsound[static_cast<size_t>(idx)]) {
            const Sample& sm = g_samples[static_cast<size_t>(idx)];
            std::vector<int16_t>& pcm = g_fpcm[static_cast<size_t>(idx)];
            pcm.assign(sm.data, sm.data + sm.frames);
            const uint32_t fade = 960;                                  // 20 ms fade out: the cut at the end must not click
            for (uint32_t i = 0; i < fade && i < sm.frames; ++i) pcm[sm.frames - 1 - i] = static_cast<int16_t>(pcm[sm.frames - 1 - i] * (static_cast<float>(i) / fade));
            alignas(8) unsigned char ex[0x20] = {};
            *reinterpret_cast<int*>(ex + 0x00) = 0x18;                  // cbsize: only the fields up to 'format' (the game's FMOD takes shorter structs)
            *reinterpret_cast<unsigned*>(ex + 0x04) = sm.frames * 2;    // length in bytes
            *reinterpret_cast<int*>(ex + 0x0C) = 1;                     // numchannels
            *reinterpret_cast<int*>(ex + 0x10) = 48000;                 // defaultfrequency
            *reinterpret_cast<int*>(ex + 0x14) = 2;                     // FMOD_SOUND_FORMAT_PCM16
            void* snd = nullptr;
            const unsigned mode = 0x1 | 0x10 | 0x100 | 0x800 | 0x1000;  // LOOP_OFF | 3D | CREATESAMPLE | OPENMEMORY | OPENRAW
            const int r = reinterpret_cast<CreateSoundFn>(g_fgame + kCreateSound)(sys, pcm.data(), mode, ex, &snd);
            if (log) log("createSound", r);
            if (r != 0 || !snd) { g_fmod_err = r ? r : -1; return false; }
            g_fsound[static_cast<size_t>(idx)] = snd;
        }
        void* ch = nullptr;
        int r = reinterpret_cast<PlaySoundFn>(g_fgame + kPlaySound)(sys, g_fsound[static_cast<size_t>(idx)], nullptr, true, &ch);
        if (log) log("playSound", r);
        if (r != 0 || !ch) return false;
        const float vel[3] = {0, 0, 0};
        r = reinterpret_cast<Set3DFn>(g_fgame + kSet3D)(ch, pos, vel, nullptr);
        if (log) log("set3DAttributes", r);
        const float base = strength >= 3 ? 1.0f : (strength == 2 ? 0.72f : 0.45f);
        reinterpret_cast<SetFloatFn>(g_fgame + kSetVolume)(ch, base * g_volume.load() * static_cast<float>(0.8 + 0.4 * rnd()));
        reinterpret_cast<SetFloatFn>(g_fgame + kSetPitch)(ch, static_cast<float>((strength >= 3 ? 0.92 : 1.0) * (0.88 + 0.24 * rnd())));
        reinterpret_cast<SetPausedFn>(g_fgame + kSetPaused)(ch, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fmod_err = -2;
        return false;
    }
}

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
    const float gain = base * g_volume.load() * dist_g * static_cast<float>(0.8 + 0.4 * rnd());
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
    slot->step = (strength >= 3 ? 0.92 : 1.0) * (0.88 + 0.24 * rnd());
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
