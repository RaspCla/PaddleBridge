// =============================================================================
//  PaddleBridge
//  MIDI-Morsepaddle -> Iambic-Keyer -> CW-Ton (WASAPI) + CAT-PTT fuer SDR Console
//
//  Kette:  Paddle -> ESP32-S3 (USB-MIDI) -> PaddleBridge -> VB-Cable -> SDR Console
//                                              |-> Kopfhoerer (Mithoerton, sofort)
//                                              '-> virtueller COM-Port (TX;/RX;)
//
//  Reines Win32 / WASAPI / WinMM, keine Fremdbibliotheken.
//  Einstellungen werden in PaddleBridge.ini neben der exe gespeichert.
//  Erstellt mit Unterstuetzung von Claude (Anthropic).
// =============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601   // Windows 7 oder neuer
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif

#include <windows.h>
#include "resource.h"
#include <commctrl.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <objbase.h>
#include <propsys.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

// ----------------------------------------------------------------- GUIDs (eigene Kopien, compilerunabhaengig)
static const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID kIID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const PROPERTYKEY kPKEY_FriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

static const double kPi = 3.14159265358979323846;

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ============================================================================ Parameter & Zustand

enum { MODE_A = 0, MODE_B = 1, MODE_S = 2 };

// Live-Parameter: koennen waehrend des Betriebs geaendert werden
struct Params {
    std::atomic<int> ditNote{1}, dahNote{2}, speedCC{0};   // speedCC 0 = aus
    std::atomic<bool> swap{false};
    std::atomic<int> mode{MODE_B}, wpm{20}, toneHz{700}, riseMs{5};
    std::atomic<int> txVol{50}, sideVol{30};
    std::atomic<int> leadMs{150}, hangMs{800};
};

// Einstellungen, die beim Start der Engine gelesen werden
struct EngineConfig {
    std::wstring midiName;
    std::wstring txDevId;      // leer = kein Sendesignal
    std::wstring sideDevId;    // leer = aus, "*" = Windows-Standardgeraet
    std::wstring comPort;      // leer = kein CAT
    std::wstring txCmd = L"TX;", rxCmd = L"RX;";
    int latencyMs = 15;
};

struct Shared {
    std::atomic<bool> dit{false}, dah{false}, ditMem{false}, dahMem{false};
    std::atomic<long long> ditT{0}, dahT{0};
    std::atomic<bool> key{false}, busy{false};
    std::atomic<unsigned long long> lastActivity{0};
    std::atomic<bool> catEnabled{false}, tx{false}, txRequest{false};
    std::atomic<bool> running{false};
};

// ============================================================================ Keyer

class Keyer {
public:
    Keyer(Shared& s, Params& p, int r) : sh(s), pr(p), rate(r) {}

    void process(uint8_t* out, int n) {
        if (pr.mode == MODE_S) { straight(out, n); return; }
        std::fill(out, out + n, (uint8_t)0);
        int i = 0;
        while (i < n) {
            if (state == IDLE) {
                int el = decide(true);
                if (el < 0) break;
                begin(el);
            }
            int take = std::min(remain, n - i);
            if (state == ELEMENT) std::fill(out + i, out + i + take, (uint8_t)1);
            watch();
            i += take;
            remain -= take;
            if (remain == 0) advance();
        }
        sh.busy = (state != IDLE);
        if (state != IDLE) sh.lastActivity = GetTickCount64();
    }

private:
    enum { IDLE, ELEMENT, GAP };
    enum { DIT = 0, DAH = 1 };
    Shared& sh;
    Params& pr;
    int rate;
    int state = IDLE, remain = 0, cur = -1, last = -1;
    bool squeeze = false;

    int unit() const {
        int w = std::max(5, pr.wpm.load());
        return std::max(1, (int)std::lround(rate * 1.2 / w));
    }
    void requestTx() {
        sh.lastActivity = GetTickCount64();
        if (sh.catEnabled && !sh.tx) sh.txRequest = true;
    }
    int decide(bool fromIdle) {
        bool pd = sh.dit, ph = sh.dah;
        // Iambic A: Squeeze im letzten Element und jetzt beide Hebel los -> Ende
        if (!fromIdle && pr.mode == MODE_A && squeeze && !pd && !ph) {
            sh.ditMem = false;
            sh.dahMem = false;
            return -1;
        }
        bool d = pd || sh.ditMem;
        bool h = ph || sh.dahMem;
        if (d && h) {
            if (fromIdle) return sh.ditT.load() <= sh.dahT.load() ? DIT : DAH;
            return last == DIT ? DAH : DIT;
        }
        if (d) return DIT;
        if (h) return DAH;
        return -1;
    }
    void watch() {
        bool pd = sh.dit, ph = sh.dah;
        if (pd && ph) squeeze = true;
        if (cur == DIT && ph) sh.dahMem = true;          // Gegenhebel merken (Iambic B)
        else if (cur == DAH && pd) sh.ditMem = true;
    }
    void begin(int el) {
        cur = last = el;
        if (el == DIT) sh.ditMem = false; else sh.dahMem = false;
        squeeze = sh.dit && sh.dah;
        int u = unit();
        state = ELEMENT;
        remain = (el == DIT) ? u : 3 * u;
        requestTx();
    }
    void advance() {
        if (state == ELEMENT) {
            state = GAP;
            remain = unit();
        } else if (state == GAP) {
            int el = decide(false);
            if (el >= 0) begin(el);
            else { state = IDLE; cur = -1; squeeze = false; }
        }
    }
    void straight(uint8_t* out, int n) {
        state = IDLE;
        sh.ditMem = false;
        sh.dahMem = false;
        bool down = sh.dit || sh.dah;
        if (down) requestTx();
        sh.busy = down;
        std::fill(out, out + n, (uint8_t)(down ? 1 : 0));
    }
};

