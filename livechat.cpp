// livechat.asi - overlay live chat (TikTok) untuk GTA SA / SAMP, tanpa MoonLoader.
// Membaca file livechat.jsonl yang ditulis oleh bridge.js (satu JSON per baris),
// lalu menggambar teksnya di layar lewat hook DirectX 9 (EndScene).
//
// Build: Visual Studio (MSVC), platform x86 / Win32, hasilnya .dll lalu ganti nama jadi .asi
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ============================================================
//  Konfigurasi (dibaca dari livechat.ini di folder yang sama)
// ============================================================
struct Config {
    std::string file;
    int   maxLines    = 14;
    int   lifetimeSec = 20;
    float posYRatio   = 0.28f;
    float widthRatio  = 0.28f;
    int   fontPx      = 15;
    std::string fontName = "Arial";
    bool  mouseDrag   = true;
    bool  arrowKeys   = true;
};

static Config      g_cfg;
static std::string g_iniPath;
static std::string g_logPath;

// Catatan diagnosa: livechat.log di folder yang sama dengan livechat.asi.
// Isi ketikan pemain TIDAK pernah dicatat, hanya tahapan plugin.
static void Log(const char* fmt, ...) {
    if (g_logPath.empty()) return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    FILE* f = std::fopen(g_logPath.c_str(), "ab");
    if (!f) return;
    std::fprintf(f, "[%lu] %s\r\n", (unsigned long)GetTickCount(), msg);
    std::fclose(f);
}
static HMODULE     g_self = nullptr;

static int g_posX = 20;
static int g_posY = -1;      // -1 = pakai posYRatio
static bool g_enabled = true;
static bool g_edit = false;

static std::string DirOf(const std::string& p) {
    size_t i = p.find_last_of("\\/");
    return i == std::string::npos ? std::string() : p.substr(0, i + 1);
}

static float ReadFloat(const char* section, const char* key, float def) {
    char buf[64];
    char defStr[64];
    std::snprintf(defStr, sizeof(defStr), "%.3f", def);
    GetPrivateProfileStringA(section, key, defStr, buf, sizeof(buf), g_iniPath.c_str());
    return (float)std::atof(buf);
}

static void LoadConfig() {
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(g_self, path, MAX_PATH);
    std::string dir = DirOf(path);
    g_iniPath = dir + "livechat.ini";
    g_logPath = dir + "livechat.log";

    char s[MAX_PATH] = {0};
    GetPrivateProfileStringA("LiveChat", "File", "livechat.jsonl", s, MAX_PATH, g_iniPath.c_str());
    std::string f = s;
    bool absolute = f.size() > 1 && (f[1] == ':' || (f[0] == '\\' && f[1] == '\\'));
    g_cfg.file = absolute ? f : dir + f;

    g_cfg.maxLines    = GetPrivateProfileIntA("LiveChat", "MaxLines", 14, g_iniPath.c_str());
    g_cfg.lifetimeSec = GetPrivateProfileIntA("LiveChat", "LifetimeSec", 20, g_iniPath.c_str());
    g_cfg.fontPx      = GetPrivateProfileIntA("LiveChat", "FontPx", 15, g_iniPath.c_str());
    g_cfg.posYRatio   = ReadFloat("LiveChat", "PosYRatio", 0.28f);
    g_cfg.widthRatio  = ReadFloat("LiveChat", "WidthRatio", 0.28f);
    g_cfg.mouseDrag   = GetPrivateProfileIntA("Move", "MouseDrag", 1, g_iniPath.c_str()) != 0;
    g_cfg.arrowKeys   = GetPrivateProfileIntA("Move", "ArrowKeys", 1, g_iniPath.c_str()) != 0;

    char fn[64] = {0};
    GetPrivateProfileStringA("LiveChat", "FontName", "Arial", fn, sizeof(fn), g_iniPath.c_str());
    g_cfg.fontName = fn;

    if (g_cfg.maxLines < 1) g_cfg.maxLines = 1;
    if (g_cfg.maxLines > 40) g_cfg.maxLines = 40;
    if (g_cfg.fontPx < 8) g_cfg.fontPx = 8;
    if (g_cfg.fontPx > 48) g_cfg.fontPx = 48;
    if (g_cfg.widthRatio < 0.1f) g_cfg.widthRatio = 0.1f;
    if (g_cfg.widthRatio > 0.9f) g_cfg.widthRatio = 0.9f;

    g_posX = GetPrivateProfileIntA("Position", "X", 20, g_iniPath.c_str());
    g_posY = GetPrivateProfileIntA("Position", "Y", -1, g_iniPath.c_str());
}

