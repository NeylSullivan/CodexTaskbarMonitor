// ===========================================================================
//  Codex Taskbar Monitor  -  lightweight Windows 10/11 taskbar widget
//
//  Shows Codex (ChatGPT plan) usage: the 5-hour window and the weekly window,
//  the exact time left until each reset (hours + minutes), and raises
//  notifications (with sound) when a limit is exhausted and when it becomes
//  available again.
//
//  Data: https://chatgpt.com/backend-api/wham/usage using the Codex CLI token
//  from %USERPROFILE%\.codex\auth.json, with the local Codex session log as an
//  offline fallback.
//
//  Native Win32 + GDI + WinHTTP. No runtime dependency, ~290 KB exe, ~8 MB RAM.
//
//  Architecture: a hidden "anchor" top-level window owns the tray icon, timers
//  and polling; a separate small window is parented into Shell_TrayWnd and only
//  paints. The widget is recreated automatically after an Explorer restart,
//  which is what makes taskbar hosting reliable on Windows 10.
// ===========================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <utility>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <ctime>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "winmm.lib")

// --------------------------------------------------------------------------
//  Helpers
// --------------------------------------------------------------------------
static std::string W2U8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring U82W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string ReadFileBytes(const std::wstring& path, size_t maxBytes = (8u << 20), long long fromEnd = 0) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) { CloseHandle(h); return {}; }
    long long start = 0, avail = size.QuadPart;
    if (fromEnd > 0 && size.QuadPart > fromEnd) { start = size.QuadPart - fromEnd; avail = fromEnd; }
    if ((size_t)avail > maxBytes) { start += (long long)((size_t)avail - maxBytes); avail = (long long)maxBytes; }
    LARGE_INTEGER pos{}; pos.QuadPart = start;
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { CloseHandle(h); return {}; }
    std::string out((size_t)avail, '\0');
    DWORD got = 0;
    if (!ReadFile(h, &out[0], (DWORD)avail, &got, nullptr)) { CloseHandle(h); return {}; }
    CloseHandle(h);
    out.resize(got);
    return out;
}

static bool WriteAllBytes(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) != 0;
    CloseHandle(h);
    return ok;
}

// --------------------------------------------------------------------------
//  Minimal JSON reader
// --------------------------------------------------------------------------
struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj };
    T t = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* Get(const char* k) const {
        if (t != Obj) return nullptr;
        for (const auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

class JParser {
public:
    JParser(const char* b, const char* e) : p_(b), e_(e) {}
    bool Parse(JVal& v) { skip(); return value(v); }
private:
    const char* p_;
    const char* e_;

    void skip() { while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_; }
    bool lit(const char* s, size_t n) {
        if ((size_t)(e_ - p_) < n || memcmp(p_, s, n) != 0) return false;
        p_ += n; return true;
    }
    bool value(JVal& v) {
        skip();
        if (p_ >= e_) return false;
        char c = *p_;
        if (c == '{') {
            ++p_; v.t = JVal::Obj; skip();
            if (p_ < e_ && *p_ == '}') { ++p_; return true; }
            for (;;) {
                skip();
                if (p_ >= e_ || *p_ != '"') return false;
                std::string key;
                if (!stringLit(key)) return false;
                skip();
                if (p_ >= e_ || *p_ != ':') return false;
                ++p_;
                JVal child;
                if (!value(child)) return false;
                v.obj.emplace_back(std::move(key), std::move(child));
                skip();
                if (p_ >= e_) return false;
                if (*p_ == ',') { ++p_; continue; }
                if (*p_ == '}') { ++p_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++p_; v.t = JVal::Arr; skip();
            if (p_ < e_ && *p_ == ']') { ++p_; return true; }
            for (;;) {
                JVal child;
                if (!value(child)) return false;
                v.arr.push_back(std::move(child));
                skip();
                if (p_ >= e_) return false;
                if (*p_ == ',') { ++p_; continue; }
                if (*p_ == ']') { ++p_; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = JVal::Str; return stringLit(v.str); }
        if (c == 't') { if (lit("true", 4)) { v.t = JVal::Bool; v.b = true; return true; } return false; }
        if (c == 'f') { if (lit("false", 5)) { v.t = JVal::Bool; v.b = false; return true; } return false; }
        if (c == 'n') { if (lit("null", 4)) { v.t = JVal::Null; return true; } return false; }
        const char* st = p_;
        if (p_ < e_ && (*p_ == '-' || *p_ == '+')) ++p_;
        while (p_ < e_ && (isdigit((unsigned char)*p_) || *p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '+' || *p_ == '-')) ++p_;
        if (p_ == st) return false;
        std::string n(st, p_);
        v.t = JVal::Num;
        v.num = atof(n.c_str());
        return true;
    }
    static void AppendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        else { out.push_back((char)(0xF0 | (cp >> 18))); out.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    }
    bool hex4(unsigned& out) {
        if (e_ - p_ < 4) return false;
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        out = v; return true;
    }
    bool stringLit(std::string& out) {
        if (p_ >= e_ || *p_ != '"') return false;
        ++p_; out.clear();
        while (p_ < e_) {
            char c = *p_++;
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (p_ >= e_) return false;
            char esc = *p_++;
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && (e_ - p_) >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                        const char* save = p_;
                        p_ += 2;
                        unsigned lo = 0;
                        if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        else
                            p_ = save;
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: out.push_back(esc); break;
            }
        }
        return false;
    }
};

// --------------------------------------------------------------------------
//  Data model
// --------------------------------------------------------------------------
struct WndInfo {
    bool valid = false;
    double used = 0.0;        // percent used 0..100
    long long resetAt = 0;    // unix seconds
    long long windowSec = 0;  // window length
};

struct Usage {
    bool ok = false;
    WndInfo primary;          // 5-hour window
    WndInfo secondary;        // weekly window
    bool limitReached = false;
    bool stale = false;
    std::wstring source;
    std::wstring error;
    long long fetchedAt = 0;
};

struct Config {
    int  refreshSec = 60;
    bool notify = true;
    bool sound = true;
    bool showWeekly = true;
    int  placement = 0;       // 0 = in taskbar, 1 = overlay, 2 = tray only
    int  monitor = 0;         // index into the taskbar list
    int  displayMode = 0;     // 0 = remaining (default), 1 = used
    int  background = 1;      // 0 = auto (mirror taskbar), 1 = transparent (default), 2 = solid
    bool widgetVisible = true;
};

// --------------------------------------------------------------------------
//  Globals
// --------------------------------------------------------------------------
static const wchar_t* kWidgetClass = L"CodexTaskbarWidget_v1";
static const wchar_t* kAnchorClass = L"CodexTaskbarAnchor_v1";
static const int kWidgetW = 236;

static const UINT WM_TRAY      = WM_APP + 1;
static const UINT WM_REFRESHED = WM_APP + 2;
static const UINT WM_REATTACH  = WM_APP + 3;

static const UINT_PTR TID_TICK = 1;
static const UINT kTrayId = 1;

enum MenuId {
    ID_NOTIFY = 100, ID_SOUND, ID_WEEKLY, ID_REFRESH_NOW, ID_TOGGLE_WIDGET,
    ID_PLACE_TASKBAR, ID_PLACE_OVERLAY, ID_PLACE_TRAY, ID_AUTOSTART,
    ID_INT_30, ID_INT_60, ID_INT_120, ID_INT_300, ID_EXIT,
    ID_MODE_REMAINING, ID_MODE_USED,
    ID_BG_AUTO, ID_BG_TRANSPARENT, ID_BG_SOLID,
    ID_MON_BASE = 300
};

static HINSTANCE g_inst = nullptr;
static HWND g_anchor = nullptr;      // hidden owner of tray icon + timers
static HWND g_widget = nullptr;      // visible widget
static HWND g_taskbar = nullptr;     // taskbar currently hosting the widget
static bool g_attached = false;
static bool g_havePrev = false;
static bool g_prevLockP = false;
static bool g_prevLockS = false;
static int  g_dpi = 96;
static ULONGLONG g_nextPollDue = 0;
static bool g_fetching = false;
static bool g_menuOpen = false;
static int  g_testMode = 0;
static int  g_testPhase = 0;
static ULONGLONG g_testNext = 0;
static std::wstring g_exePath;
static UINT g_msgTaskbarCreated = 0;

static std::mutex g_mu;
static Usage  g_usage;
static Config g_cfg;

static HFONT g_fMain = nullptr, g_fSmall = nullptr, g_fTiny = nullptr;
static HICON g_trayIcon = nullptr;
static bool  g_trayAdded = false;
static int   g_fontDpi = 0;
static int   g_fontQuality = CLEARTYPE_QUALITY;
static int   g_bgEffective = 1;   // runtime background mode after capability fallback

// --------------------------------------------------------------------------
//  Theme
// --------------------------------------------------------------------------
struct Theme {
    bool dark = true;
    COLORREF bg, text, dim, track, green, amber, red;
};

static Theme DetectTheme() {
    Theme t;
    DWORD light = 1, sz = sizeof(light);
    HKEY k = nullptr;
    bool known = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"SystemUsesLightTheme", nullptr, nullptr, (LPBYTE)&light, &sz) == ERROR_SUCCESS)
            known = true;
        RegCloseKey(k);
    }
    t.dark = known ? (light == 0) : false;
    if (t.dark) {
        t.bg    = RGB(0x1F, 0x1F, 0x1F);
        t.text  = RGB(0xF2, 0xF2, 0xF2);
        t.dim   = RGB(0x9A, 0x9A, 0x9A);
        t.track = RGB(0x50, 0x50, 0x50);
    } else {
        t.bg    = RGB(0xF0, 0xF0, 0xF0);
        t.text  = RGB(0x1A, 0x1A, 0x1A);
        t.dim   = RGB(0x60, 0x60, 0x60);
        t.track = RGB(0xC8, 0xC8, 0xC8);
    }
    t.green = RGB(0x3F, 0xA9, 0x4C);
    t.amber = RGB(0xE0, 0x9A, 0x00);
    t.red   = RGB(0xE0, 0x3B, 0x3B);
    return t;
}

// --------------------------------------------------------------------------
//  Config
// --------------------------------------------------------------------------
static std::wstring AppDataDir() {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, 32768);
    std::wstring base = (n > 0 && n < 32768) ? std::wstring(buf, n) : std::wstring(L".");
    std::wstring dir = base + L"\\CodexTaskbar";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static std::wstring ConfigPath() { return AppDataDir() + L"\\config.ini"; }

static void LoadConfig() {
    std::wstring p = ConfigPath();
    g_cfg.refreshSec  = (int)GetPrivateProfileIntW(L"general", L"refresh_sec", 60, p.c_str());
    g_cfg.notify      = GetPrivateProfileIntW(L"general", L"notify", 1, p.c_str()) != 0;
    g_cfg.sound       = GetPrivateProfileIntW(L"general", L"sound", 1, p.c_str()) != 0;
    g_cfg.showWeekly  = GetPrivateProfileIntW(L"general", L"show_weekly", 1, p.c_str()) != 0;
    g_cfg.placement   = (int)GetPrivateProfileIntW(L"general", L"placement", 0, p.c_str());
    g_cfg.monitor     = (int)GetPrivateProfileIntW(L"general", L"monitor", 0, p.c_str());
    g_cfg.displayMode = (int)GetPrivateProfileIntW(L"general", L"display_mode", 0, p.c_str());
    g_cfg.background  = (int)GetPrivateProfileIntW(L"general", L"background", 1, p.c_str());
    g_cfg.widgetVisible = GetPrivateProfileIntW(L"general", L"widget_visible", 1, p.c_str()) != 0;
    if (g_cfg.refreshSec < 15) g_cfg.refreshSec = 15;
    if (g_cfg.placement < 0 || g_cfg.placement > 2) g_cfg.placement = 0;
    if (g_cfg.monitor < 0) g_cfg.monitor = 0;
    if (g_cfg.displayMode < 0 || g_cfg.displayMode > 1) g_cfg.displayMode = 0;
    if (g_cfg.background < 0 || g_cfg.background > 2) g_cfg.background = 1;
}

static void SaveConfig() {
    std::wstring p = ConfigPath();
    wchar_t buf[32];
    auto put = [&](const wchar_t* key, int v) {
        swprintf_s(buf, L"%d", v);
        WritePrivateProfileStringW(L"general", key, buf, p.c_str());
    };
    put(L"refresh_sec", g_cfg.refreshSec);
    put(L"notify", g_cfg.notify ? 1 : 0);
    put(L"sound", g_cfg.sound ? 1 : 0);
    put(L"show_weekly", g_cfg.showWeekly ? 1 : 0);
    put(L"placement", g_cfg.placement);
    put(L"monitor", g_cfg.monitor);
    put(L"display_mode", g_cfg.displayMode);
    put(L"background", g_cfg.background);
    put(L"widget_visible", g_cfg.widgetVisible ? 1 : 0);
}

// --------------------------------------------------------------------------
//  Autostart
// --------------------------------------------------------------------------
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool IsAutostart() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[1024]; DWORD sz = sizeof(buf), type = 0;
    bool found = RegQueryValueExW(k, L"CodexTaskbar", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS;
    RegCloseKey(k);
    return found;
}

static void SetAutostart(bool on) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring cmd = L"\"" + g_exePath + L"\"";
        RegSetValueExW(k, L"CodexTaskbar", 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"CodexTaskbar");
    }
    RegCloseKey(k);
}