// Verzoegerung des Sendesignals (Zeit fuer die PTT-Umschaltung)
class Delay {
public:
    void process(uint8_t* k, int n, size_t want) {
        if (want != buf.size()) { buf.assign(want, 0); pos = 0; }
        if (buf.empty()) return;
        for (int i = 0; i < n; ++i) {
            uint8_t o = buf[pos];
            buf[pos] = k[i];
            k[i] = o;
            if (++pos == buf.size()) pos = 0;
        }
    }
private:
    std::vector<uint8_t> buf;
    size_t pos = 0;
};

// Sinuston mit Raised-Cosine-Flanken
class Tone {
public:
    explicit Tone(int r) : rate(r) {}
    float next(bool key, double freq, double vol, double riseMs) {
        double step = 1.0 / std::max(1.0, rate * riseMs / 1000.0);
        env += key ? step : -step;
        if (env > 1.0) env = 1.0;
        if (env < 0.0) env = 0.0;
        double shaped = 0.5 - 0.5 * std::cos(kPi * env);
        double s = vol * shaped * std::sin(phase);
        phase += 2.0 * kPi * freq / rate;
        if (phase >= 2.0 * kPi) phase -= 2.0 * kPi;
        return (float)s;
    }
private:
    int rate;
    double phase = 0.0, env = 0.0;
};

// ============================================================================ Geraete-Listen

struct AudioDev { std::wstring id, name; };

static bool LessNoCase(const std::wstring& a, const std::wstring& b) {
    return lstrcmpiW(a.c_str(), b.c_str()) < 0;
}

static std::vector<AudioDev> EnumRenderDevices() {
    std::vector<AudioDev> v;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en)))
        return v;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT cnt = 0;
        col->GetCount(&cnt);
        for (UINT i = 0; i < cnt; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            LPWSTR id = nullptr;
            std::wstring name;
            d->GetId(&id);
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(kPKEY_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
                    name = pv.pwszVal;
                PropVariantClear(&pv);
                ps->Release();
            }
            if (id) {
                v.push_back({id, name.empty() ? std::wstring(id) : name});
                CoTaskMemFree(id);
            }
            d->Release();
        }
        col->Release();
    }
    en->Release();
    std::sort(v.begin(), v.end(), [](const AudioDev& a, const AudioDev& b) { return LessNoCase(a.name, b.name); });
    return v;
}

static std::wstring DefaultRenderId() {
    std::wstring r;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en)))
        return r;
    IMMDevice* d = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id)) && id) { r = id; CoTaskMemFree(id); }
        d->Release();
    }
    en->Release();
    return r;
}

static std::vector<std::wstring> EnumMidiInputs() {
    std::vector<std::wstring> v;
    UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) v.push_back(caps.szPname);
    }
    std::sort(v.begin(), v.end(), LessNoCase);
    return v;
}

static int FindMidiIndex(const std::wstring& name) {
    UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR && name == caps.szPname) return (int)i;
    }
    return -1;
}

static std::vector<std::wstring> EnumComPorts() {
    std::vector<std::wstring> v;
    wchar_t target[1024];
    for (int i = 1; i <= 255; ++i) {
        wchar_t n[16];
        swprintf(n, 16, L"COM%d", i);
        if (QueryDosDeviceW(n, target, 1024)) v.push_back(n);
    }
    return v;
}

static std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back((char)(c < 128 ? c : '?'));
    return s;
}

static std::wstring HrText(const wchar_t* what, HRESULT hr) {
    wchar_t b[256];
    swprintf(b, 256, L"%ls fehlgeschlagen (0x%08lX)", what, (unsigned long)hr);
    return b;
}

// ============================================================================ Engine

enum { ROLE_TX, ROLE_SIDE_MASTER, ROLE_SIDE_MIRROR };

class Engine {
public:
    Shared sh;
    Params pr;
    EngineConfig cfg;

    bool Start(std::wstring& err);
    void Stop();
    bool Running() const { return sh.running; }
    void ForceRx() { sh.lastActivity = 0; }
    void ResetPaddles() { sh.dit = sh.dah = sh.ditMem = sh.dahMem = false; }
    std::wstring TakeError() {
        std::lock_guard<std::mutex> lk(errMx);
        std::wstring e = lastErr;
        lastErr.clear();
        return e;
    }

private:
    HMIDIIN midi = nullptr;
    HANDLE com = INVALID_HANDLE_VALUE;
    std::vector<std::thread> threads;
    std::mutex errMx;
    std::wstring lastErr;

    static void CALLBACK MidiProc(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR);
    void OnMidi(BYTE st, BYTE d1, BYTE d2);
    void Paddle(bool isDit, bool pressed);
    void AudioThread(std::wstring devId, int role);
    void CatThread();
    void WriteCom(const std::wstring& s);
    void SetError(const std::wstring& e) {
        std::lock_guard<std::mutex> lk(errMx);
        lastErr = e;
    }
};

void CALLBACK Engine::MidiProc(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR) {
    if (msg != MIM_DATA) return;
    reinterpret_cast<Engine*>(inst)->OnMidi((BYTE)(p1 & 0xFF), (BYTE)((p1 >> 8) & 0x7F), (BYTE)((p1 >> 16) & 0x7F));
}