static void SavePos() {
    char b[32];
    std::snprintf(b, sizeof(b), "%d", g_posX);
    WritePrivateProfileStringA("Position", "X", b, g_iniPath.c_str());
    std::snprintf(b, sizeof(b), "%d", g_posY);
    WritePrivateProfileStringA("Position", "Y", b, g_iniPath.c_str());
}

// ============================================================
//  Parsing JSON sederhana: "key":"value"
// ============================================================
static bool GetField(const std::string& line, const std::string& key, std::string& out) {
    std::string pat = "\"" + key + "\"";
    size_t k = line.find(pat);
    if (k == std::string::npos) return false;
    size_t c = line.find(':', k + pat.size());
    if (c == std::string::npos) return false;
    size_t q = line.find('"', c + 1);
    if (q == std::string::npos) return false;
    out.clear();
    for (size_t i = q + 1; i < line.size(); ++i) {
        char ch = line[i];
        if (ch == '\\') {
            if (i + 1 >= line.size()) break;
            char n = line[++i];
            if (n == 'n') out += ' ';
            else if (n == 'u') { out += '?'; i += 4; }
            else out += n;
        } else if (ch == '"') {
            return true;
        } else {
            out += ch;
        }
    }
    return true;
}

// ============================================================
//  Data pesan
// ============================================================
struct Seg  { std::string text; COLORREF color; };
struct Line { std::vector<Seg> segs; };
struct Message { std::vector<Line> lines; DWORD born; };

static std::vector<Message> g_msgs;
static bool g_dirty = true;

// ============================================================
//  Perintah: dari ketikan di chat game (/lc ...) atau dari bridge (type "cmd")
// ============================================================
static CRITICAL_SECTION g_cs;
static std::vector<std::string> g_cmdQueue;

static void QueueCommand(const std::string& c) {
    EnterCriticalSection(&g_cs);
    if (g_cmdQueue.size() < 32) g_cmdQueue.push_back(c);
    LeaveCriticalSection(&g_cs);
}

static std::string g_notice;
static bool g_noticeActive = false;
static DWORD g_noticeStart = 0;

static void Notice(const std::string& t) {
    g_notice = t;
    g_noticeActive = true;
    g_noticeStart = GetTickCount();
    g_dirty = true;
}

// Subclass jendela game: ikuti apa yang diketik, lalu saat Enter cek apakah itu perintah kita.
static WNDPROC g_origWndProc = nullptr;
static HWND g_subclassed = nullptr;
static std::string g_typed;

static LRESULT CALLBACK LcWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CHAR) {
        char c = (char)wp;
        if (c == '/') {
            g_typed = "/";                        // mulai buffer baru hanya saat ada '/'
        } else if (!g_typed.empty()) {
            if (c == '\b') {
                g_typed.pop_back();
            } else if (c >= 32 && c < 127) {
                g_typed += c;
                if (g_typed.size() > 64) g_typed.clear();
            }
        }
    } else if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            if (!g_typed.empty()) {
                std::string cmd = g_typed;
                for (size_t i = 0; i < cmd.size(); ++i)
                    if (cmd[i] >= 'A' && cmd[i] <= 'Z') cmd[i] = (char)(cmd[i] - 'A' + 'a');
                if (cmd.compare(0, 3, "/lc") == 0) QueueCommand(cmd);
            }
            g_typed.clear();
        } else if (wp == VK_ESCAPE) {
            g_typed.clear();
        }
    }
    return CallWindowProcA(g_origWndProc, h, msg, wp, lp);
}

static void EnsureSubclass(HWND hwnd) {
    if (!hwnd || hwnd == g_subclassed) return;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&LcWndProc)));
    g_subclassed = hwnd;
    Log("jendela game di-subclass: hwnd=%p, wndproc lama=%p", (void*)hwnd, (void*)g_origWndProc);
}

static int TotalLines() {
    int n = 0;
    for (size_t i = 0; i < g_msgs.size(); ++i) n += (int)g_msgs[i].lines.size();
    return n;
}

// ============================================================
//  GDI: teks digambar ke bitmap, lalu diunggah ke texture D3D9
// ============================================================
struct Gdi {
    HDC dcA = nullptr, dcB = nullptr;           // A = bayangan/garis tepi, B = warna teks
    HBITMAP bmA = nullptr, bmB = nullptr;
    HGDIOBJ oldBmA = nullptr, oldBmB = nullptr;
    HGDIOBJ oldFontA = nullptr, oldFontB = nullptr;
    HFONT font = nullptr;
    unsigned int* bitsA = nullptr;
    unsigned int* bitsB = nullptr;
    int w = 0, h = 0;                            // ukuran bitmap = ukuran texture (pangkat 2)
    int lineH = 0;
};
static Gdi g_gdi;