// --------------------------------------------------------------------------
//  Sound
// --------------------------------------------------------------------------
static std::vector<BYTE> g_wavLimit;
static std::vector<BYTE> g_wavFree;

static std::vector<BYTE> MakeWav(const std::vector<std::pair<double, int>>& notes) {
    const int rate = 22050;
    const double amp = 0.30;
    size_t total = 0;
    for (const auto& n : notes) total += (size_t)(rate * n.second / 1000.0);
    std::vector<short> pcm(total + rate / 20, 0);
    size_t pos = 0;
    const double fade = 0.004 * rate;
    for (const auto& n : notes) {
        size_t count = (size_t)(rate * n.second / 1000.0);
        for (size_t i = 0; i < count && pos < pcm.size(); ++i, ++pos) {
            double env = 1.0;
            if (i < fade) env = i / fade;
            if (count - i < fade) env = (double)(count - i) / fade;
            double s = sin(2.0 * 3.14159265358979 * n.first * (double)i / rate);
            s += 0.30 * sin(4.0 * 3.14159265358979 * n.first * (double)i / rate);
            pcm[pos] = (short)(s * env * amp * 32767.0 * 0.62);
        }
    }
    std::vector<BYTE> wav;
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) wav.push_back((BYTE)(v >> (8 * i))); };
    auto put16 = [&](uint16_t v) { wav.push_back((BYTE)v); wav.push_back((BYTE)(v >> 8)); };
    uint32_t dataBytes = (uint32_t)(pcm.size() * 2);
    const char riff[] = "RIFF"; wav.insert(wav.end(), riff, riff + 4);
    put32(36 + dataBytes);
    const char wavefmt[] = "WAVEfmt "; wav.insert(wav.end(), wavefmt, wavefmt + 8);
    put32(16); put16(1); put16(1);
    put32(rate); put32(rate * 2); put16(2); put16(16);
    const char dat[] = "data"; wav.insert(wav.end(), dat, dat + 4);
    put32(dataBytes);
    const BYTE* p = (const BYTE*)pcm.data();
    wav.insert(wav.end(), p, p + dataBytes);
    return wav;
}

static void PlayCue(bool limitReached) {
    if (!g_cfg.sound) return;
    if (limitReached) {
        if (g_wavLimit.empty()) g_wavLimit = MakeWav({ { 880, 130 }, { 660, 130 }, { 494, 260 } });
        PlaySoundW((LPCWSTR)g_wavLimit.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    } else {
        if (g_wavFree.empty()) g_wavFree = MakeWav({ { 523, 110 }, { 659, 110 }, { 784, 120 }, { 1047, 200 } });
        PlaySoundW((LPCWSTR)g_wavFree.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
}

// --------------------------------------------------------------------------
//  Tray icon
// --------------------------------------------------------------------------
static HICON MakeTrayIcon(int percent, COLORREF col, bool alert) {
    const int S = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = S;
    bi.bV5Height = -S;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        ReleaseDC(nullptr, screen);
        DeleteDC(dc);
        if (dib) DeleteObject(dib);
        return nullptr;
    }
    memset(bits, 0, (size_t)S * S * 4);
    BYTE* px = (BYTE*)bits;
    const double cx = S / 2.0 - 0.5, cy = S / 2.0 - 0.5;
    const double rOuter = S / 2.0 - 1.0, thickness = 6.5;
    const double rInner = rOuter - thickness;
    const double sweep = std::max(0.0, std::min(100.0, (double)percent)) / 100.0 * 2.0 * 3.14159265358979;
    if (alert) col = RGB(0xE0, 0x3B, 0x3B);
    const COLORREF trackCol = alert ? RGB(0x8E, 0x3A, 0x3A) : RGB(0x78, 0x78, 0x78);

    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            double dx = x - cx, dy = y - cy;
            double d = sqrt(dx * dx + dy * dy);
            double rad = atan2(dy, dx) + 3.14159265358979 / 2.0;
            if (rad < 0) rad += 2.0 * 3.14159265358979;
            double cov = std::min(std::max((rOuter - d) + 0.5, 0.0), 1.0) *
                         std::min(std::max((d - rInner) + 0.5, 0.0), 1.0);
            cov = std::min(cov, 1.0);
            bool inSweep = (percent >= 100) || (rad <= sweep);
            COLORREF c = inSweep ? col : trackCol;
            double a = cov * (inSweep ? 1.0 : 0.5);
            if (a <= 0.003) continue;
            BYTE* o = px + ((size_t)y * S + x) * 4;
            o[0] = (BYTE)(GetBValue(c) * a);
            o[1] = (BYTE)(GetGValue(c) * a);
            o[2] = (BYTE)(GetRValue(c) * a);
            o[3] = (BYTE)(a * 255.0);
        }
    }
    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            double dx = x - cx, dy = y - cy;
            double d = sqrt(dx * dx + dy * dy);
            double a = std::min(std::max((3.5 - d) + 0.5, 0.0), 1.0);
            if (a <= 0.003) continue;
            BYTE* o = px + ((size_t)y * S + x) * 4;
            if (a <= o[3] / 255.0) continue;
            o[0] = (BYTE)(GetBValue(col) * a);
            o[1] = (BYTE)(GetGValue(col) * a);
            o[2] = (BYTE)(GetRValue(col) * a);
            o[3] = (BYTE)(a * 255.0);
        }
    }
    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = dib;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(dib);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}