void Engine::OnMidi(BYTE st, BYTE d1, BYTE d2) {
    BYTE kind = st & 0xF0;
    if (kind == 0x80 || kind == 0x90) {
        bool pressed = (kind == 0x90 && d2 > 0);
        bool isDit;
        if (d1 == pr.ditNote) isDit = true;
        else if (d1 == pr.dahNote) isDit = false;
        else return;
        if (pr.swap) isDit = !isDit;
        Paddle(isDit, pressed);
    } else if (kind == 0xB0 && pr.speedCC > 0 && d1 == pr.speedCC) {
        pr.wpm = std::clamp((int)d2, 5, 60);
    }
}

void Engine::Paddle(bool isDit, bool pressed) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    if (isDit) {
        sh.dit = pressed;
        if (pressed) { sh.ditMem = true; sh.ditT = c.QuadPart; }
    } else {
        sh.dah = pressed;
        if (pressed) { sh.dahMem = true; sh.dahT = c.QuadPart; }
    }
}

bool Engine::Start(std::wstring& err) {
    if (sh.running) return true;
    ResetPaddles();
    sh.key = false; sh.busy = false; sh.tx = false; sh.txRequest = false; sh.catEnabled = false;
    sh.running = true;

    // --- MIDI
    int idx = FindMidiIndex(cfg.midiName);
    if (idx < 0) {
        err = L"MIDI-Eingang \"" + cfg.midiName + L"\" nicht gefunden.";
        Stop();
        return false;
    }
    MMRESULT r = midiInOpen(&midi, (UINT)idx, reinterpret_cast<DWORD_PTR>(&Engine::MidiProc),
                            reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
        midi = nullptr;
        err = L"MIDI-Eingang konnte nicht ge\u00f6ffnet werden (evtl. von einem anderen Programm belegt).";
        Stop();
        return false;
    }
    midiInStart(midi);

    // --- CAT
    if (!cfg.comPort.empty()) {
        std::wstring path = L"\\\\.\\" + cfg.comPort;
        com = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (com == INVALID_HANDLE_VALUE) {
            err = cfg.comPort + L" konnte nicht ge\u00f6ffnet werden (belegt oder nicht vorhanden).";
            Stop();
            return false;
        }
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        GetCommState(com, &dcb);
        dcb.BaudRate = CBR_9600;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        SetCommState(com, &dcb);
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = MAXDWORD;   // Lesen kehrt sofort zurueck
        to.WriteTotalTimeoutConstant = 200;
        SetCommTimeouts(com, &to);
        sh.catEnabled = true;
        threads.emplace_back(&Engine::CatThread, this);
    }

    // --- Audio
    bool hasTx = !cfg.txDevId.empty();
    bool hasSide = !cfg.sideDevId.empty();
    std::wstring sideId = cfg.sideDevId == L"*" ? DefaultRenderId() : cfg.sideDevId;
    if (hasTx && sideId == cfg.txDevId) hasSide = false;   // gleiches Geraet -> kein doppelter Ton
    if (!hasTx && !hasSide) {
        err = L"Kein Audioger\u00e4t gew\u00e4hlt.";
        Stop();
        return false;
    }
    if (hasTx) threads.emplace_back(&Engine::AudioThread, this, cfg.txDevId, (int)ROLE_TX);
    if (hasSide) threads.emplace_back(&Engine::AudioThread, this, sideId, hasTx ? (int)ROLE_SIDE_MIRROR : (int)ROLE_SIDE_MASTER);
    return true;
}

void Engine::Stop() {
    sh.running = false;
    for (auto& t : threads)
        if (t.joinable()) t.join();
    threads.clear();
    if (com != INVALID_HANDLE_VALUE) {
        if (sh.tx) WriteCom(cfg.rxCmd);
        CloseHandle(com);
        com = INVALID_HANDLE_VALUE;
    }
    sh.tx = false;
    sh.txRequest = false;
    sh.catEnabled = false;
    sh.key = false;
    if (midi) {
        midiInStop(midi);
        midiInReset(midi);
        midiInClose(midi);
        midi = nullptr;
    }
}

void Engine::WriteCom(const std::wstring& s) {
    std::string a = Narrow(s);
    DWORD w = 0;
    WriteFile(com, a.data(), (DWORD)a.size(), &w, nullptr);
}

void Engine::CatThread() {
    char buf[256];
    while (sh.running) {
        unsigned long long now = GetTickCount64();
        unsigned long long la = sh.lastActivity;
        if (sh.txRequest && !sh.tx) {
            WriteCom(cfg.txCmd);
            sh.tx = true;
            sh.txRequest = false;
        } else if (sh.txRequest) {
            sh.txRequest = false;
        } else if (sh.tx && !sh.busy && !sh.key && la <= now &&
                   now - la > (unsigned long long)(pr.hangMs + pr.leadMs)) {
            WriteCom(cfg.rxCmd);
            sh.tx = false;
        }
        DWORD rd = 0;
        ReadFile(com, buf, sizeof(buf), &rd, nullptr);   // Antworten verwerfen
        Sleep(2);
    }
}

static int SampleFormat(const WAVEFORMATEX* wf) {
    WORD tag = wf->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22)
        tag = (WORD)reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf)->SubFormat.Data1;
    if (tag == 3 && wf->wBitsPerSample == 32) return 1;        // float32
    if (tag == 1 && wf->wBitsPerSample == 16) return 2;        // int16
    if (tag == 1 && wf->wBitsPerSample == 32) return 3;        // int32
    return 0;
}