static void GdiDestroy() {
    if (g_gdi.dcA) {
        if (g_gdi.oldFontA) SelectObject(g_gdi.dcA, g_gdi.oldFontA);
        if (g_gdi.oldBmA) SelectObject(g_gdi.dcA, g_gdi.oldBmA);
        DeleteDC(g_gdi.dcA);
    }
    if (g_gdi.dcB) {
        if (g_gdi.oldFontB) SelectObject(g_gdi.dcB, g_gdi.oldFontB);
        if (g_gdi.oldBmB) SelectObject(g_gdi.dcB, g_gdi.oldBmB);
        DeleteDC(g_gdi.dcB);
    }
    if (g_gdi.bmA) DeleteObject(g_gdi.bmA);
    if (g_gdi.bmB) DeleteObject(g_gdi.bmB);
    if (g_gdi.font) DeleteObject(g_gdi.font);
    g_gdi = Gdi();
}

static int NextPow2(int v) {
    int p = 64;
    while (p < v) p <<= 1;
    return p;
}

static HBITMAP MakeDib(HDC dc, int w, int h, unsigned int** bits) {
    BITMAPINFO bi;
    std::memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;       // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* p = nullptr;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    *bits = static_cast<unsigned int*>(p);
    return bm;
}

static bool GdiCreate(int w, int h) {
    GdiDestroy();
    g_gdi.w = w;
    g_gdi.h = h;
    g_gdi.dcA = CreateCompatibleDC(nullptr);
    g_gdi.dcB = CreateCompatibleDC(nullptr);
    if (!g_gdi.dcA || !g_gdi.dcB) return false;

    g_gdi.bmA = MakeDib(g_gdi.dcA, w, h, &g_gdi.bitsA);
    g_gdi.bmB = MakeDib(g_gdi.dcB, w, h, &g_gdi.bitsB);
    if (!g_gdi.bmA || !g_gdi.bmB) return false;

    g_gdi.oldBmA = SelectObject(g_gdi.dcA, g_gdi.bmA);
    g_gdi.oldBmB = SelectObject(g_gdi.dcB, g_gdi.bmB);

    g_gdi.font = CreateFontA(-g_cfg.fontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             g_cfg.fontName.c_str());
    if (!g_gdi.font) return false;
    g_gdi.oldFontA = SelectObject(g_gdi.dcA, g_gdi.font);
    g_gdi.oldFontB = SelectObject(g_gdi.dcB, g_gdi.font);

    SetBkMode(g_gdi.dcA, TRANSPARENT);
    SetBkMode(g_gdi.dcB, TRANSPARENT);
    SetTextColor(g_gdi.dcA, RGB(255, 255, 255));

    TEXTMETRICA tm;
    std::memset(&tm, 0, sizeof(tm));
    GetTextMetricsA(g_gdi.dcA, &tm);
    g_gdi.lineH = tm.tmHeight + 2;
    return true;
}

static int TextW(const std::string& s) {
    if (s.empty() || !g_gdi.dcA) return 0;
    SIZE sz;
    sz.cx = 0; sz.cy = 0;
    GetTextExtentPoint32A(g_gdi.dcA, s.c_str(), (int)s.size(), &sz);
    return (int)sz.cx;
}

