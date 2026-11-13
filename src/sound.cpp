// The pop sounds: FH6's backfire samples (packed into sounds.pak, linked into the DLL) mixed by a small waveOut mixer of our own.
// Every pop picks a random sample (of the chosen engine type, or all of them), with a random pitch, volume and pan.
#include <windows.h>
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
struct Sample { std::string type; const int16_t* data; uint32_t frames; };
std::vector<Sample> g_samples;
std::vector<int> g_pick;                       // indices of the samples in use

struct Voice { const int16_t* data = nullptr; uint32_t frames = 0; double pos = 0, step = 1; float gl = 0, gr = 0; };
constexpr int kVoices = 16, kRate = 44100, kBuf = 512, kBufs = 4;
Voice g_voices[kVoices];
SRWLOCK g_lock = SRWLOCK_INIT;
HWAVEOUT g_wo = nullptr;
WAVEHDR g_hdr[kBufs];
int16_t g_pcm[kBufs][kBuf * 2];
HANDLE g_ev = nullptr, g_thread = nullptr;
std::atomic<bool> g_run{false};
std::atomic<float> g_volume{0.8f};
uint64_t g_rng = 88172645463325252ull;
double rnd() { g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27; return static_cast<double>((g_rng * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0; }

void fill(int16_t* out) {
    float mix[kBuf * 2] = {};
    AcquireSRWLockExclusive(&g_lock);
    for (Voice& v : g_voices) {
        if (!v.data) continue;
        for (int i = 0; i < kBuf; ++i) {
            const uint32_t p = static_cast<uint32_t>(v.pos);
            if (p + 1 >= v.frames) { v.data = nullptr; break; }
            const float f = static_cast<float>(v.pos - p);
            const float s = (static_cast<float>(v.data[p]) * (1.0f - f) + static_cast<float>(v.data[p + 1]) * f) / 32768.0f;
            mix[i * 2] += s * v.gl;
            mix[i * 2 + 1] += s * v.gr;
            v.pos += v.step;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (int i = 0; i < kBuf * 2; ++i) {
        float x = mix[i];
        x = x / (1.0f + std::fabs(x) * 0.6f) * 1.3f;            // soft clip
        if (x > 1.0f) x = 1.0f;
        if (x < -1.0f) x = -1.0f;
        out[i] = static_cast<int16_t>(x * 32760.0f);
    }
}

DWORD WINAPI mixer(LPVOID) {
    while (g_run.load()) {
        WaitForSingleObject(g_ev, 50);
        for (int b = 0; b < kBufs; ++b)
            if (g_hdr[b].dwFlags & WHDR_DONE) { fill(g_pcm[b]); g_hdr[b].dwFlags &= ~WHDR_DONE; waveOutWrite(g_wo, &g_hdr[b], sizeof(WAVEHDR)); }
    }
    return 0;
}
}  // namespace

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
        g_samples.push_back({t, reinterpret_cast<const int16_t*>(p + 16), frames});
        p += 16 + static_cast<size_t>(frames) * 2;
    }
    g_pick.clear();
    const bool all = !type || !*type || _stricmp(type, "All") == 0;
    for (size_t i = 0; i < g_samples.size(); ++i)
        if (all || _stricmp(g_samples[i].type.c_str(), type) == 0) g_pick.push_back(static_cast<int>(i));
    if (g_pick.empty()) for (size_t i = 0; i < g_samples.size(); ++i) g_pick.push_back(static_cast<int>(i));
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

// strength 1 = soft, 2 = normal, 3 = big. rpm_scale shifts the pitch a little (0.9 .. 1.1).
void sound_play(int strength) {
    if (!g_run.load() || g_pick.empty()) return;
    const Sample& s = g_samples[static_cast<size_t>(g_pick[static_cast<size_t>(rnd() * static_cast<double>(g_pick.size())) % g_pick.size()])];
    const float base = strength >= 3 ? 1.0f : (strength == 2 ? 0.72f : 0.45f);
    const float gain = base * g_volume.load() * static_cast<float>(0.8 + 0.4 * rnd());
    const float pan  = static_cast<float>(rnd() * 0.5 - 0.25);
    AcquireSRWLockExclusive(&g_lock);
    Voice* slot = &g_voices[0];
    for (Voice& v : g_voices) if (!v.data) { slot = &v; break; }
    slot->data = s.data;
    slot->frames = s.frames;
    slot->pos = 0;
    slot->step = (strength >= 3 ? 0.92 : 1.0) * (0.88 + 0.24 * rnd());
    slot->gl = gain * (1.0f - pan * 0.5f);
    slot->gr = gain * (1.0f + pan * 0.5f);
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