void Engine::AudioThread(std::wstring devId, int role) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);

    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IAudioRenderClient* rc = nullptr;
    WAVEFORMATEX* wf = nullptr;
    const wchar_t* who = role == ROLE_TX ? L"Sendesignal" : L"Mith\u00f6rton";

    do {
        HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en);
        if (FAILED(hr)) { SetError(HrText(L"Audio-Initialisierung", hr)); break; }
        hr = en->GetDevice(devId.c_str(), &dev);
        if (FAILED(hr)) { SetError(std::wstring(who) + L": " + HrText(L"Ger\u00e4t \u00f6ffnen", hr)); break; }
        hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac);
        if (FAILED(hr)) { SetError(std::wstring(who) + L": " + HrText(L"Aktivieren", hr)); break; }
        hr = ac->GetMixFormat(&wf);
        if (FAILED(hr)) { SetError(std::wstring(who) + L": " + HrText(L"Format lesen", hr)); break; }
        int fmt = SampleFormat(wf);
        if (!fmt) { SetError(std::wstring(who) + L": Audioformat wird nicht unterst\u00fctzt."); break; }

        REFERENCE_TIME dur = (REFERENCE_TIME)(cfg.latencyMs * 2 + 40) * 10000;
        hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, dur, 0, wf, nullptr);
        if (FAILED(hr)) { SetError(std::wstring(who) + L": " + HrText(L"Initialisieren", hr)); break; }
        UINT32 bufFrames = 0;
        ac->GetBufferSize(&bufFrames);
        hr = ac->GetService(kIID_IAudioRenderClient, (void**)&rc);
        if (FAILED(hr)) { SetError(std::wstring(who) + L": " + HrText(L"Render-Client", hr)); break; }

        const int rate = (int)wf->nSamplesPerSec;
        const int ch = wf->nChannels;
        UINT32 target = (UINT32)std::max(rate / 1000, rate * cfg.latencyMs / 1000);
        if (target > bufFrames) target = bufFrames;

        Keyer keyer(sh, pr, rate);
        Tone tone(rate);
        Delay delay;
        std::vector<uint8_t> keys;

        ac->Start();
        while (sh.running) {
            UINT32 pad = 0;
            if (FAILED(ac->GetCurrentPadding(&pad))) { SetError(std::wstring(who) + L": Audioger\u00e4t getrennt."); break; }
            if (pad < target) {
                UINT32 n = target - pad;
                BYTE* data = nullptr;
                if (SUCCEEDED(rc->GetBuffer(n, &data))) {
                    keys.resize(n);
                    if (role == ROLE_SIDE_MIRROR) {
                        std::fill(keys.begin(), keys.end(), (uint8_t)(sh.key ? 1 : 0));
                    } else {
                        keyer.process(keys.data(), (int)n);
                        sh.key = keys[n - 1] != 0;      // sofortiger Zustand fuer den Mithoerton
                    }
                    if (role == ROLE_TX)
                        delay.process(keys.data(), (int)n, (size_t)((long long)rate * pr.leadMs / 1000));

                    double vol = (role == ROLE_TX ? pr.txVol : pr.sideVol) / 100.0;
                    double freq = pr.toneHz;
                    double rise = pr.riseMs;
                    for (UINT32 i = 0; i < n; ++i) {
                        float s = tone.next(keys[i] != 0, freq, vol, rise);
                        for (int c = 0; c < ch; ++c) {
                            size_t k = (size_t)i * ch + c;
                            if (fmt == 1) reinterpret_cast<float*>(data)[k] = s;
                            else if (fmt == 2) reinterpret_cast<int16_t*>(data)[k] = (int16_t)(s * 32767.0f);
                            else reinterpret_cast<int32_t*>(data)[k] = (int32_t)(s * 2147483000.0);
                        }
                    }
                    rc->ReleaseBuffer(n, 0);
                }
            }
            Sleep(1);
        }
        ac->Stop();
    } while (false);

    if (role != ROLE_SIDE_MIRROR) sh.key = false;
    SafeRelease(rc);
    SafeRelease(ac);
    SafeRelease(dev);
    SafeRelease(en);
    if (wf) CoTaskMemFree(wf);
    timeEndPeriod(1);
    CoUninitialize();
}

// ============================================================================ Einstellungen (INI)

static std::wstring g_ini;
static const wchar_t* kSec = L"PaddleBridge";

static std::wstring IniGet(const wchar_t* key, const wchar_t* def) {
    wchar_t b[1024];
    GetPrivateProfileStringW(kSec, key, def, b, 1024, g_ini.c_str());
    return b;
}
static int IniGetInt(const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(kSec, key, def, g_ini.c_str()); }
static void IniSet(const wchar_t* key, const std::wstring& v) { WritePrivateProfileStringW(kSec, key, v.c_str(), g_ini.c_str()); }
static void IniSetInt(const wchar_t* key, int v) { IniSet(key, std::to_wstring(v)); }

// ============================================================================ Oberflaeche