static std::vector<std::string> WrapText(const std::string& plain, int maxW) {
    std::vector<std::string> lines;
    std::string cur;
    size_t i = 0;
    while (i < plain.size()) {
        while (i < plain.size() && plain[i] == ' ') ++i;
        size_t j = i;
        while (j < plain.size() && plain[j] != ' ') ++j;
        if (j > i) {
            std::string w = plain.substr(i, j - i);
            for (;;) {
                std::string part = w.size() > 28 ? w.substr(0, 28) : w;
                std::string t = cur.empty() ? part : cur + " " + part;
                if (!cur.empty() && TextW(t) > maxW) {
                    lines.push_back(cur);
                    cur = part;
                } else {
                    cur = t;
                }
                if (w.size() > 28) w = w.substr(28); else break;
            }
        }
        i = j;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

static const COLORREF C_WHITE  = RGB(255, 255, 255);
static const COLORREF C_YELLOW = RGB(255, 255, 0);
static const COLORREF C_GOLD   = RGB(255, 215, 0);

static void AddMessage(bool gift, std::string user, std::string text, int maxW) {
    for (size_t i = 0; i < user.size(); ++i) if (user[i] == ' ') user[i] = '_';
    if (text.size() > 100) text = text.substr(0, 100) + "...";

    std::string plain = gift ? ("[GIFT] " + user + ": " + text) : (user + ": " + text);
    std::vector<std::string> lines = WrapText(plain, maxW);
    if (lines.empty()) return;

    Message m;
    m.born = GetTickCount();
    for (size_t i = 0; i < lines.size(); ++i) {
        Line ln;
        Seg s;
        if (gift) {
            s.text = lines[i]; s.color = C_GOLD;
            ln.segs.push_back(s);
        } else if (i == 0 && lines[i].compare(0, user.size(), user) == 0) {
            s.text = user; s.color = C_YELLOW;
            ln.segs.push_back(s);
            s.text = lines[i].substr(user.size()); s.color = C_WHITE;
            ln.segs.push_back(s);
        } else {
            s.text = lines[i]; s.color = C_WHITE;
            ln.segs.push_back(s);
        }
        m.lines.push_back(ln);
    }
    g_msgs.push_back(m);
    while (TotalLines() > g_cfg.maxLines && g_msgs.size() > 1) g_msgs.erase(g_msgs.begin());
    g_dirty = true;
}

static void ExpireOld() {
    DWORD now = GetTickCount();
    if (g_noticeActive && now - g_noticeStart > 4000) { g_noticeActive = false; g_dirty = true; }
    bool removed = false;
    while (!g_msgs.empty() && now - g_msgs.front().born > (DWORD)g_cfg.lifetimeSec * 1000) {
        g_msgs.erase(g_msgs.begin());
        removed = true;
    }
    if (removed) g_dirty = true;
}

// ============================================================
//  Baca baris baru dari livechat.jsonl
// ============================================================
static long g_offset = -1;

static void ReadNew(int maxW) {
    FILE* f = std::fopen(g_cfg.file.c_str(), "rb");
    if (!f) {
        if (g_offset < 0) g_offset = 0;
        return;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    if (g_offset < 0) {                 // pertama kali: mulai dari akhir file
        g_offset = size;
        std::fclose(f);
        return;
    }
    if (size < g_offset) g_offset = 0;  // file di-reset
    if (size == g_offset) { std::fclose(f); return; }

    std::fseek(f, g_offset, SEEK_SET);
    std::string data((size_t)(size - g_offset), '\0');
    size_t n = std::fread(&data[0], 1, data.size(), f);
    data.resize(n);
    std::fclose(f);

    size_t pos = 0;
    long consumed = 0;
    int count = 0;
    for (;;) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = data.substr(pos, nl - pos);
        pos = nl + 1;
        consumed = (long)pos;

        if (line.size() > 2) {
            std::string type, user, text;
            GetField(line, "type", type);
            GetField(line, "user", user);
            GetField(line, "text", text);
            if (user.empty()) user = "?";
            if (type == "cmd") QueueCommand("/" + text);
            else if (g_enabled) AddMessage(type == "gift", user, text, maxW);
        }
        if (++count >= 10) break;
    }
    g_offset += consumed;
}

// ============================================================
//  Render ke texture D3D9
// ============================================================
static IDirect3DTexture9* g_tex = nullptr;
static IDirect3DDevice9*  g_dev = nullptr;
static int g_scrW = 0, g_scrH = 0;
static int g_usedW = 0;                // lebar area teks (piksel)
static const int PAD = 4;

static void ReleaseTexture() {
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
}

static void ResetAll() {
    ReleaseTexture();
    GdiDestroy();
    g_msgs.clear();
    g_dirty = true;
    g_dev = nullptr;
    g_scrW = g_scrH = 0;
}

static bool EnsureResources(IDirect3DDevice9* dev, int sw, int sh) {
    if (dev == g_dev && sw == g_scrW && sh == g_scrH && g_tex && g_gdi.dcA) return true;

    ResetAll();
    g_dev = dev;
    g_scrW = sw;
    g_scrH = sh;
    g_usedW = (int)(sw * g_cfg.widthRatio);

    // font dulu (untuk ukuran baris), lalu bitmap dan texture
    if (!GdiCreate(64, 64)) return false;
    int lineH = g_gdi.lineH;
    int w = NextPow2(g_usedW + PAD * 2);
    int h = NextPow2(g_cfg.maxLines * lineH + PAD * 2);
    if (!GdiCreate(w, h)) return false;

    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr))) {
        g_tex = nullptr;
        return false;
    }
    return true;
}