// --------------------------------------------------------------------------
//  Codex data
// --------------------------------------------------------------------------
static std::wstring CodexHome() {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(L"CODEX_HOME", buf, 32768);
    if (n > 0 && n < 32768) {
        std::wstring s(buf, n);
        if (!s.empty()) return s;
    }
    n = GetEnvironmentVariableW(L"USERPROFILE", buf, 32768);
    if (n > 0 && n < 32768) return std::wstring(buf, n) + L"\\.codex";
    return L"";
}

struct Creds {
    bool ok = false;
    std::wstring token;
    std::wstring accountId;
};

static Creds LoadCreds() {
    Creds c;
    std::wstring home = CodexHome();
    if (home.empty()) return c;
    std::string raw = ReadFileBytes(home + L"\\auth.json", 1u << 20);
    if (raw.empty()) return c;
    JVal v;
    JParser p(raw.c_str(), raw.c_str() + raw.size());
    if (!p.Parse(v)) return c;
    const JVal* tokens = v.Get("tokens");
    if (!tokens) return c;
    const JVal* at = tokens->Get("access_token");
    if (!at || at->t != JVal::Str || at->str.empty()) return c;
    c.token = U82W(at->str);
    if (const JVal* acc = tokens->Get("account_id"))
        if (acc->t == JVal::Str) c.accountId = U82W(acc->str);
    if (c.accountId.empty())
        if (const JVal* acc = tokens->Get("chatgpt_account_id"))
            if (acc->t == JVal::Str) c.accountId = U82W(acc->str);
    c.ok = !c.token.empty();
    return c;
}

struct HttpResp {
    DWORD status = 0;
    std::string body;
    std::wstring error;
};