enum {
    IDC_MIDI = 101, IDC_MIDI_REFRESH, IDC_DITNOTE, IDC_DAHNOTE, IDC_SWAP, IDC_SPEEDCC,
    IDC_MODE = 110, IDC_WPM, IDC_WPM_LBL, IDC_TONE, IDC_TONE_LBL, IDC_RISE,
    IDC_TXDEV = 120, IDC_TXVOL, IDC_TXVOL_LBL, IDC_SIDEDEV, IDC_SIDEVOL, IDC_SIDEVOL_LBL, IDC_LATENCY, IDC_AUDIO_REFRESH,
    IDC_COM = 130, IDC_COM_REFRESH, IDC_TXCMD, IDC_RXCMD, IDC_LEAD, IDC_HANG,
    IDC_START = 140, IDC_RXNOW, IDC_STATUS,
    IDT_STATUS = 1
};

static Engine g_eng;
static HWND g_wnd = nullptr;
static HFONT g_font = nullptr;
static double g_scale = 1.0;
static bool g_loading = false;
static std::vector<AudioDev> g_devs;
static std::wstring g_lastStatus;

static int S(int v) { return (int)std::lround(v * g_scale); }
static HWND Item(int id) { return GetDlgItem(g_wnd, id); }

static HWND Ctl(const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND hw = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                              (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hw, WM_SETFONT, (WPARAM)g_font, TRUE);
    return hw;
}
static void Label(const wchar_t* t, int x, int y, int w, int id = -1) { Ctl(L"STATIC", t, SS_LEFT, x, y + 4, w, 18, id); }
static void Edit(int x, int y, int w, int id) { Ctl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, 23, id, WS_EX_CLIENTEDGE); }
static void Combo(int x, int y, int w, int id) { Ctl(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, x, y, w, 320, id); }
static void Button(const wchar_t* t, int x, int y, int w, int id, int h = 25) { Ctl(L"BUTTON", t, WS_TABSTOP | BS_PUSHBUTTON, x, y, w, h, id); }
static void Group(const wchar_t* t, int x, int y, int w, int h) { Ctl(L"BUTTON", t, BS_GROUPBOX, x, y, w, h, -1); }
static void Track(int x, int y, int w, int id, int mn, int mx) {
    HWND h = Ctl(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_NOTICKS, x, y, w, 28, id);
    SendMessageW(h, TBM_SETRANGE, TRUE, MAKELPARAM(mn, mx));
}

static void SetText(int id, const std::wstring& t) { SetWindowTextW(Item(id), t.c_str()); }
static std::wstring GetText(int id) {
    wchar_t b[256];
    GetWindowTextW(Item(id), b, 256);
    return b;
}
static int GetInt(int id, int def, int mn, int mx) {
    std::wstring t = GetText(id);
    if (t.empty()) return def;
    return std::clamp(_wtoi(t.c_str()), mn, mx);
}
static int TrackPos(int id) { return (int)SendMessageW(Item(id), TBM_GETPOS, 0, 0); }
static void SetTrack(int id, int v) { SendMessageW(Item(id), TBM_SETPOS, TRUE, v); }
static int ComboSel(int id) { return (int)SendMessageW(Item(id), CB_GETCURSEL, 0, 0); }
static LRESULT ComboData(int id) {
    int s = ComboSel(id);
    return s < 0 ? -1 : SendMessageW(Item(id), CB_GETITEMDATA, s, 0);
}
static int ComboAdd(int id, const std::wstring& t, LRESULT data) {
    int i = (int)SendMessageW(Item(id), CB_ADDSTRING, 0, (LPARAM)t.c_str());
    SendMessageW(Item(id), CB_SETITEMDATA, i, data);
    return i;
}
static std::wstring ComboText(int id) {
    int s = ComboSel(id);
    if (s < 0) return L"";
    wchar_t b[512];
    SendMessageW(Item(id), CB_GETLBTEXT, s, (LPARAM)b);
    return b;
}

static void UpdateLabels() {
    SetText(IDC_WPM_LBL, std::to_wstring(TrackPos(IDC_WPM)) + L" WpM");
    SetText(IDC_TONE_LBL, std::to_wstring(TrackPos(IDC_TONE)) + L" Hz");
    SetText(IDC_TXVOL_LBL, std::to_wstring(TrackPos(IDC_TXVOL)) + L" %");
    SetText(IDC_SIDEVOL_LBL, std::to_wstring(TrackPos(IDC_SIDEVOL)) + L" %");
}

// --- Listen fuellen (Auswahl ueber gespeicherten Namen/ID wiederherstellen)
static void FillMidi(const std::wstring& want) {
    HWND h = Item(IDC_MIDI);
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (auto& n : EnumMidiInputs()) {
        int i = ComboAdd(IDC_MIDI, n, 0);
        if (n == want) sel = i;
    }
    if (sel < 0) {   // Vorschlag: TinyUSB, sonst erster Eintrag
        int cnt = (int)SendMessageW(h, CB_GETCOUNT, 0, 0);
        for (int i = 0; i < cnt && sel < 0; ++i) {
            wchar_t b[256];
            SendMessageW(h, CB_GETLBTEXT, i, (LPARAM)b);
            if (wcsstr(b, L"TinyUSB")) sel = i;
        }
        if (sel < 0 && cnt > 0) sel = 0;
    }
    SendMessageW(h, CB_SETCURSEL, sel, 0);
}