static std::vector<Line> CurrentLines() {
    std::vector<Line> out;

    if (g_noticeActive && !g_notice.empty()) {
        Line ln;
        Seg a, b;
        a.text = "[LiveChat] "; a.color = C_YELLOW;
        b.text = g_notice;      b.color = C_WHITE;
        ln.segs.push_back(a);
        ln.segs.push_back(b);
        out.push_back(ln);
    }
    if (!g_enabled) return out;

    for (size_t i = 0; i < g_msgs.size(); ++i)
        for (size_t j = 0; j < g_msgs[i].lines.size(); ++j)
            out.push_back(g_msgs[i].lines[j]);

    if (g_edit && g_msgs.empty()) {
        const char* txt[3] = {
            "Geser teks ini dengan mouse",
            "Klik kanan atau /lcmove = simpan",
            "Panah = geser halus (Shift = cepat)"
        };
        for (int i = 0; i < 3; ++i) {
            Line ln;
            Seg sg;
            sg.text = txt[i];
            sg.color = (i == 0) ? C_YELLOW : C_WHITE;
            ln.segs.push_back(sg);
            out.push_back(ln);
        }
    }
    return out;
}

static void UploadText(const std::vector<Line>& lines) {
    if (!g_tex || !g_gdi.bitsA || !g_gdi.bitsB) return;
    const int w = g_gdi.w, h = g_gdi.h;
    std::memset(g_gdi.bitsA, 0, (size_t)w * h * 4);
    std::memset(g_gdi.bitsB, 0, (size_t)w * h * 4);

    int y = PAD;
    for (size_t i = 0; i < lines.size(); ++i) {
        int x = PAD;
        for (size_t k = 0; k < lines[i].segs.size(); ++k) {
            const Seg& sg = lines[i].segs[k];
            if (!sg.text.empty()) {
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        TextOutA(g_gdi.dcA, x + dx, y + dy, sg.text.c_str(), (int)sg.text.size());
                SetTextColor(g_gdi.dcB, sg.color);
                TextOutA(g_gdi.dcB, x, y, sg.text.c_str(), (int)sg.text.size());
                x += TextW(sg.text);
            }
        }
        y += g_gdi.lineH;
    }
    GdiFlush();

    D3DLOCKED_RECT lr;
    if (FAILED(g_tex->LockRect(0, &lr, nullptr, 0))) return;
    for (int row = 0; row < h; ++row) {
        unsigned int* dst = reinterpret_cast<unsigned int*>(static_cast<unsigned char*>(lr.pBits) + (size_t)row * lr.Pitch);
        const unsigned int* a = g_gdi.bitsA + (size_t)row * w;
        const unsigned int* b = g_gdi.bitsB + (size_t)row * w;
        for (int col = 0; col < w; ++col) {
            unsigned int ao = a[col] & 0xFF;
            unsigned int pb = b[col];
            unsigned int r = (pb >> 16) & 0xFF, g = (pb >> 8) & 0xFF, bl = pb & 0xFF;
            unsigned int at = std::max(r, std::max(g, bl));
            unsigned int A = std::max(ao, at);
            dst[col] = (A << 24) | (r << 16) | (g << 8) | bl;   // premultiplied alpha
        }
    }
    g_tex->UnlockRect(0);
}

struct Vtx { float x, y, z, rhw; DWORD color; float u, v; };
static const DWORD VTX_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

static void DrawQuad(IDirect3DDevice9* dev, float x, float y, float w, float h, float u1, float v1, DWORD color) {
    Vtx v[4] = {
        { x - 0.5f,     y - 0.5f,     0.0f, 1.0f, color, 0.0f, 0.0f },
        { x + w - 0.5f, y - 0.5f,     0.0f, 1.0f, color, u1,   0.0f },
        { x - 0.5f,     y + h - 0.5f, 0.0f, 1.0f, color, 0.0f, v1   },
        { x + w - 0.5f, y + h - 0.5f, 0.0f, 1.0f, color, u1,   v1   }
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
}

static void SetCommonStates(IDirect3DDevice9* dev, int sw, int sh) {
    D3DVIEWPORT9 vp;
    vp.X = 0; vp.Y = 0; vp.Width = (DWORD)sw; vp.Height = (DWORD)sh; vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(VTX_FVF);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0);
}