static HttpResp HttpGet(const std::wstring& host, const std::wstring& path,
                        const std::vector<std::pair<std::wstring, std::wstring>>& headers) {
    HttpResp out;
    HINTERNET ses = WinHttpOpen(L"CodexTaskbar/1.0 (Windows)",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        ses = WinHttpOpen(L"CodexTaskbar/1.0 (Windows)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { out.error = L"WinHttpOpen failed"; return out; }
    WinHttpSetTimeouts(ses, 8000, 8000, 12000, 20000);

    HINTERNET con = WinHttpConnect(ses, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) { out.error = L"No connection"; WinHttpCloseHandle(ses); return out; }

    HINTERNET req = WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) { out.error = L"Request failed"; WinHttpCloseHandle(con); WinHttpCloseHandle(ses); return out; }

    for (const auto& h : headers) {
        std::wstring line = h.first + L": " + h.second;
        WinHttpAddRequestHeaders(req, line.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        wchar_t buf[128];
        swprintf_s(buf, L"Network error (%lu)", GetLastError());
        out.error = buf;
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        return out;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    out.status = status;

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::string chunk((size_t)avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(req, &chunk[0], avail, &got) || got == 0) break;
        chunk.resize(got);
        out.body += chunk;
        if (out.body.size() > (2u << 20)) break;
    }
    WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
    return out;
}

static void FillWindow(const JVal* w, WndInfo& out, long long defaultWindowSec, bool minutesField) {
    if (!w || w->t != JVal::Obj) return;
    const JVal* up = w->Get("used_percent");
    const JVal* ra = w->Get("reset_at");
    if (!up || up->t != JVal::Num || !ra || ra->t != JVal::Num) return;
    out.valid = true;
    out.used = up->num;
    out.resetAt = (long long)ra->num;
    out.windowSec = defaultWindowSec;
    if (minutesField) {
        if (const JVal* m = w->Get("window_minutes"))
            if (m->t == JVal::Num) out.windowSec = (long long)(m->num * 60.0);
    } else {
        if (const JVal* s = w->Get("limit_window_seconds"))
            if (s->t == JVal::Num) out.windowSec = (long long)s->num;
    }
}

static std::wstring NewestRollout() {
    std::wstring root = CodexHome() + L"\\sessions";
    struct Found { std::wstring path; ULONGLONG time = 0; } best;
    std::vector<std::pair<std::wstring, int>> stack{ { root, 0 } };
    while (!stack.empty()) {
        auto item = stack.back();
        stack.pop_back();
        if (item.second > 6) continue;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((item.first + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring full = item.first + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                stack.push_back({ full, item.second + 1 });
            } else if (!wcsncmp(fd.cFileName, L"rollout-", 8)) {
                ULARGE_INTEGER t{};
                t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
                t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                if (t.QuadPart > best.time) { best.time = t.QuadPart; best.path = full; }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return best.path;
}

static bool LoadFromRollout(Usage& u) {
    std::wstring file = NewestRollout();
    if (file.empty()) return false;
    std::string tail = ReadFileBytes(file, 1u << 19, 1u << 19);
    if (tail.empty()) return false;
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= tail.size(); ++i) {
        if (i == tail.size() || tail[i] == '\n') {
            if (i > start) lines.emplace_back(tail, start, i - start);
            start = i + 1;
        }
    }
    for (size_t idx = lines.size(); idx-- > 0;) {
        const std::string& line = lines[idx];
        if (line.find("rate_limits") == std::string::npos) continue;
        JVal v;
        JParser p(line.c_str(), line.c_str() + line.size());
        if (!p.Parse(v)) continue;
        const JVal* payload = v.Get("payload");
        if (!payload) continue;
        const JVal* rl = payload->Get("rate_limits");
        if (!rl || rl->t != JVal::Obj) continue;
        WndInfo pri, sec;
        FillWindow(rl->Get("primary"), pri, 18000, true);
        FillWindow(rl->Get("secondary"), sec, 604800, true);
        if (!pri.valid && !sec.valid) continue;
        u.primary = pri;
        u.secondary = sec;
        u.stale = true;
        u.source = L"Codex session log";
        u.ok = true;
        u.fetchedAt = (long long)time(nullptr);
        return true;
    }
    return false;
}

static Usage FetchUsage() {
    Usage u;
    u.fetchedAt = (long long)time(nullptr);
    Creds c = LoadCreds();
    if (c.ok) {
        std::vector<std::pair<std::wstring, std::wstring>> h;
        h.emplace_back(L"Authorization", L"Bearer " + c.token);
        h.emplace_back(L"User-Agent", L"codex-cli");
        h.emplace_back(L"Accept", L"application/json");
        if (!c.accountId.empty()) h.emplace_back(L"ChatGPT-Account-Id", c.accountId);
        HttpResp r = HttpGet(L"chatgpt.com", L"/backend-api/wham/usage", h);
        if (r.status == 200 && !r.body.empty()) {
            JVal root;
            JParser p(r.body.c_str(), r.body.c_str() + r.body.size());
            if (p.Parse(root)) {
                if (const JVal* rl = root.Get("rate_limit")) {
                    FillWindow(rl->Get("primary_window"), u.primary, 18000, false);
                    FillWindow(rl->Get("secondary_window"), u.secondary, 604800, false);
                    if (const JVal* lr = rl->Get("limit_reached"))
                        u.limitReached = (lr->t == JVal::Bool && lr->b);
                }
                if (u.primary.valid || u.secondary.valid) {
                    u.ok = true;
                    u.source = L"API Codex";
                    u.fetchedAt = (long long)time(nullptr);
                    return u;
                }
                u.error = L"API returned no limit data";
            } else {
                u.error = L"Could not parse API response";
            }
        } else if (r.status == 401 || r.status == 403) {
            u.error = L"Sign in to Codex CLI";
        } else if (r.status == 0) {
            u.error = r.error.empty() ? L"No network" : r.error;
        } else {
            wchar_t buf[128];
            swprintf_s(buf, L"HTTP %lu", r.status);
            u.error = buf;
        }
    } else {
        u.error = L"auth.json not found";
    }

    Usage fb;
    if (LoadFromRollout(fb)) {
        fb.error = u.error;
        return fb;
    }
    return u;
}

// --------------------------------------------------------------------------
//  Formatting
// --------------------------------------------------------------------------
static std::wstring FormatRemaining(long long secs, bool longWindow) {
    wchar_t b[64];
    if (secs <= 0) return L"0m";
    long long h = secs / 3600, m = (secs % 3600) / 60;
    if (longWindow) {
        long long d = h / 24;
        if (d > 0) { swprintf_s(b, L"%lldd %lldh", d, h % 24); return b; }
        if (h > 0) { swprintf_s(b, L"%lldh %lldm", h, m); return b; }
        swprintf_s(b, L"%lldm", m);
        return b;
    }
    if (h > 0) { swprintf_s(b, L"%lldh %02lldm", h, m); return b; }
    if (m > 0) { swprintf_s(b, L"%lldm", m); return b; }
    return L"<1m";
}

static std::wstring PercentText(double pct) {
    wchar_t b[32];
    swprintf_s(b, L"%d%%", (int)llround(std::max(0.0, std::min(100.0, pct))));
    return b;
}

static void EnsureFonts(int dpi, int quality = CLEARTYPE_QUALITY) {
    if (g_fontDpi == dpi && g_fontQuality == quality && g_fMain) return;
    if (g_fMain) DeleteObject(g_fMain);
    if (g_fSmall) DeleteObject(g_fSmall);
    if (g_fTiny) DeleteObject(g_fTiny);
    auto mk = [&](int px, int weight) {
        return CreateFontW(-MulDiv(px, dpi, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           quality, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };
    g_fMain  = mk(13, FW_SEMIBOLD);
    g_fSmall = mk(12, FW_NORMAL);
    g_fTiny  = mk(11, FW_NORMAL);
    g_fontDpi = dpi;
    g_fontQuality = quality;
}

static void DrawTextAt(HDC dc, const std::wstring& s, int x, int y, int w, int h,
                       HFONT f, COLORREF col, UINT fmt) {
    if (w <= 0 || h <= 0) return;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, col);
    HGDIOBJ old = SelectObject(dc, f);
    RECT r{ x, y, x + w, y + h };
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
    SelectObject(dc, old);
}

static void DrawBar(HDC dc, int x, int y, int w, int h, double pct, COLORREF fill, COLORREF track) {
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HBRUSH b = CreateSolidBrush(track);
    SelectObject(dc, b);
    RoundRect(dc, x, y, x + w, y + h, h, h);
    SelectObject(dc, oldBrush);
    DeleteObject(b);
    int fw = (int)lround(w * std::max(0.0, std::min(100.0, pct)) / 100.0);
    if (fw > 0) {
        if (fw < h) fw = h;
        HBRUSH fb = CreateSolidBrush(fill);
        SelectObject(dc, fb);
        RoundRect(dc, x, y, x + fw, y + h, h, h);
        SelectObject(dc, oldBrush);
        DeleteObject(fb);
    }
    SelectObject(dc, oldPen);
}

// --------------------------------------------------------------------------
//  Widget painting
// --------------------------------------------------------------------------
// Background options: 0 = auto (mirror the taskbar pixels), 1 = transparent
// (color key + coverage mask), 2 = solid theme color.
static const COLORREF kKeyColor = RGB(0xFF, 0x00, 0xFF);

struct BgProfile {
    COLORREF rows[192];
    int count = 0;
    bool valid = false;
};
static BgProfile g_bgProfile;

// Copy the real taskbar colours row by row, so even the 1px highlight line at
// the top of the bar and any acrylic tint continue seamlessly through the widget.
//
// The strip is captured with BitBlt instead of GetPixel: the raw screen DC
// reports pre-composition colours, so an acrylic taskbar would come back much
// lighter than what the user actually sees.
static void SampleBackground(const RECT& box) {
    g_bgProfile.valid = false;
    if (g_bgEffective >= 2) return;   // needed by both auto and transparent modes
    const int h = box.bottom - box.top;
    if (h <= 1 || h > (int)(sizeof(g_bgProfile.rows) / sizeof(g_bgProfile.rows[0]))) return;

    const int gap = MulDiv(6, g_dpi > 0 ? g_dpi : 96, 96);
    const int probeW = MulDiv(16, g_dpi > 0 ? g_dpi : 96, 96);
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int stripX = box.left - gap - probeW;
    if (stripX < 0) stripX = box.right + gap;
    if (stripX < 0 || stripX + probeW > screenW) return;

    HDC screen = GetDC(nullptr);
    if (!screen) return;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, probeW, h);
    if (!bmp) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old = SelectObject(mem, bmp);

    // Capture the whole strip in one go (CAPTUREBLT makes layered windows show up).
    BitBlt(mem, 0, 0, probeW, h, screen, stripX, box.top, SRCCOPY | CAPTUREBLT);

    for (int y = 0; y < h; ++y) {
        int r = 0, g = 0, b = 0, n = 0;
        for (int x = 0; x < probeW; ++x) {
            COLORREF c = GetPixel(mem, x, y);
            if (c == CLR_INVALID) continue;
            r += GetRValue(c); g += GetGValue(c); b += GetBValue(c); ++n;
        }
        if (n == 0) {
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            ReleaseDC(nullptr, screen);
            return;
        }
        g_bgProfile.rows[y] = RGB(r / n, g / n, b / n);
    }
    g_bgProfile.count = h;
    g_bgProfile.valid = true;

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static void FillBackground(HDC dc, int w, int h, const Theme& th) {
    RECT full{ 0, 0, w, h };
    if (g_bgEffective <= 1 && g_bgProfile.valid && g_bgProfile.count >= h) {
        int y = 0;
        while (y < h) {
            int y2 = y;
            while (y2 + 1 < h && g_bgProfile.rows[y2 + 1] == g_bgProfile.rows[y]) ++y2;
            HBRUSH b = CreateSolidBrush(g_bgProfile.rows[y]);
            RECT r{ 0, y, w, y2 + 1 };
            FillRect(dc, &r, b);
            DeleteObject(b);
            y = y2 + 1;
        }
        return;
    }
    HBRUSH bg = CreateSolidBrush(th.bg);
    FillRect(dc, &full, bg);
    DeleteObject(bg);
}

// Draws only the content; the caller has already filled the background.
static void DrawContent(HDC mem, int w, int h, const Usage& u, const Theme& th) {
    const int rows = g_cfg.showWeekly ? 2 : 1;
    const int rowH = h / rows;
    auto px = [&](int v) { return MulDiv(v, g_dpi, 96); };

    auto accentFor = [&](double pct) -> COLORREF {
        if (pct >= 99.5) return th.red;
        if (pct >= 70.0) return th.amber;
        return th.green;
    };

    if (!u.ok) {
        std::wstring msg = u.error.empty() ? L"no Codex data" : u.error;
        int x = px(8);
        HBRUSH db = CreateSolidBrush(th.dim);
        HGDIOBJ op = SelectObject(mem, GetStockObject(NULL_PEN));
        HGDIOBJ ob = SelectObject(mem, db);
        int cy = h / 2, d = px(6);
        Ellipse(mem, x, cy - d / 2, x + d, cy + d / 2);
        SelectObject(mem, ob);
        SelectObject(mem, op);
        DeleteObject(db);
        DrawTextAt(mem, msg, x + px(12), 0, w - x - px(16), h, g_fSmall, th.dim,
                   DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        return;
    }

    auto drawRow = [&](int rowIdx, const std::wstring& label, const WndInfo& wi, bool longWindow) {
        const int top = rowIdx * rowH;
        const int cy = top + rowH / 2;
        int x = px(8);
        long long now = (long long)time(nullptr);
        double usedPct = wi.valid ? wi.used : 0.0;
        long long remain = wi.valid && wi.resetAt > 0 ? wi.resetAt - now : 0;
        bool resetPassed = wi.valid && remain <= 0;
        if (resetPassed) usedPct = 0.0;   // window already rolled over server-side
        usedPct = std::max(0.0, std::min(100.0, usedPct));
        bool locked = wi.valid && usedPct >= 99.5 && !resetPassed;
        COLORREF accent = wi.valid ? accentFor(usedPct) : th.dim;
        // "Remaining" is the default view: a fresh window reads 100% and drains.
        double shown = (g_cfg.displayMode == 0) ? (100.0 - usedPct) : usedPct;

        HBRUSH db = CreateSolidBrush(accent);
        HGDIOBJ op = SelectObject(mem, GetStockObject(NULL_PEN));
        HGDIOBJ ob = SelectObject(mem, db);
        int d = px(7);
        Ellipse(mem, x, cy - d / 2, x + d, cy + d / 2);
        SelectObject(mem, ob);
        SelectObject(mem, op);
        DeleteObject(db);

        int tx = x + px(13);
        DrawTextAt(mem, label, tx, top, px(26), rowH, g_fTiny, th.dim, DT_LEFT | DT_VCENTER);

        int barX = tx + px(26);
        int barW = px(56);
        int barH = px(rows > 1 ? 6 : 8);
        DrawBar(mem, barX, cy - barH / 2, barW, barH, shown, accent, th.track);

        int pctX = barX + barW + px(7);
        DrawTextAt(mem, wi.valid ? PercentText(shown) : L"—", pctX, top, px(44), rowH,
                   g_fMain, locked ? th.red : th.text, DT_LEFT | DT_VCENTER);

        int timeX = pctX + px(42);
        int timeW = w - timeX - px(6);
        std::wstring timeText;
        COLORREF timeColor = th.dim;
        if (!wi.valid) {
            timeText = L"—";
        } else if (remain > 0) {
            timeText = FormatRemaining(remain, longWindow);
            timeColor = locked ? th.red : th.dim;
        } else {
            timeText = L"reset";
            timeColor = accent;
        }
        DrawTextAt(mem, timeText, timeX, top, timeW, rowH,
                   locked ? g_fMain : g_fSmall, timeColor, DT_RIGHT | DT_VCENTER);
    };

    int hours = u.primary.windowSec > 0 ? (int)(u.primary.windowSec / 3600) : 5;
    wchar_t lbl[16];
    swprintf_s(lbl, L"%dh", hours);
    drawRow(0, lbl, u.primary, false);

    if (g_cfg.showWeekly) {
        long long days = u.secondary.windowSec > 0 ? u.secondary.windowSec / 86400 : 7;
        std::wstring wl = days >= 7 ? std::wstring(L"wk") : (std::to_wstring(days) + L"d");
        drawRow(1, wl, u.secondary, true);
    }
}

// Renders the widget: opaque auto-sampled background, transparent color key,
// or a flat theme colour.
static void PaintWidget(HDC target, const RECT& rc, const Usage& u) {
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    Theme th = DetectTheme();
    const bool transparent = (g_bgEffective == 1);
    if (transparent) th.bg = kKeyColor;   // becomes see-through via LWA_COLORKEY
    EnsureFonts(g_dpi, transparent ? ANTIALIASED_QUALITY : CLEARTYPE_QUALITY);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // top-down, so row 0 is the top row
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(target);
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(mem, dib);

    FillBackground(mem, w, h, th);
    DrawContent(mem, w, h, u, th);

    if (transparent) {
        // A second pass renders the same content as white-on-black; its
        // luminance is the ink coverage. Pixels below the threshold turn into
        // the key colour and therefore let the real taskbar show through,
        // while antialiased edges keep the colour they were blended with.
        void* maskBits = nullptr;
        HDC mem2 = CreateCompatibleDC(target);
        HBITMAP dib2 = CreateDIBSection(mem2, &bi, DIB_RGB_COLORS, &maskBits, nullptr, 0);
        if (dib2 && maskBits) {
            HGDIOBJ old2 = SelectObject(mem2, dib2);
            Theme mono;
            mono.bg = RGB(0, 0, 0);
            mono.text = mono.dim = mono.track = RGB(255, 255, 255);
            mono.green = mono.amber = mono.red = RGB(255, 255, 255);
            HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
            RECT full{ 0, 0, w, h };
            FillRect(mem2, &full, black);
            DeleteObject(black);
            DrawContent(mem2, w, h, u, mono);

            BYTE* px = (BYTE*)bits;
            const BYTE* mp = (const BYTE*)maskBits;
            const int total = w * h;
            for (int i = 0; i < total; ++i) {
                const BYTE* m = mp + (size_t)i * 4;
                int lum = (m[2] * 299 + m[1] * 587 + m[0] * 114) / 1000;
                if (lum < 48) {
                    BYTE* p = px + (size_t)i * 4;
                    p[0] = GetBValue(kKeyColor);
                    p[1] = GetGValue(kKeyColor);
                    p[2] = GetRValue(kKeyColor);
                }
            }
            SelectObject(mem2, old2);
        }
        if (dib2) DeleteObject(dib2);
        DeleteDC(mem2);
    }

    // A 32bpp DIB leaves the alpha channel at zero, and the shell composites a
    // child of the taskbar with per-pixel alpha - which made the widget twice as
    // bright. Force fully opaque alpha; the colour key handles transparency.
    {
        BYTE* ap = (BYTE*)bits;
        const int total = w * h;
        for (int i = 0; i < total; ++i) ap[(size_t)i * 4 + 3] = 0xFF;
    }

    BitBlt(target, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(dib);
    DeleteDC(mem);
}

// --------------------------------------------------------------------------
//  Taskbars & placement
// --------------------------------------------------------------------------
struct TaskbarInfo {
    HWND hwnd = nullptr;
    HMONITOR mon = nullptr;
    RECT rect{};
};

static BOOL CALLBACK EnumTaskbarProc(HWND h, LPARAM p) {
    wchar_t cls[64] = { 0 };
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"Shell_TrayWnd") && wcscmp(cls, L"Shell_SecondaryTrayWnd")) return TRUE;
    TaskbarInfo ti;
    ti.hwnd = h;
    ti.mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    GetWindowRect(h, &ti.rect);
    auto* list = (std::vector<TaskbarInfo>*)p;
    list->push_back(ti);
    return TRUE;
}

static std::vector<TaskbarInfo> FindTaskbars() {
    std::vector<TaskbarInfo> list;
    EnumWindows(EnumTaskbarProc, (LPARAM)&list);
    std::sort(list.begin(), list.end(), [](const TaskbarInfo& a, const TaskbarInfo& b) {
        bool ap = false, bp = false;
        wchar_t cls[64] = { 0 };
        GetClassNameW(a.hwnd, cls, 64);
        ap = !wcscmp(cls, L"Shell_TrayWnd");
        GetClassNameW(b.hwnd, cls, 64);
        bp = !wcscmp(cls, L"Shell_TrayWnd");
        if (ap != bp) return ap;
        if (a.rect.top != b.rect.top) return a.rect.top < b.rect.top;
        return a.rect.left < b.rect.left;
    });
    return list;
}

static HWND CurrentTaskbar() {
    auto list = FindTaskbars();
    if (list.empty()) return nullptr;
    int idx = std::max(0, std::min((int)list.size() - 1, g_cfg.monitor));
    return list[idx].hwnd;
}

// Screen-space rectangle for the widget.
static RECT TargetRect(HWND tb) {
    RECT out{ 0, 0, 0, 0 };
    int dpi = 96;
    RECT tbr{};
    if (tb) {
        GetWindowRect(tb, &tbr);
        UINT d = GetDpiForWindow(tb);
        if (d >= 96 && d <= 480) dpi = (int)d;
    } else {
        tbr = RECT{ 0, GetSystemMetrics(SM_CYSCREEN) - 40, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        HDC dc = GetDC(nullptr);
        int dp = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
        if (dp > 0) dpi = dp;
    }
    int h = tbr.bottom - tbr.top;
    if (h < MulDiv(24, dpi, 96)) h = MulDiv(40, dpi, 96);
    if (h > MulDiv(80, dpi, 96)) {
        // Vertical taskbar: park a compact block at its bottom end.
        out.left = tbr.left;
        out.right = tbr.right;
        out.bottom = tbr.bottom;
        out.top = tbr.bottom - MulDiv(84, dpi, 96);
        return out;
    }
    int w = MulDiv(kWidgetW, dpi, 96);

    RECT tray = tbr;
    if (tb) {
        HWND trayWnd = FindWindowExW(tb, nullptr, L"TrayNotifyWnd", nullptr);
        if (trayWnd) GetWindowRect(trayWnd, &tray);
        else tray.left = tbr.right - MulDiv(180, dpi, 96);
    }
    int x = tray.left - w - MulDiv(6, dpi, 96);
    if (x < tbr.left + MulDiv(4, dpi, 96)) x = tbr.left + MulDiv(4, dpi, 96);
    out.left = x;
    out.right = x + w;
    out.top = tbr.top;
    out.bottom = tbr.top + h;
    return out;
}

static void DestroyWidget() {
    if (g_widget) {
        HWND w = g_widget;
        g_widget = nullptr;
        DestroyWindow(w);
    }
    g_attached = false;
    g_taskbar = nullptr;
}

static void CreateWidget() {
    if (g_widget && IsWindow(g_widget)) return;
    g_widget = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWidgetClass, L"",
                               WS_POPUP, 0, 0, MulDiv(kWidgetW, 96, 96), 40,
                               nullptr, nullptr, g_inst, nullptr);
}

static void ApplyBackgroundStyle() {
    if (!g_widget) return;
    g_bgEffective = g_cfg.background;
    LONG_PTR ex = GetWindowLongPtrW(g_widget, GWL_EXSTYLE);
    if (g_cfg.background == 1) {
        SetWindowLongPtrW(g_widget, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        if (!SetLayeredWindowAttributes(g_widget, kKeyColor, 0, LWA_COLORKEY)) {
            // Layered child windows are unsupported here (or blocked by policy):
            // fall back to mirroring the taskbar pixels.
            g_bgEffective = 0;
            LONG_PTR now = GetWindowLongPtrW(g_widget, GWL_EXSTYLE);
            SetWindowLongPtrW(g_widget, GWL_EXSTYLE, now & ~WS_EX_LAYERED);
        }
    } else if (ex & WS_EX_LAYERED) {
        SetWindowLongPtrW(g_widget, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    }
}

static void ApplyPlacement() {
    if (!g_cfg.widgetVisible || g_cfg.placement == 2) {
        DestroyWidget();
        return;
    }
    HWND tb = CurrentTaskbar();
    if (!tb && g_cfg.placement == 0) {
        DestroyWidget();
        return;
    }
    CreateWidget();
    if (!g_widget) return;

    RECT target = TargetRect(tb);
    UINT dpi = tb ? GetDpiForWindow(tb) : 96;
    g_dpi = (dpi >= 96 && dpi <= 480) ? (int)dpi : 96;

    if (g_cfg.placement == 0 && tb) {
        if (GetParent(g_widget) != tb) {
            SetWindowLongPtrW(g_widget, GWL_STYLE, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
            SetWindowLongPtrW(g_widget, GWL_EXSTYLE, WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
            SetParent(g_widget, tb);
        }
        g_taskbar = tb;
        g_attached = true;
        POINT pts[2] = { { target.left, target.top }, { target.right, target.bottom } };
        MapWindowPoints(HWND_DESKTOP, tb, pts, 2);
        SetWindowPos(g_widget, HWND_TOP, pts[0].x, pts[0].y,
                     pts[1].x - pts[0].x, pts[1].y - pts[0].y,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        if (GetParent(g_widget)) SetParent(g_widget, nullptr);
        SetWindowLongPtrW(g_widget, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(g_widget, GWL_EXSTYLE, WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
        g_attached = false;
        g_taskbar = tb;
        SetWindowPos(g_widget, HWND_TOPMOST, target.left, target.top,
                     target.right - target.left, target.bottom - target.top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    ApplyBackgroundStyle();
    RECT wr{};
    GetWindowRect(g_widget, &wr);
    SampleBackground(wr);
    InvalidateRect(g_widget, nullptr, TRUE);
}

static bool ForegroundIsFullscreen() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_widget || fg == g_anchor) return false;
    wchar_t cls[64] = { 0 };
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Progman") ||
        !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
        return false;
    RECT r{};
    if (!GetWindowRect(fg, &r)) return false;
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

// Keep the widget alive, correctly sized and on top of the taskbar.
static void WatchdogTick() {
    if (!g_cfg.widgetVisible || g_cfg.placement == 2) return;

    HWND tb = CurrentTaskbar();
    if (g_cfg.placement == 0) {
        if (!tb) { if (g_widget && IsWindow(g_widget)) DestroyWidget(); return; }
        if (!g_widget || !IsWindow(g_widget)) { ApplyPlacement(); return; }
        if (GetParent(g_widget) != tb || g_taskbar != tb) { ApplyPlacement(); return; }
        RECT want = TargetRect(tb), have{};
        GetWindowRect(g_widget, &have);
        if (abs((int)(have.right - have.left) - (int)(want.right - want.left)) > 1 ||
            abs((int)(have.bottom - have.top) - (int)(want.bottom - want.top)) > 1 ||
            abs((int)have.left - (int)want.left) > 1) {
            ApplyPlacement();
        } else {
            SetWindowPos(g_widget, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return;
    }

    // Overlay mode: follow the taskbar area, hide with it or behind fullscreen apps.
    if (!g_widget || !IsWindow(g_widget)) { ApplyPlacement(); return; }
    bool hide = !tb || !IsWindowVisible(tb) || ForegroundIsFullscreen();
    if (hide) {
        ShowWindow(g_widget, SW_HIDE);
        return;
    }
    ShowWindow(g_widget, SW_SHOWNOACTIVATE);
    RECT want = TargetRect(tb), have{};
    GetWindowRect(g_widget, &have);
    if (!EqualRect(&want, &have))
        SetWindowPos(g_widget, HWND_TOPMOST, want.left, want.top,
                     want.right - want.left, want.bottom - want.top, SWP_NOACTIVATE);
}

// --------------------------------------------------------------------------
//  Notifications
// --------------------------------------------------------------------------
static void ShowBalloon(const std::wstring& title, const std::wstring& text, bool warning) {
    if (!g_cfg.notify || !g_anchor) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_anchor;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = warning ? NIIF_WARNING : NIIF_INFO;
    nid.uTimeout = 15000;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void NotifyLimit(bool weekly, const WndInfo& w) {
    long long now = (long long)time(nullptr);
    long long remain = w.resetAt > now ? w.resetAt - now : 0;
    time_t t = (time_t)w.resetAt;
    struct tm lt{};
    localtime_s(&lt, &t);
    wchar_t hm[32];
    swprintf_s(hm, L"%02d:%02d", lt.tm_hour, lt.tm_min);
    std::wstring title = weekly ? L"Codex: weekly limit reached" : L"Codex: 5-hour limit reached";
    std::wstring body = weekly ? L"The weekly Codex allowance is 100% used." : L"The 5-hour Codex allowance is 100% used.";
    body += L"\nResets at " + std::wstring(hm) + L", in " + FormatRemaining(remain, weekly) + L".";
    ShowBalloon(title, body, true);
    PlayCue(true);
}

static void NotifyAvailable(bool weekly) {
    std::wstring title = weekly ? L"Codex: weekly limit available again" : L"Codex: available again";
    std::wstring body = weekly ? L"The weekly Codex allowance has reset." : L"The 5-hour Codex limit has reset - you can continue.";
    ShowBalloon(title, body, false);
    PlayCue(false);
}

static void UpdateTransitionState(const Usage& u) {
    long long now = (long long)time(nullptr);
    auto step = [&](const WndInfo& w, bool weekly, bool& prevLocked) {
        if (!w.valid) return;
        bool resetPassed = w.resetAt > 0 && now >= w.resetAt + 2;
        bool locked = w.used >= 99.5 && !resetPassed;
        if (!g_havePrev) { prevLocked = locked; return; }
        if (locked && !prevLocked) NotifyLimit(weekly, w);
        else if (!locked && prevLocked) NotifyAvailable(weekly);
        prevLocked = locked;
    };
    step(u.primary, false, g_prevLockP);
    step(u.secondary, true, g_prevLockS);
    g_havePrev = true;
}

// --------------------------------------------------------------------------
//  Tray icon management
// --------------------------------------------------------------------------
static void UpdateTray() {
    if (!g_anchor) return;
    Usage u;
    { std::lock_guard<std::mutex> lk(g_mu); u = g_usage; }

    int pct = 0;
    bool locked = false;
    double usedPct = 0.0;
    Theme th = DetectTheme();
    if (u.primary.valid) {
        usedPct = std::max(0.0, std::min(100.0, u.primary.used));
        locked = usedPct >= 99.5;
        pct = (int)llround(g_cfg.displayMode == 0 ? 100.0 - usedPct : usedPct);
    }
    COLORREF col = locked ? th.red : (usedPct >= 70.0 ? th.amber : th.green);

    HICON old = g_trayIcon;
    g_trayIcon = MakeTrayIcon(pct, col, locked);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_anchor;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    HICON fallback = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    nid.hIcon = g_trayIcon ? g_trayIcon : (fallback ? fallback : LoadIconW(nullptr, IDI_APPLICATION));

    long long now = (long long)time(nullptr);
    const bool remaining = (g_cfg.displayMode == 0);
    std::wstring tip = L"Codex";
    if (u.ok) {
        if (u.primary.valid) {
            long long r = u.primary.resetAt - now;
            double shown = remaining ? 100.0 - std::max(0.0, std::min(100.0, u.primary.used)) : u.primary.used;
            tip += L"\n5h: " + PercentText(shown) + (remaining ? L" left, resets in " : L" used, resets in ") +
                   FormatRemaining(r > 0 ? r : 0, false);
        }
        if (u.secondary.valid) {
            long long r = u.secondary.resetAt - now;
            double shown = remaining ? 100.0 - std::max(0.0, std::min(100.0, u.secondary.used)) : u.secondary.used;
            tip += L"\nwk: " + PercentText(shown) + (remaining ? L" left, resets in " : L" used, resets in ") +
                   FormatRemaining(r > 0 ? r : 0, true);
        }
        if (u.stale) tip += L"\n(from session log)";
    } else {
        tip += L"\n" + (u.error.empty() ? std::wstring(L"no data") : u.error);
    }
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);

    if (!g_trayAdded) {
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            g_trayAdded = true;
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
        }
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    if (old && old != g_trayIcon) DestroyIcon(old);
}

// --------------------------------------------------------------------------
//  Polling
// --------------------------------------------------------------------------
static void ScheduleNextPoll(const Usage& u) {
    long long now = (long long)time(nullptr);
    long long delayMs = (long long)g_cfg.refreshSec * 1000;
    if (!u.ok) {
        delayMs = 20000;
    } else {
        long long best = 0;
        auto consider = [&](const WndInfo& w) {
            if (!w.valid || w.resetAt <= 0) return;
            long long ms = (w.resetAt - now + 3) * 1000;
            if (ms > 0 && (best == 0 || ms < best)) best = ms;
        };
        consider(u.primary);
        consider(u.secondary);
        if (best > 0 && best < delayMs) delayMs = std::max<long long>(best, 5000);
    }
    g_nextPollDue = GetTickCount64() + (ULONGLONG)delayMs;
}

static void RequestRefresh() {
    if (g_fetching) return;
    g_fetching = true;
    HWND anchor = g_anchor;
    std::thread([anchor]() {
        Usage u = FetchUsage();
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_usage = u;
        }
        g_fetching = false;
        if (anchor) PostMessageW(anchor, WM_REFRESHED, 0, 0);
    }).detach();
}

// --------------------------------------------------------------------------
//  Menu
// --------------------------------------------------------------------------
static void ShowMenu() {
    if (g_menuOpen) return;
    g_menuOpen = true;

    Usage u;
    { std::lock_guard<std::mutex> lk(g_mu); u = g_usage; }

    HMENU m = CreatePopupMenu();
    HMENU interval = CreatePopupMenu();
    AppendMenuW(interval, MF_STRING | (g_cfg.refreshSec == 30 ? MF_CHECKED : 0), ID_INT_30, L"30 seconds");
    AppendMenuW(interval, MF_STRING | (g_cfg.refreshSec == 60 ? MF_CHECKED : 0), ID_INT_60, L"1 minute");
    AppendMenuW(interval, MF_STRING | (g_cfg.refreshSec == 120 ? MF_CHECKED : 0), ID_INT_120, L"2 minutes");
    AppendMenuW(interval, MF_STRING | (g_cfg.refreshSec == 300 ? MF_CHECKED : 0), ID_INT_300, L"5 minutes");

    HMENU display = CreatePopupMenu();
    AppendMenuW(display, MF_STRING | (g_cfg.displayMode == 0 ? MF_CHECKED : 0), ID_MODE_REMAINING,
                L"Remaining (fresh window = 100%)");
    AppendMenuW(display, MF_STRING | (g_cfg.displayMode == 1 ? MF_CHECKED : 0), ID_MODE_USED,
                L"Used (fresh window = 0%)");

    HMENU background = CreatePopupMenu();
    AppendMenuW(background, MF_STRING | (g_cfg.background == 0 ? MF_CHECKED : 0), ID_BG_AUTO,
                L"Auto (blend with taskbar)");
    AppendMenuW(background, MF_STRING | (g_cfg.background == 1 ? MF_CHECKED : 0), ID_BG_TRANSPARENT,
                L"Transparent");
    AppendMenuW(background, MF_STRING | (g_cfg.background == 2 ? MF_CHECKED : 0), ID_BG_SOLID,
                L"Solid");

    auto taskbars = FindTaskbars();
    HMENU monMenu = nullptr;
    if (taskbars.size() > 1) {
        monMenu = CreatePopupMenu();
        for (size_t i = 0; i < taskbars.size(); ++i) {
            wchar_t label[64];
            swprintf_s(label, L"Monitor %d", (int)i + 1);
            AppendMenuW(monMenu, MF_STRING | ((int)i == g_cfg.monitor ? MF_CHECKED : 0),
                        ID_MON_BASE + (int)i, label);
        }
    }

    AppendMenuW(m, MF_STRING, ID_REFRESH_NOW, L"Refresh now");
    AppendMenuW(m, MF_STRING | (g_cfg.widgetVisible ? MF_CHECKED : 0), ID_TOGGLE_WIDGET, L"Show widget");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)display, L"Display");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)background, L"Background");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_cfg.placement == 0 ? MF_CHECKED : 0), ID_PLACE_TASKBAR, L"Widget in taskbar");
    AppendMenuW(m, MF_STRING | (g_cfg.placement == 1 ? MF_CHECKED : 0), ID_PLACE_OVERLAY, L"Widget above taskbar");
    AppendMenuW(m, MF_STRING | (g_cfg.placement == 2 ? MF_CHECKED : 0), ID_PLACE_TRAY, L"Tray icon only");
    if (monMenu) AppendMenuW(m, MF_POPUP, (UINT_PTR)monMenu, L"Monitor");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_cfg.notify ? MF_CHECKED : 0), ID_NOTIFY, L"Notifications");
    AppendMenuW(m, MF_STRING | (g_cfg.sound ? MF_CHECKED : 0), ID_SOUND, L"Sound alerts");
    AppendMenuW(m, MF_STRING | (g_cfg.showWeekly ? MF_CHECKED : 0), ID_WEEKLY, L"Show weekly limit");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)interval, L"Refresh interval");
    AppendMenuW(m, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::wstring status = u.ok
        ? (u.stale ? std::wstring(L"Source: Codex session log") : std::wstring(L"Source: Codex API"))
        : (L"No data: " + u.error);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, status.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    HWND owner = (g_widget && IsWindow(g_widget)) ? g_widget : g_anchor;
    SetForegroundWindow(owner);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                  pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(m);
    g_menuOpen = false;
    if (g_anchor) PostMessageW(g_anchor, WM_NULL, 0, 0);

    switch (cmd) {
        case ID_REFRESH_NOW: RequestRefresh(); break;
        case ID_TOGGLE_WIDGET:
            g_cfg.widgetVisible = !g_cfg.widgetVisible;
            SaveConfig(); ApplyPlacement();
            break;
        case ID_PLACE_TASKBAR: g_cfg.placement = 0; g_cfg.widgetVisible = true; SaveConfig(); ApplyPlacement(); break;
        case ID_PLACE_OVERLAY: g_cfg.placement = 1; g_cfg.widgetVisible = true; SaveConfig(); ApplyPlacement(); break;
        case ID_PLACE_TRAY:    g_cfg.placement = 2; SaveConfig(); ApplyPlacement(); break;
        case ID_NOTIFY: g_cfg.notify = !g_cfg.notify; SaveConfig(); break;
        case ID_SOUND:  g_cfg.sound = !g_cfg.sound; SaveConfig(); break;
        case ID_MODE_REMAINING:
        case ID_MODE_USED:
            g_cfg.displayMode = (cmd == ID_MODE_REMAINING) ? 0 : 1;
            SaveConfig();
            UpdateTray();
            if (g_widget && IsWindow(g_widget)) InvalidateRect(g_widget, nullptr, TRUE);
            break;
        case ID_BG_AUTO:
        case ID_BG_TRANSPARENT:
        case ID_BG_SOLID:
            g_cfg.background = (cmd == ID_BG_AUTO) ? 0 : (cmd == ID_BG_TRANSPARENT ? 1 : 2);
            SaveConfig();
            ApplyPlacement();
            break;
        case ID_WEEKLY: g_cfg.showWeekly = !g_cfg.showWeekly; SaveConfig(); ApplyPlacement(); break;
        case ID_AUTOSTART: SetAutostart(!IsAutostart()); break;
        case ID_INT_30:  g_cfg.refreshSec = 30;  SaveConfig(); g_nextPollDue = 0; break;
        case ID_INT_60:  g_cfg.refreshSec = 60;  SaveConfig(); g_nextPollDue = 0; break;
        case ID_INT_120: g_cfg.refreshSec = 120; SaveConfig(); g_nextPollDue = 0; break;
        case ID_INT_300: g_cfg.refreshSec = 300; SaveConfig(); g_nextPollDue = 0; break;
        case ID_EXIT: DestroyWindow(g_anchor); break;
        default:
            if (cmd >= ID_MON_BASE) {
                g_cfg.monitor = cmd - ID_MON_BASE;
                SaveConfig();
                ApplyPlacement();
            }
            break;
    }
}

// --------------------------------------------------------------------------
//  Widget window proc (painting only)
// --------------------------------------------------------------------------
static LRESULT CALLBACK WidgetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            Usage u;
            { std::lock_guard<std::mutex> lk(g_mu); u = g_usage; }
            PaintWidget(dc, rc, u);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            if (g_anchor) PostMessageW(g_anchor, WM_TRAY, 0, MAKELPARAM(WM_RBUTTONUP, kTrayId));
            return 0;
        case WM_NCHITTEST:
            return HTCLIENT;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --------------------------------------------------------------------------
//  Anchor window proc (owner: tray icon, timers, polling, menu)
// --------------------------------------------------------------------------
static LRESULT CALLBACK AnchorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, TID_TICK, 1000, nullptr);
            return 0;

        case WM_TIMER:
            if (wp == 2) { KillTimer(hwnd, 2); DestroyWindow(hwnd); return 0; }
            if (wp != TID_TICK) return 0;

            if (g_testMode) {
                ULONGLONG now = GetTickCount64();
                if (now >= g_testNext) {
                    if (g_testMode == 2) {   // --test-free: only the "available again" cue
                        NotifyAvailable(false);
                        g_testMode = 0;
                        g_testNext = now + 6000;
                        SetTimer(hwnd, 2, 6000, nullptr);
                    } else if (g_testPhase == 0) {
                        WndInfo fake;
                        fake.valid = true;
                        fake.used = 100;
                        fake.resetAt = (long long)time(nullptr) + 5400;
                        fake.windowSec = 18000;
                        NotifyLimit(false, fake);
                        g_testNext = now + 5000;
                        g_testPhase = 1;
                    } else if (g_testPhase == 1) {
                        NotifyAvailable(false);
                        g_testNext = now + 5000;
                        g_testPhase = 2;
                    } else {
                        DestroyWindow(hwnd);
                        return 0;
                    }
                }
            }

            WatchdogTick();
            if (g_widget && IsWindow(g_widget)) {
                RECT wr{};
                GetWindowRect(g_widget, &wr);
                SampleBackground(wr);
                InvalidateRect(g_widget, nullptr, FALSE);
            }
            if (g_nextPollDue == 0 || GetTickCount64() >= g_nextPollDue) {
                g_nextPollDue = GetTickCount64() + 15000;
                RequestRefresh();
            }
            return 0;

        case WM_TRAY: {
            UINT ev = LOWORD(lp);
            if (ev == WM_LBUTTONUP || ev == NIN_SELECT || ev == NIN_KEYSELECT) {
                g_cfg.widgetVisible = !g_cfg.widgetVisible;
                SaveConfig();
                ApplyPlacement();
            } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
                ShowMenu();
            }
            return 0;
        }

        case WM_REFRESHED: {
            Usage u;
            { std::lock_guard<std::mutex> lk(g_mu); u = g_usage; }
            UpdateTransitionState(u);
            UpdateTray();
            ScheduleNextPoll(u);
            if (g_widget && IsWindow(g_widget)) InvalidateRect(g_widget, nullptr, FALSE);
            return 0;
        }

        case WM_REATTACH:
            ApplyPlacement();
            return 0;

        case WM_DISPLAYCHANGE:
            ApplyPlacement();
            return 0;

        case WM_SETTINGCHANGE:
            ApplyPlacement();
            return 0;

        case WM_DESTROY: {
            KillTimer(hwnd, TID_TICK);
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = kTrayId;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            g_trayAdded = false;
            DestroyWidget();
            if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = nullptr; }
            PostQuitMessage(0);
            return 0;
        }
    }

    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
        // Explorer restarted: the previous taskbar (and our child window) are gone.
        DestroyWidget();
        g_trayAdded = false;
        UpdateTray();
        ApplyPlacement();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --------------------------------------------------------------------------
//  Diagnostics
// --------------------------------------------------------------------------
static int RunDump() {
    Usage u = FetchUsage();
    wchar_t buf[1024];

    // Background sampling probe: what does the app actually read from the taskbar?
    std::wstring bgInfo = L"bg: (no taskbar)";
    LoadConfig();
    HWND tb = CurrentTaskbar();
    if (tb) {
        UINT d = GetDpiForWindow(tb);
        g_dpi = (d >= 96 && d <= 480) ? (int)d : 96;
        RECT tr = TargetRect(tb);
        SampleBackground(tr);
        Theme th = DetectTheme();
        wchar_t b2[512];
        swprintf_s(b2,
                   L"bg: dpi=%d valid=%d count=%d target=%ld,%ld-%ld,%ld mode=%d theme=%06lX rows=%06lX,%06lX,%06lX,%06lX",
                   g_dpi, (int)g_bgProfile.valid, g_bgProfile.count,
                   tr.left, tr.top, tr.right, tr.bottom, g_cfg.background,
                   (unsigned long)(th.bg & 0xFFFFFF),
                   g_bgProfile.count > 0 ? (unsigned long)(g_bgProfile.rows[0] & 0xFFFFFF) : 0ul,
                   g_bgProfile.count > 1 ? (unsigned long)(g_bgProfile.rows[1] & 0xFFFFFF) : 0ul,
                   g_bgProfile.count > 2 ? (unsigned long)(g_bgProfile.rows[2] & 0xFFFFFF) : 0ul,
                   g_bgProfile.count > 20 ? (unsigned long)(g_bgProfile.rows[20] & 0xFFFFFF) : 0ul);
        bgInfo = b2;
    }

    swprintf_s(buf,
        L"ok=%d stale=%d source=%s error=%s\r\n"
        L"primary: valid=%d used=%.2f resetAt=%lld window=%lld\r\n"
        L"secondary: valid=%d used=%.2f resetAt=%lld window=%lld\r\n"
        L"now=%lld\r\n",
        (int)u.ok, (int)u.stale, u.source.c_str(), u.error.c_str(),
        (int)u.primary.valid, u.primary.used, u.primary.resetAt, u.primary.windowSec,
        (int)u.secondary.valid, u.secondary.used, u.secondary.resetAt, u.secondary.windowSec,
        (long long)time(nullptr));
    std::wstring out = buf;
    out += L"\r\n";
    out += bgInfo;
    out += L"\r\n";
    WriteAllBytes(AppDataDir() + L"\\dump.txt", W2U8(out));
    return 0;
}

// --------------------------------------------------------------------------
//  Entry point
// --------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;

    if (wcsstr(GetCommandLineW(), L"--dump")) return RunDump();
    if (wcsstr(GetCommandLineW(), L"--test-free")) g_testMode = 2;
    else if (wcsstr(GetCommandLineW(), L"--test")) g_testMode = 1;

    HANDLE singleMutex = CreateMutexW(nullptr, TRUE, L"CodexTaskbarMonitor_SingleInstance");
    if (singleMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(kAnchorClass, nullptr);
        if (prev) {
            PostMessageW(prev, WM_REATTACH, 0, 0);
            PostMessageW(prev, WM_TRAY, 0, MAKELPARAM(WM_RBUTTONUP, kTrayId));
        }
        return 0;
    }

    wchar_t exe[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, exe, (DWORD)(MAX_PATH * 4));
    g_exePath = exe;

    using SetDpiCtxFn = BOOL(WINAPI*)(void*);
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        auto fn = (SetDpiCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (!fn || !fn((void*)-4)) {
            auto fn1 = (BOOL(WINAPI*)(void))GetProcAddress(u32, "SetProcessDPIAware");
            if (fn1) fn1();
        }
    }

    LoadConfig();
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AnchorProc;
    wc.hInstance = inst;
    wc.lpszClassName = kAnchorClass;
    if (!RegisterClassExW(&wc)) return 1;

    WNDCLASSEXW ww{};
    ww.cbSize = sizeof(ww);
    ww.lpfnWndProc = WidgetProc;
    ww.hInstance = inst;
    ww.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    ww.hbrBackground = nullptr;
    ww.lpszClassName = kWidgetClass;
    if (!RegisterClassExW(&ww)) return 1;

    // Hidden top-level owner: survives Explorer restarts, receives broadcasts.
    g_anchor = CreateWindowExW(WS_EX_TOOLWINDOW, kAnchorClass, L"Codex Taskbar Monitor",
                               WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_anchor) return 1;

    UpdateTray();
    ApplyPlacement();
    RequestRefresh();

    if (g_testMode) {
        g_testPhase = 0;
        g_testNext = GetTickCount64() + 1200;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (singleMutex) CloseHandle(singleMutex);
    return 0;
}