static void FillAudio(const std::wstring& txId, const std::wstring& txName,
                      const std::wstring& sideId, const std::wstring& sideName) {
    g_devs = EnumRenderDevices();
    HWND ht = Item(IDC_TXDEV), hs = Item(IDC_SIDEDEV);
    SendMessageW(ht, CB_RESETCONTENT, 0, 0);
    SendMessageW(hs, CB_RESETCONTENT, 0, 0);
    // Combos sind nicht sortiert (CBS_SORT fehlt) -> Reihenfolge = sortierte Liste
    int tSel = ComboAdd(IDC_TXDEV, L"(kein Sendesignal)", -1);
    int sSel = ComboAdd(IDC_SIDEDEV, L"(aus)", -1);
    int sDef = ComboAdd(IDC_SIDEDEV, L"(Windows-Standardger\u00e4t)", -2);
    if (sideId == L"*") sSel = sDef;
    int tByName = -1, sByName = -1;
    for (size_t k = 0; k < g_devs.size(); ++k) {
        int a = ComboAdd(IDC_TXDEV, g_devs[k].name, (LRESULT)k);
        int b = ComboAdd(IDC_SIDEDEV, g_devs[k].name, (LRESULT)k);
        if (g_devs[k].id == txId) tSel = a;
        if (g_devs[k].name == txName) tByName = a;
        if (g_devs[k].id == sideId) sSel = b;
        if (g_devs[k].name == sideName) sByName = b;
    }
    if (tSel == 0 && tByName >= 0) tSel = tByName;
    if (sSel == 0 && sideId != L"" && sByName >= 0) sSel = sByName;
    SendMessageW(ht, CB_SETCURSEL, tSel, 0);
    SendMessageW(hs, CB_SETCURSEL, sSel, 0);
}

static void FillCom(const std::wstring& want) {
    HWND h = Item(IDC_COM);
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    int sel = ComboAdd(IDC_COM, L"(kein CAT)", 0);
    for (auto& p : EnumComPorts()) {
        int i = ComboAdd(IDC_COM, p, 1);
        if (p == want) sel = i;
    }
    SendMessageW(h, CB_SETCURSEL, sel, 0);
}

// --- GUI -> Parameter
static void ReadLiveParams() {
    Params& p = g_eng.pr;
    p.ditNote = GetInt(IDC_DITNOTE, 1, 0, 127);
    p.dahNote = GetInt(IDC_DAHNOTE, 2, 0, 127);
    p.speedCC = GetInt(IDC_SPEEDCC, 0, 0, 127);
    p.swap = SendMessageW(Item(IDC_SWAP), BM_GETCHECK, 0, 0) == BST_CHECKED;
    p.mode = std::max(0, ComboSel(IDC_MODE));
    p.wpm = TrackPos(IDC_WPM);
    p.toneHz = TrackPos(IDC_TONE);
    p.riseMs = GetInt(IDC_RISE, 5, 1, 20);
    p.txVol = TrackPos(IDC_TXVOL);
    p.sideVol = TrackPos(IDC_SIDEVOL);
    p.leadMs = GetInt(IDC_LEAD, 150, 0, 1000);
    p.hangMs = GetInt(IDC_HANG, 800, 50, 5000);
}

static void ReadEngineConfig() {
    EngineConfig& c = g_eng.cfg;
    c.midiName = ComboText(IDC_MIDI);
    LRESULT t = ComboData(IDC_TXDEV);
    c.txDevId = (t >= 0 && t < (LRESULT)g_devs.size()) ? g_devs[t].id : L"";
    LRESULT s = ComboData(IDC_SIDEDEV);
    c.sideDevId = s == -2 ? L"*" : ((s >= 0 && s < (LRESULT)g_devs.size()) ? g_devs[s].id : L"");
    c.comPort = ComboData(IDC_COM) == 1 ? ComboText(IDC_COM) : L"";
    c.txCmd = GetText(IDC_TXCMD);
    c.rxCmd = GetText(IDC_RXCMD);
    c.latencyMs = GetInt(IDC_LATENCY, 15, 3, 200);
}

static void SaveSettings() {
    ReadLiveParams();
    ReadEngineConfig();
    Params& p = g_eng.pr;
    EngineConfig& c = g_eng.cfg;
    IniSet(L"MidiIn", c.midiName);
    IniSetInt(L"DitNote", p.ditNote);
    IniSetInt(L"DahNote", p.dahNote);
    IniSetInt(L"SpeedCC", p.speedCC);
    IniSetInt(L"Swap", p.swap ? 1 : 0);
    IniSetInt(L"Mode", p.mode);
    IniSetInt(L"Wpm", p.wpm);
    IniSetInt(L"ToneHz", p.toneHz);
    IniSetInt(L"RiseMs", p.riseMs);
    IniSet(L"TxDevId", c.txDevId);
    IniSet(L"TxDevName", c.txDevId.empty() ? L"" : ComboText(IDC_TXDEV));
    IniSetInt(L"TxVol", p.txVol);
    IniSet(L"SideDevId", c.sideDevId);
    IniSet(L"SideDevName", (c.sideDevId.empty() || c.sideDevId == L"*") ? L"" : ComboText(IDC_SIDEDEV));
    IniSetInt(L"SideVol", p.sideVol);
    IniSetInt(L"LatencyMs", c.latencyMs);
    IniSet(L"ComPort", c.comPort);
    IniSet(L"TxCmd", c.txCmd);
    IniSet(L"RxCmd", c.rxCmd);
    IniSetInt(L"LeadMs", p.leadMs);
    IniSetInt(L"HangMs", p.hangMs);
}