// ============================================================
//  Perintah + geser dengan mouse / panah
// ============================================================
static int g_blockX = 0, g_blockY = 0, g_blockW = 0, g_blockH = 0;   // area teks terakhir
static bool g_dragging = false;
static int g_dragDX = 0, g_dragDY = 0;

static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void ShowMouse(bool on) {
    int n = 0;
    if (on) { while (ShowCursor(TRUE) < 0 && n++ < 10) {} }
    else    { while (ShowCursor(FALSE) >= 0 && n++ < 10) {} }
}

static void SetEdit(bool on) {
    if (on == g_edit) return;
    g_edit = on;
    g_dragging = false;
    if (g_cfg.mouseDrag) ShowMouse(on);
    if (!on) { SavePos(); Notice("Posisi disimpan"); }
    else Notice("Mode geser aktif");
    g_dirty = true;
}

static void ClampPos(int sw, int sh) {
    if (g_posX < 0) g_posX = 0;
    if (g_posX > sw - 60) g_posX = sw - 60;
    if (g_posY >= 0) {
        if (g_posY > sh - 30) g_posY = sh - 30;
    }
}

// cmd contoh: "/lcpos 20 300"
static void ExecCommand(const std::string& raw, int sw, int sh) {
    std::string c = raw;
    while (!c.empty() && (c[0] == '/' || c[0] == ' ')) c.erase(0, 1);
    while (!c.empty() && c[c.size() - 1] == ' ') c.erase(c.size() - 1);
    size_t sp = c.find(' ');
    std::string name = (sp == std::string::npos) ? c : c.substr(0, sp);
    std::string args = (sp == std::string::npos) ? std::string() : c.substr(sp + 1);
    Log("perintah diterima: %s", name.c_str());

    if (name == "lc") {
        g_enabled = !g_enabled;
        if (!g_enabled) { g_msgs.clear(); SetEdit(false); }
        Notice(g_enabled ? "ON" : "OFF");
    } else if (name == "lcclear") {
        g_msgs.clear();
        g_dirty = true;
    } else if (name == "lcmove") {
        if (!g_enabled) { g_enabled = true; }
        SetEdit(!g_edit);
    } else if (name == "lcpos") {
        int x = 0, y = 0;
        if (std::sscanf(args.c_str(), "%d %d", &x, &y) == 2) {
            g_posX = x;
            g_posY = y;
            ClampPos(sw, sh);
            SavePos();
            Notice("Posisi diubah");
        } else {
            Notice("Pakai: /lcpos X Y   atau /lcmove");
        }
    } else if (name == "lcreset") {
        g_posX = 20;
        g_posY = -1;
        SavePos();
        Notice("Posisi dikembalikan ke awal");
    }
}

static void HandleInput(HWND hwnd, int sw, int sh) {
    static bool pRight = false;

    // perintah dari ketikan chat / bridge
    std::vector<std::string> cmds;
    EnterCriticalSection(&g_cs);
    cmds.swap(g_cmdQueue);
    LeaveCriticalSection(&g_cs);
    for (size_t i = 0; i < cmds.size(); ++i) ExecCommand(cmds[i], sw, sh);

    if (!g_edit) { pRight = KeyDown(VK_RBUTTON); return; }
    if (hwnd && GetForegroundWindow() != hwnd) return;   // abaikan input saat game tidak aktif

    if (g_posY < 0) g_posY = g_blockY;

    // panah: geser halus
    if (g_cfg.arrowKeys) {
        int step = KeyDown(VK_SHIFT) ? 10 : 3;
        if (KeyDown(VK_LEFT))  g_posX -= step;
        if (KeyDown(VK_RIGHT)) g_posX += step;
        if (KeyDown(VK_UP))    g_posY -= step;
        if (KeyDown(VK_DOWN))  g_posY += step;
    }

    // mouse: seret blok teks
    if (g_cfg.mouseDrag && hwnd) {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        RECT rc;
        GetClientRect(hwnd, &rc);
        float fx = rc.right > 0 ? (float)sw / (float)rc.right : 1.0f;
        float fy = rc.bottom > 0 ? (float)sh / (float)rc.bottom : 1.0f;
        int cx = (int)(p.x * fx), cy = (int)(p.y * fy);

        bool down = KeyDown(VK_LBUTTON);
        if (down && !g_dragging) {
            if (cx >= g_blockX - 6 && cx <= g_blockX + g_blockW + 6 &&
                cy >= g_blockY - 4 && cy <= g_blockY + g_blockH + 4) {
                g_dragging = true;
                g_dragDX = cx - g_posX;
                g_dragDY = cy - g_posY;
            }
        } else if (down && g_dragging) {
            g_posX = cx - g_dragDX;
            g_posY = cy - g_dragDY;
        } else if (!down) {
            g_dragging = false;
        }
    }
    ClampPos(sw, sh);

    // klik kanan: simpan dan keluar
    bool r = KeyDown(VK_RBUTTON);
    if (r && !pRight) { pRight = r; SetEdit(false); return; }
    pRight = r;
}

// ============================================================
//  Hook EndScene
// ============================================================
typedef HRESULT (WINAPI *EndScene_t)(IDirect3DDevice9*);
static EndScene_t g_origEndScene = nullptr;

static DWORD g_lastRead = 0;
static unsigned g_endSceneCalls = 0;

static void RenderOverlay(IDirect3DDevice9* dev) {
    static int  logNotBack = 0;
    static bool loggedSameSize = false, loggedFirstDraw = false, loggedNoStateBlock = false, loggedResFail = false;

    // hanya gambar saat target render = backbuffer (EndScene dipanggil beberapa kali per frame)
    IDirect3DSurface9* rt = nullptr;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) { rt->Release(); return; }
    D3DSURFACE_DESC desc, rdesc;
    std::memset(&desc, 0, sizeof(desc));
    std::memset(&rdesc, 0, sizeof(rdesc));
    bb->GetDesc(&desc);
    rt->GetDesc(&rdesc);
    bool same = (rt == bb);
    bool sameSize = rdesc.Width == desc.Width && rdesc.Height == desc.Height && rdesc.Format == desc.Format;
    rt->Release();
    bb->Release();
    if (!same && !sameSize) {
        if (logNotBack < 3) { ++logNotBack; Log("EndScene bukan backbuffer (target %ux%u, backbuffer %ux%u)", rdesc.Width, rdesc.Height, desc.Width, desc.Height); }
        return;
    }
    if (!same && !loggedSameSize) { loggedSameSize = true; Log("target render bukan pointer backbuffer, tapi ukuran sama; tetap digambar"); }

    int sw = (int)desc.Width, sh = (int)desc.Height;
    if (sw < 320 || sh < 240) return;

    if (!EnsureResources(dev, sw, sh)) {
        if (!loggedResFail) { loggedResFail = true; Log("EnsureResources GAGAL (layar %dx%d)", sw, sh); }
        return;
    }

    D3DDEVICE_CREATION_PARAMETERS cp;
    std::memset(&cp, 0, sizeof(cp));
    dev->GetCreationParameters(&cp);
    HWND hwnd = cp.hFocusWindow;

    // input + baca file + hapus pesan lama
    EnsureSubclass(hwnd);
    HandleInput(hwnd, sw, sh);
    DWORD now = GetTickCount();
    if (now - g_lastRead > 300) {
        g_lastRead = now;
        ReadNew(g_usedW);
        ExpireOld();
    }

    std::vector<Line> lines = CurrentLines();
    if (lines.empty()) return;

    static size_t lastCount = (size_t)-1;
    if (g_dirty || lines.size() != lastCount) {
        UploadText(lines);
        lastCount = lines.size();
        g_dirty = false;
    }

    int lineH = g_gdi.lineH;
    int x = g_posX;
    int y = (g_posY >= 0) ? g_posY : (int)(sh * g_cfg.posYRatio);
    int w = g_usedW + PAD * 2;
    int h = (int)lines.size() * lineH + PAD * 2;
    g_blockX = x; g_blockY = y; g_blockW = w; g_blockH = h;

    float u1 = (float)w / (float)g_gdi.w;
    float v1 = (float)h / (float)g_gdi.h;
    if (u1 > 1.0f) u1 = 1.0f;
    if (v1 > 1.0f) v1 = 1.0f;

    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) {
        if (!loggedNoStateBlock) { loggedNoStateBlock = true; Log("CreateStateBlock GAGAL"); }
        return;
    }

    SetCommonStates(dev, sw, sh);

    if (g_edit) {
        // kotak latar saat mode geser
        dev->SetTexture(0, nullptr);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        DWORD boxColor = g_dragging ? 0x9900AA00 : 0x88000000;
        DrawQuad(dev, (float)(x - 6), (float)(y - 4), (float)(w + 12), (float)(h + 8), 0.0f, 0.0f, boxColor);
    }

    // teks (premultiplied alpha)
    dev->SetTexture(0, g_tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    DrawQuad(dev, (float)x, (float)y, (float)w, (float)h, u1, v1, 0xFFFFFFFF);

    sb->Apply();
    sb->Release();

    if (!loggedFirstDraw) {
        loggedFirstDraw = true;
        Log("gambar pertama berhasil: layar %dx%d, blok di (%d,%d) %dx%d, %d baris", sw, sh, x, y, w, h, (int)lines.size());
    }
}