static void LoadSettings() {
    g_loading = true;
    FillMidi(IniGet(L"MidiIn", L""));
    SetText(IDC_DITNOTE, std::to_wstring(IniGetInt(L"DitNote", 1)));
    SetText(IDC_DAHNOTE, std::to_wstring(IniGetInt(L"DahNote", 2)));
    SetText(IDC_SPEEDCC, std::to_wstring(IniGetInt(L"SpeedCC", 0)));
    SendMessageW(Item(IDC_SWAP), BM_SETCHECK, IniGetInt(L"Swap", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(Item(IDC_MODE), CB_SETCURSEL, std::clamp(IniGetInt(L"Mode", MODE_B), 0, 2), 0);
    SetTrack(IDC_WPM, std::clamp(IniGetInt(L"Wpm", 20), 5, 60));
    SetTrack(IDC_TONE, std::clamp(IniGetInt(L"ToneHz", 700), 300, 1200));
    SetText(IDC_RISE, std::to_wstring(IniGetInt(L"RiseMs", 5)));
    FillAudio(IniGet(L"TxDevId", L""), IniGet(L"TxDevName", L""),
              IniGet(L"SideDevId", L"*"), IniGet(L"SideDevName", L""));
    SetTrack(IDC_TXVOL, std::clamp(IniGetInt(L"TxVol", 50), 0, 100));
    SetTrack(IDC_SIDEVOL, std::clamp(IniGetInt(L"SideVol", 30), 0, 100));
    SetText(IDC_LATENCY, std::to_wstring(IniGetInt(L"LatencyMs", 15)));
    FillCom(IniGet(L"ComPort", L""));
    SetText(IDC_TXCMD, IniGet(L"TxCmd", L"TX;"));
    SetText(IDC_RXCMD, IniGet(L"RxCmd", L"RX;"));
    SetText(IDC_LEAD, std::to_wstring(IniGetInt(L"LeadMs", 150)));
    SetText(IDC_HANG, std::to_wstring(IniGetInt(L"HangMs", 800)));
    UpdateLabels();
    g_loading = false;
    ReadLiveParams();
}

// --- Start/Stop
static void StartEngine() {
    SaveSettings();
    std::wstring err;
    if (!g_eng.Start(err)) MessageBoxW(g_wnd, err.c_str(), L"PaddleBridge", MB_ICONWARNING);
    SetText(IDC_START, g_eng.Running() ? L"Stop" : L"Start");
}
static void StopEngine() {
    g_eng.Stop();
    SetText(IDC_START, L"Start");
}
static void RestartIfRunning() {
    if (g_eng.Running()) { StopEngine(); StartEngine(); }
}

static void BuildUi() {
    int y = 10;
    Group(L"Paddle (MIDI)", 10, y, 560, 88);
    Label(L"MIDI-Eingang:", 22, y + 22, 100);
    Combo(125, y + 22, 320, IDC_MIDI);
    Button(L"Aktualisieren", 455, y + 21, 105, IDC_MIDI_REFRESH);
    Label(L"Dit-Note:", 22, y + 54, 60);
    Edit(85, y + 54, 40, IDC_DITNOTE);
    Label(L"Dah-Note:", 140, y + 54, 65);
    Edit(207, y + 54, 40, IDC_DAHNOTE);
    Ctl(L"BUTTON", L"Paddles tauschen", WS_TABSTOP | BS_AUTOCHECKBOX, 265, y + 56, 135, 20, IDC_SWAP);
    Label(L"Tempo-CC (0=aus):", 405, y + 54, 115);
    Edit(522, y + 54, 38, IDC_SPEEDCC);

    y = 108;
    Group(L"Keyer", 10, y, 560, 122);
    Label(L"Modus:", 22, y + 22, 60);
    Combo(85, y + 22, 150, IDC_MODE);
    ComboAdd(IDC_MODE, L"Iambic A", MODE_A);
    ComboAdd(IDC_MODE, L"Iambic B", MODE_B);
    ComboAdd(IDC_MODE, L"Handtaste", MODE_S);
    Label(L"Flanke (ms):", 260, y + 22, 85);
    Edit(345, y + 22, 40, IDC_RISE);
    Label(L"Tempo:", 22, y + 56, 60);
    Track(85, y + 54, 380, IDC_WPM, 5, 60);
    Label(L"", 475, y + 56, 85, IDC_WPM_LBL);
    Label(L"Tonh\u00f6he:", 22, y + 88, 60);
    Track(85, y + 86, 380, IDC_TONE, 300, 1200);
    Label(L"", 475, y + 88, 85, IDC_TONE_LBL);

    y = 240;
    Group(L"Audio", 10, y, 560, 186);
    Label(L"Sendesignal:", 22, y + 22, 100);
    Combo(125, y + 22, 435, IDC_TXDEV);
    Label(L"Lautst\u00e4rke:", 22, y + 54, 100);
    Track(125, y + 52, 340, IDC_TXVOL, 0, 100);
    Label(L"", 475, y + 54, 85, IDC_TXVOL_LBL);
    Label(L"Mith\u00f6rton:", 22, y + 88, 100);
    Combo(125, y + 88, 435, IDC_SIDEDEV);
    Label(L"Lautst\u00e4rke:", 22, y + 120, 100);
    Track(125, y + 118, 340, IDC_SIDEVOL, 0, 100);
    Label(L"", 475, y + 120, 85, IDC_SIDEVOL_LBL);
    Label(L"Puffer (ms):", 22, y + 152, 100);
    Edit(125, y + 152, 45, IDC_LATENCY);
    Button(L"Ger\u00e4te aktualisieren", 400, y + 151, 160, IDC_AUDIO_REFRESH);

    y = 436;
    Group(L"PTT / CAT (SDR Console)", 10, y, 560, 120);
    Label(L"COM-Port:", 22, y + 22, 100);
    Combo(125, y + 22, 150, IDC_COM);
    Button(L"Aktualisieren", 285, y + 21, 105, IDC_COM_REFRESH);
    Label(L"TX-Befehl:", 22, y + 54, 100);
    Edit(125, y + 54, 70, IDC_TXCMD);
    Label(L"RX-Befehl:", 215, y + 54, 75);
    Edit(292, y + 54, 70, IDC_RXCMD);
    Label(L"Vorlauf (ms):", 22, y + 86, 100);
    Edit(125, y + 86, 50, IDC_LEAD);
    Label(L"Haltezeit (ms):", 215, y + 86, 95);
    Edit(312, y + 86, 50, IDC_HANG);

    y = 568;
    Button(L"Start", 10, y, 120, IDC_START, 32);
    Button(L"Sofort RX", 140, y, 110, IDC_RXNOW, 32);
    Label(L"Gestoppt", 265, y + 4, 305, IDC_STATUS);
}

static LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_wnd = hw;
        BuildUi();
        LoadSettings();
        SetTimer(hw, IDT_STATUS, 50, nullptr);
        if (IniGetInt(L"Running", 0)) StartEngine();
        return 0;

    case WM_HSCROLL:
        if (!g_loading) { ReadLiveParams(); UpdateLabels(); }
        return 0;

    case WM_COMMAND: {
        if (g_loading) return 0;
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDC_START:
            if (g_eng.Running()) StopEngine(); else StartEngine();
            break;
        case IDC_RXNOW:
            g_eng.ForceRx();
            break;
        case IDC_MIDI_REFRESH:
            g_loading = true; FillMidi(ComboText(IDC_MIDI)); g_loading = false;
            break;
        case IDC_AUDIO_REFRESH:
            ReadEngineConfig();
            g_loading = true;
            FillAudio(g_eng.cfg.txDevId, L"", g_eng.cfg.sideDevId, L"");
            g_loading = false;
            break;
        case IDC_COM_REFRESH:
            g_loading = true; FillCom(ComboText(IDC_COM)); g_loading = false;
            break;
        case IDC_SWAP:
            if (code == BN_CLICKED) { ReadLiveParams(); g_eng.ResetPaddles(); }
            break;
        case IDC_MODE:
            if (code == CBN_SELCHANGE) ReadLiveParams();
            break;
        case IDC_MIDI: case IDC_TXDEV: case IDC_SIDEDEV: case IDC_COM:
            if (code == CBN_SELCHANGE) RestartIfRunning();   // Geraetewechsel -> Engine neu starten
            break;
        case IDC_DITNOTE: case IDC_DAHNOTE: case IDC_SPEEDCC: case IDC_RISE: case IDC_LEAD: case IDC_HANG:
            if (code == EN_CHANGE) ReadLiveParams();
            break;
        case IDC_LATENCY: case IDC_TXCMD: case IDC_RXCMD:
            if (code == EN_KILLFOCUS) RestartIfRunning();
            break;
        }
        return 0;
    }

    case WM_TIMER: {
        std::wstring err = g_eng.TakeError();
        if (!err.empty()) {
            StopEngine();
            MessageBoxW(hw, err.c_str(), L"PaddleBridge", MB_ICONWARNING);
        }
        // Tempo per MIDI-Controller geaendert? -> Schieberegler nachziehen
        if (TrackPos(IDC_WPM) != g_eng.pr.wpm) {
            g_loading = true; SetTrack(IDC_WPM, g_eng.pr.wpm); UpdateLabels(); g_loading = false;
        }
        std::wstring st;
        if (!g_eng.Running()) st = L"Gestoppt";
        else {
            st = g_eng.sh.tx ? L"SENDEN (TX)" : (g_eng.sh.catEnabled ? L"Empfang (RX)" : L"L\u00e4uft (ohne CAT)");
            if (g_eng.sh.key) st += L"   \u25CF";
        }
        if (st != g_lastStatus) { SetText(IDC_STATUS, st); g_lastStatus = st; }
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == Item(IDC_STATUS) && g_eng.Running() && g_eng.sh.tx) {
            SetTextColor((HDC)wp, RGB(200, 0, 0));
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;

    case WM_CLOSE:
        SaveSettings();
        IniSetInt(L"Running", g_eng.Running() ? 1 : 0);
        g_eng.Stop();
        DestroyWindow(hw);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDPIAware();
    HDC dc = GetDC(nullptr);
    g_scale = GetDeviceCaps(dc, LOGPIXELSY) / 96.0;
    ReleaseDC(nullptr, dc);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_ini = path;
    size_t dot = g_ini.find_last_of(L'.');
    if (dot != std::wstring::npos) g_ini = g_ini.substr(0, dot);
    g_ini += L".ini";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"PaddleBridgeWnd";
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, S(580), S(612)};
    AdjustWindowRect(&rc, style, FALSE);
    HWND hw = CreateWindowExW(0, wc.lpszClassName, L"PaddleBridge \u2013 MIDI-Paddle Keyer f\u00fcr SDR Console", style,
                              CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, inst, nullptr);
    ShowWindow(hw, show);
    UpdateWindow(hw);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hw, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    DeleteObject(g_font);
    CoUninitialize();
    return 0;
}