static HRESULT WINAPI hkEndScene(IDirect3DDevice9* dev) {
    static bool busy = false;
    ++g_endSceneCalls;
    if (g_endSceneCalls == 1) Log("hook EndScene DIPANGGIL game (dev=%p)", (void*)dev);
    if (g_endSceneCalls == 2000) Log("EndScene sudah dipanggil 2000 kali, hook hidup");
    if (!busy && dev) {
        busy = true;
        try {
            RenderOverlay(dev);
        } catch (...) {
            static bool loggedEx = false;
            if (!loggedEx) { loggedEx = true; Log("EXCEPTION di RenderOverlay"); }
        }
        busy = false;
    }
    return g_origEndScene(dev);
}

// ============================================================
//  Pasang hook (lewat vtable device dummy)
// ============================================================
typedef IDirect3D9* (WINAPI *Direct3DCreate9_t)(UINT);

static bool InstallHook() {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9) { Log("InstallHook: d3d9.dll belum dimuat"); return false; }
    char d3dPath[MAX_PATH] = {0};
    GetModuleFileNameA(d3d9, d3dPath, MAX_PATH);
    Log("d3d9.dll dipakai: %s", d3dPath);
    Log("samp.dll %s", GetModuleHandleA("samp.dll") ? "ADA" : "tidak ada");

    Direct3DCreate9_t create = reinterpret_cast<Direct3DCreate9_t>(GetProcAddress(d3d9, "Direct3DCreate9"));
    if (!create) { Log("InstallHook: Direct3DCreate9 tidak ditemukan"); return false; }

    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d) { Log("InstallHook: Direct3DCreate9 mengembalikan NULL"); return false; }

    HWND wnd = CreateWindowExA(0, "STATIC", "lc_dummy", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, g_self, nullptr);
    if (!wnd) { Log("InstallHook: CreateWindowEx GAGAL (error %lu)", GetLastError()); d3d->Release(); return false; }

    D3DPRESENT_PARAMETERS pp;
    std::memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = wnd;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT, &pp, &dev);
    if (FAILED(hr) || !dev) {
        Log("CreateDevice HAL gagal (hr=0x%08lX), coba NULLREF", (unsigned long)hr);
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, wnd,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    }
    if (FAILED(hr) || !dev) {
        Log("CreateDevice NULLREF juga gagal (hr=0x%08lX)", (unsigned long)hr);
        DestroyWindow(wnd);
        d3d->Release();
        return false;
    }

    void** vtbl = *reinterpret_cast<void***>(dev);
    const int ENDSCENE_INDEX = 42;
    DWORD oldProt = 0;
    bool ok = false;
    if (VirtualProtect(&vtbl[ENDSCENE_INDEX], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
        g_origEndScene = reinterpret_cast<EndScene_t>(vtbl[ENDSCENE_INDEX]);
        vtbl[ENDSCENE_INDEX] = reinterpret_cast<void*>(&hkEndScene);
        VirtualProtect(&vtbl[ENDSCENE_INDEX], sizeof(void*), oldProt, &oldProt);
        Log("vtable dipatch: vtbl=%p, EndScene asli=%p, hook=%p", (void*)vtbl, (void*)g_origEndScene, (void*)&hkEndScene);
        ok = true;
    } else {
        Log("VirtualProtect GAGAL (error %lu)", GetLastError());
    }

    dev->Release();
    d3d->Release();
    DestroyWindow(wnd);
    return ok;
}

static DWORD WINAPI InitThread(LPVOID) {
    LoadConfig();
    Log("==== livechat.asi v4 dimulai ====");
    Log("ini=%s", g_iniPath.c_str());
    Log("file komentar=%s", g_cfg.file.c_str());
    for (int i = 0; i < 1200; ++i) {                 // tunggu d3d9.dll (maks. 2 menit)
        if (GetModuleHandleA("d3d9.dll")) break;
        Sleep(100);
    }
    Sleep(2000);
    bool ok = InstallHook();
    Log("InstallHook -> %s", ok ? "BERHASIL" : "GAGAL");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hMod;
        InitializeCriticalSection(&g_cs);
        DisableThreadLibraryCalls(hMod);
        HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
