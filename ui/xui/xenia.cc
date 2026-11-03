//
// xemu User Interface - Xbox 360 games through Xenia
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "xenia.hh"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace fs = std::filesystem;

XeniaSettings g_xenia;

namespace {

const size_t kSector = 2048;
const char kMagic[] = "MICROSOFT*XBOX*MEDIA";
// Where the game partition starts in an image: plain XISO, XGD3, XGD2, XGD1
const uint64_t kOffsets[] = { 0, 0x2080000, 0xFD90000, 0x18300000 };

uint16_t Le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
uint32_t Le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
uint32_t Be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return s;
}

class File {
public:
    explicit File(const std::string &path)
        : f(fs::u8path(path), std::ios::binary)
    {
        if (f) {
            f.seekg(0, std::ios::end);
            size = (uint64_t)f.tellg();
        }
    }
    bool Ok() const { return (bool)f; }
    uint64_t Size() const { return size; }
    bool Read(uint64_t off, size_t len, std::vector<uint8_t> &buf)
    {
        if (off + len > size) {
            return false;
        }
        buf.resize(len);
        f.clear();
        f.seekg((std::streamoff)off);
        f.read((char *)buf.data(), (std::streamsize)len);
        return (size_t)f.gcount() == len;
    }

private:
    std::ifstream f;
    uint64_t size = 0;
};

// Walk an XDVDFS directory table for `want` (case-insensitive)
bool FindInDir(const std::vector<uint8_t> &dir, const char *want,
               uint32_t &sector, uint32_t &size)
{
    std::vector<uint32_t> stack = { 0 };
    std::unordered_set<uint32_t> seen;
    size_t want_len = strlen(want);
    while (!stack.empty()) {
        uint32_t pos = stack.back() * 4;
        stack.pop_back();
        if (!seen.insert(pos).second || pos + 14 > dir.size()) {
            continue;
        }
        const uint8_t *e = &dir[pos];
        uint16_t left = Le16(e), right = Le16(e + 2);
        if (left == 0xFFFF && right == 0xFFFF) {
            continue;
        }
        uint8_t n = e[13];
        if (pos + 14 + n <= dir.size() && n == want_len) {
            bool same = true;
            for (size_t i = 0; i < n && same; i++) {
                same = tolower(e[14 + i]) == tolower((unsigned char)want[i]);
            }
            if (same) {
                sector = Le32(e + 4);
                size = Le32(e + 8);
                return true;
            }
        }
        if (left && left != 0xFFFF) {
            stack.push_back(left);
        }
        if (right && right != 0xFFFF) {
            stack.push_back(right);
        }
    }
    return false;
}

// UTF-16BE text to UTF-8
std::string Utf16Be(const uint8_t *p, size_t max_chars)
{
    std::string out;
    for (size_t i = 0; i < max_chars; i++) {
        uint32_t c = (p[i * 2] << 8) | p[i * 2 + 1];
        if (c == 0) {
            break;
        }
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < max_chars) {
            uint32_t lo = (p[(i + 1) * 2] << 8) | p[(i + 1) * 2 + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

bool ReadXexFile(File &f, uint64_t off, uint32_t size, uint32_t *title_id)
{
    std::vector<uint8_t> hdr;
    uint64_t avail = f.Size() > off ? f.Size() - off : 0;
    size_t n = (size_t)std::min<uint64_t>(size ? size : 0x10000, 0x10000);
    n = (size_t)std::min<uint64_t>(n, avail);
    if (!f.Read(off, n, hdr)) {
        return false;
    }
    return X360ParseXex(hdr.data(), hdr.size(), title_id);
}

bool ReadIso(const std::string &path, X360TitleInfo &out)
{
    File f(path);
    if (!f.Ok()) {
        out.error = "Cannot open file";
        return false;
    }
    std::vector<uint8_t> buf;
    uint64_t base = 0;
    bool found = false;
    for (uint64_t off : kOffsets) {
        if (f.Read(off + 32 * kSector, kSector, buf) &&
            memcmp(buf.data(), kMagic, 20) == 0) {
            base = off;
            found = true;
            break;
        }
    }
    if (!found) {
        out.error = "Not a disc image";
        return false;
    }
    uint32_t root_sector = Le32(&buf[20]), root_size = Le32(&buf[24]);
    if (root_size == 0 || root_size > 64 * 1024 * 1024) {
        out.error = "Bad root directory";
        return false;
    }
    std::vector<uint8_t> dir;
    if (!f.Read(base + (uint64_t)root_sector * kSector, root_size, dir)) {
        out.error = "Cannot read root directory";
        return false;
    }
    uint32_t sector, size;
    if (!FindInDir(dir, "default.xex", sector, size)) {
        out.error = "No default.xex";
        return false;
    }
    if (!ReadXexFile(f, base + (uint64_t)sector * kSector, size,
                     &out.title_id)) {
        out.error = "Invalid XEX";
        return false;
    }
    out.kind = "iso";
    out.ok = true;
    return true;
}

bool ReadStfs(const std::string &path, X360TitleInfo &out)
{
    File f(path);
    if (!f.Ok()) {
        out.error = "Cannot open file";
        return false;
    }
    std::vector<uint8_t> h;
    if (!f.Read(0, 0x1800, h)) {
        out.error = "Too small";
        return false;
    }
    if (memcmp(h.data(), "LIVE", 4) && memcmp(h.data(), "PIRS", 4) &&
        memcmp(h.data(), "CON ", 4)) {
        out.error = "Not a package";
        return false;
    }
    uint32_t type = Be32(&h[0x344]);
    // Games on Demand, Xbox Live Arcade, installed game, game download
    if (type != 0x7000 && type != 0xD0000 && type != 0x80000 &&
        type != 0x4000) {
        out.error = "Not a game";
        return false;
    }
    out.title_id = Be32(&h[0x360]);
    if (out.title_id == 0) {
        out.error = "No title ID";
        return false;
    }
    out.title_name = Utf16Be(&h[0x1691], 0x40);
    if (out.title_name.empty()) {
        out.title_name = Utf16Be(&h[0x411], 0x40);
    }
    // Thumbnails: package image at 0x171A, title image at 0x571A
    uint32_t sizes[2] = { Be32(&h[0x1712]), Be32(&h[0x1716]) };
    uint64_t offs[2] = { 0x171A, 0x571A };
    for (int pass = 0; pass < 2 && out.icon_png.empty(); pass++) {
        int i = pass == 0 ? 1 : 0; // the title image first
        if (sizes[i] > 8 && sizes[i] <= 0x4000) {
            std::vector<uint8_t> png;
            static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
            if (f.Read(offs[i], sizes[i], png) && memcmp(png.data(), sig, 8) == 0) {
                out.icon_png = std::move(png);
            }
        }
    }
    out.kind = "god";
    out.ok = true;
    return true;
}

} // namespace

bool X360ParseXex(const uint8_t *d, size_t len, uint32_t *title_id)
{
    if (len < 0x18 || (memcmp(d, "XEX2", 4) && memcmp(d, "XEX1", 4))) {
        return false;
    }
    uint32_t count = Be32(d + 0x14);
    if (count > 256 || 0x18 + (size_t)count * 8 > len) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = d + 0x18 + i * 8;
        if (Be32(e) != 0x00040006) { // execution info
            continue;
        }
        uint32_t off = Be32(e + 4);
        if ((size_t)off + 0x10 > len) {
            return false;
        }
        *title_id = Be32(d + off + 0x0C);
        return true;
    }
    return false;
}

bool X360LooksLikeCandidate(const std::string &filename)
{
    std::string n = Lower(filename);
    size_t dot = n.rfind('.');
    if (dot == std::string::npos) {
        // Games on Demand headers are long extensionless names
        return n.size() >= 8;
    }
    std::string ext = n.substr(dot);
    return ext == ".xex" || ext == ".iso";
}

std::string X360NameFromFile(const std::string &path)
{
    std::string s = fs::u8path(path).stem().u8string();
    // A game folder's default.xex is named after the folder
    if (Lower(s) == "default") {
        s = fs::u8path(path).parent_path().filename().u8string();
    }
    // Drop (region) / [tags] groups
    std::string out;
    int depth = 0;
    for (char c : s) {
        if (c == '(' || c == '[') {
            depth++;
        } else if (c == ')' || c == ']') {
            depth = std::max(0, depth - 1);
        } else if (depth == 0) {
            out += (c == '_') ? ' ' : c;
        }
    }
    while (!out.empty() && isspace((unsigned char)out.back())) {
        out.pop_back();
    }
    size_t a = 0;
    while (a < out.size() && isspace((unsigned char)out[a])) {
        a++;
    }
    out = out.substr(a);
    return out.empty() ? s : out;
}

bool X360ReadTitleInfo(const std::string &path, X360TitleInfo &out)
{
    out = X360TitleInfo();
    std::string name = Lower(fs::u8path(path).filename().u8string());
    size_t dot = name.rfind('.');
    std::string ext = dot == std::string::npos ? "" : name.substr(dot);
    bool ok = false;
    if (ext == ".iso") {
        ok = ReadIso(path, out);
    } else if (ext == ".xex") {
        File f(path);
        ok = f.Ok() && ReadXexFile(f, 0, 0, &out.title_id);
        if (ok) {
            out.kind = "xex";
            out.ok = true;
        } else {
            out.error = "Invalid XEX";
        }
    } else if (ext.empty()) {
        ok = ReadStfs(path, out);
    } else {
        out.error = "Unsupported file";
    }
    if (ok && out.title_name.empty()) {
        out.title_name = X360NameFromFile(path);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Running Xenia
// ---------------------------------------------------------------------------

namespace {

#ifdef _WIN32
HANDLE g_proc = NULL;
DWORD g_pid_win = 0;
HWND g_embed_wnd = NULL;      // Xenia's window while it is inside ours
HWND g_embed_parent = NULL;
bool g_embed_focus = false;
int g_embed_tries = 0;
RECT g_embed_rect = { 0, 0, 0, 0 };
int g_embed_top = 0;
ULONGLONG g_stop_deadline = 0;   // after a polite close request, end it here
ULONGLONG g_pad_hold = 0;        // when Back + Start were pressed together

typedef struct {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
} PadState;
typedef DWORD(WINAPI *PadGetState)(DWORD, PadState *);

// Back + Start on any Xbox controller, read straight from Windows so it works
// while the game window has the focus
bool PadComboHeld()
{
    static PadGetState get = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (!m) {
            m = LoadLibraryA("xinput9_1_0.dll");
        }
        if (m) {
            get = (PadGetState)GetProcAddress(m, "XInputGetState");
        }
    }
    if (!get) {
        return false;
    }
    for (DWORD i = 0; i < 4; i++) {
        PadState st;
        memset(&st, 0, sizeof(st));
        if (get(i, &st) == 0 && (st.buttons & 0x30) == 0x30) { // Back and Start
            return true;
        }
    }
    return false;
}

bool OurProcessHasFocus()
{
    HWND fg = GetForegroundWindow();
    if (!fg) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() || pid == g_pid_win;
}

struct FindCtx {
    DWORD pid;
    HWND found;
};

BOOL CALLBACK FindXeniaWindow(HWND h, LPARAM lp)
{
    FindCtx *c = (FindCtx *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER) != NULL) {
        return TRUE;
    }
    char cls[64] = { 0 };
    GetClassNameA(h, cls, sizeof(cls) - 1);
    if (strcmp(cls, "ConsoleWindowClass") == 0) {
        return TRUE;
    }
    RECT r;
    if (!GetWindowRect(h, &r) || r.right - r.left < 200 || r.bottom - r.top < 150) {
        return TRUE; // not the game window (splash, tool windows)
    }
    c->found = h;
    return FALSE;
}

void EmbedRelease()
{
    if (g_embed_parent && IsWindow(g_embed_parent)) {
        SetForegroundWindow(g_embed_parent);
    }
    g_embed_wnd = NULL;
    g_embed_parent = NULL;
}
#else
pid_t g_pid = 0;
#endif
bool g_running = false;
std::string g_path;
std::chrono::steady_clock::time_point g_started;

std::vector<std::string> SplitArgs(const std::string &s)
{
    std::vector<std::string> out;
    std::string cur;
    bool in = false, quoted = false;
    for (char c : s) {
        if (c == '"') {
            quoted = !quoted;
            in = true;
        } else if (isspace((unsigned char)c) && !quoted) {
            if (in) {
                out.push_back(cur);
                cur.clear();
                in = false;
            }
        } else {
            cur += c;
            in = true;
        }
    }
    if (in) {
        out.push_back(cur);
    }
    return out;
}

} // namespace

bool XeniaConfigured()
{
    if (g_xenia.exe.empty()) {
        return false;
    }
    std::error_code ec;
    return fs::is_regular_file(fs::u8path(g_xenia.exe), ec);
}

bool XeniaIsRunning() { return g_running; }
std::string XeniaRunningPath() { return g_running ? g_path : std::string(); }

#ifdef _WIN32
static std::wstring Widen(const std::string &s)
{
    if (s.empty()) {
        return L"";
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring Quote(const std::wstring &a)
{
    return L"\"" + a + L"\"";
}
#endif

std::vector<std::string> XeniaBuildFlags(const XeniaSettings &d,
                                         const XeniaGameSettings *game)
{
    const bool own = game && game->enabled;
    int res = own && game->res_scale > 0 ? game->res_scale : d.res_scale;
    int gpu = own && game->gpu > 0 ? game->gpu : d.gpu;
    int vsync = own && game->vsync >= 0 ? game->vsync : d.vsync;
    int full = own && game->fullscreen >= 0 ? game->fullscreen : (d.fullscreen ? 1 : -1);
    if (d.embed) {
        full = 0; // inside our window it fills the window; it must not go fullscreen
    }
    std::vector<std::string> out;
    if (full >= 0) {
        out.push_back(full ? "--fullscreen=true" : "--fullscreen=false");
    }
    if (res >= 1 && res <= 8) {
        out.push_back("--draw_resolution_scale_x=" + std::to_string(res));
        out.push_back("--draw_resolution_scale_y=" + std::to_string(res));
    }
    if (gpu == 1) {
        out.push_back("--gpu=d3d12");
    } else if (gpu == 2) {
        out.push_back("--gpu=vulkan");
    }
    if (vsync >= 0) {
        out.push_back(vsync ? "--vsync=true" : "--vsync=false");
    }
    for (auto &a : SplitArgs(d.extra_args)) {
        out.push_back(a);
    }
    if (own) {
        for (auto &a : SplitArgs(game->extra_args)) {
            out.push_back(a);
        }
    }
    return out;
}

bool XeniaLaunch(const std::string &game_path, std::string *err,
                 const XeniaGameSettings *game)
{
    auto fail = [&](const char *m) {
        if (err) {
            *err = m;
        }
        return false;
    };
    if (g_running) {
        return fail("Xenia is already running");
    }
    if (!XeniaConfigured()) {
        return fail("Set the path to Xenia in the library settings first");
    }
    std::error_code ec;
    if (!fs::exists(fs::u8path(game_path), ec)) {
        return fail("The game file is missing");
    }
    XeniaSettings eff = g_xenia;
    eff.embed = eff.embed && XeniaEmbedSupported();
    std::vector<std::string> extra = XeniaBuildFlags(eff, game);
    fs::path exe = fs::u8path(g_xenia.exe);
    fs::path cwd = exe.parent_path(); // Xenia keeps its portable data here

#ifdef _WIN32
    std::wstring cmd = Quote(exe.wstring());
    for (auto &a : extra) {
        cmd += L" " + Quote(Widen(a));
    }
    cmd += L" " + Quote(fs::u8path(game_path).wstring());
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(exe.wstring().c_str(), &cmd[0], NULL, NULL, FALSE, 0,
                        NULL, cwd.empty() ? NULL : cwd.wstring().c_str(), &si,
                        &pi)) {
        return fail("Could not start Xenia");
    }
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    g_pid_win = pi.dwProcessId;
    g_embed_wnd = NULL;
    g_embed_tries = 0;
#else
    std::vector<std::string> args;
    std::string ext = Lower(exe.extension().u8string());
    if (ext == ".exe") {
        args.push_back("wine"); // a Windows build of Xenia on Linux
    }
    args.push_back(exe.u8string());
    for (auto &a : extra) {
        args.push_back(a);
    }
    args.push_back(game_path);
    std::vector<char *> argv;
    for (auto &a : args) {
        argv.push_back(const_cast<char *>(a.c_str()));
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
    // Xenia's own folder is its working directory
    posix_spawn_file_actions_addchdir_np(&fa, cwd.u8string().c_str());
#endif
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        return fail(ext == ".exe" ? "Could not start Xenia (is Wine installed?)"
                                  : "Could not start Xenia");
    }
    g_pid = pid;
#endif
    g_running = true;
    g_path = game_path;
    g_started = std::chrono::steady_clock::now();
    return true;
}

bool XeniaEmbedActive()
{
#ifdef _WIN32
    return g_running && g_embed_wnd && IsWindow(g_embed_wnd);
#else
    return false;
#endif
}

bool XeniaEmbedSupported()
{
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

void XeniaEmbedTick(void *parent_hwnd, float top_units, float display_w)
{
#ifdef _WIN32
    HWND parent = (HWND)parent_hwnd;
    if (!g_running || !parent || !IsWindow(parent)) {
        return;
    }
    RECT pc;
    GetClientRect(parent, &pc);
    int w = pc.right - pc.left, h = pc.bottom - pc.top;
    if (w < 16 || h < 16 || IsIconic(parent)) {
        // Minimised: an owned window goes with its owner, any other is hidden
        if (g_embed_wnd && IsWindow(g_embed_wnd) &&
            GetWindow(g_embed_wnd, GW_OWNER) != parent) {
            ShowWindow(g_embed_wnd, SW_HIDE);
            g_embed_rect = { 0, 0, 0, 0 };
        }
        return;
    }
    int top = display_w > 1 ? (int)(top_units * (float)w / display_w + 0.5f) : 0;
    if (top < 0 || top > h / 3) {
        top = 0;
    }
    if (g_embed_wnd && !IsWindow(g_embed_wnd)) {
        g_embed_wnd = NULL;
        return;
    }
    if (!g_embed_wnd) {
        // Xenia needs a moment to open its window; look a few times a second
        // for about a minute
        static int skip = 0;
        if (g_embed_tries > 1800 || (++skip % 10) != 0) {
            g_embed_tries++;
            return;
        }
        g_embed_tries++;
        FindCtx c = { g_pid_win, NULL };
        EnumWindows(FindXeniaWindow, (LPARAM)&c);
        if (!c.found) {
            return;
        }
        HWND x = c.found;
        // Xenia keeps its own window (a child window of ours can end up hidden
        // behind xemu's picture), made borderless, owned by xemu's window and
        // kept exactly over it. An owned window stays above its owner and
        // follows it when it is minimised.
        LONG_PTR st = GetWindowLongPtrW(x, GWL_STYLE);
        st &= ~(WS_CHILD | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZE |
                WS_MAXIMIZE | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        st |= WS_POPUP | WS_VISIBLE;
        SetWindowLongPtrW(x, GWL_STYLE, st);
        LONG_PTR ex = GetWindowLongPtrW(x, GWL_EXSTYLE);
        ex &= ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE |
                WS_EX_DLGMODALFRAME);
        ex |= WS_EX_TOOLWINDOW; // no taskbar button or Alt+Tab entry of its own
        SetWindowLongPtrW(x, GWL_EXSTYLE, ex);
        SetLastError(0);
        SetWindowLongPtrW(x, GWLP_HWNDPARENT, (LONG_PTR)parent);
        g_embed_wnd = x;
        g_embed_parent = parent;
        g_embed_rect = { 0, 0, 0, 0 };
        g_embed_top = -1;
        g_embed_focus = true;
    }
    // Where Xenia belongs: the client area of xemu's window below the strip
    POINT org = { 0, top };
    ClientToScreen(parent, &org);
    RECT want = { org.x, org.y, org.x + w, org.y + (h - top) };
    bool owned = GetWindow(g_embed_wnd, GW_OWNER) == parent;
    // If the owner could not be set: stay on top while xemu or Xenia is the
    // window in use, and step back when something else is
    HWND after = HWND_TOP;
    if (!owned) {
        after = OurProcessHasFocus() ? HWND_TOPMOST : HWND_NOTOPMOST;
    }
    static HWND last_after = NULL;
    if (after != last_after || want.left != g_embed_rect.left || want.top != g_embed_rect.top ||
        want.right != g_embed_rect.right || want.bottom != g_embed_rect.bottom ||
        top != g_embed_top) {
        last_after = after;
        SetWindowPos(g_embed_wnd, after, want.left, want.top, want.right - want.left,
                     want.bottom - want.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        if (g_embed_focus) {
            g_embed_focus = false;
            SetForegroundWindow(g_embed_wnd);
        }
        g_embed_rect = want;
        g_embed_top = top;
    }
    // Ways out that work while Xenia has the keyboard and the controller:
    // Ctrl+Shift+Q, or Back and Start together for a second
    if (OurProcessHasFocus()) {
        bool keys = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                    (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                    (GetAsyncKeyState('Q') & 0x8000);
        bool pad = PadComboHeld();
        if (pad) {
            if (!g_pad_hold) {
                g_pad_hold = GetTickCount64();
            }
        } else {
            g_pad_hold = 0;
        }
        if (keys || (g_pad_hold && GetTickCount64() - g_pad_hold > 1000)) {
            g_pad_hold = 0;
            XeniaStop();
        }
    }
#else
    (void)parent_hwnd;
#endif
}

bool XeniaPoll(std::string *path, double *seconds)
{
    if (!g_running) {
        return false;
    }
    bool done = false;
#ifdef _WIN32
    done = WaitForSingleObject(g_proc, 0) != WAIT_TIMEOUT;
    if (!done && g_stop_deadline && GetTickCount64() > g_stop_deadline) {
        TerminateProcess(g_proc, 0);
        g_stop_deadline = 0;
    }
    if (done) {
        g_stop_deadline = 0;
        CloseHandle(g_proc);
        g_proc = NULL;
        EmbedRelease();
    }
#else
    int status = 0;
    pid_t r = waitpid(g_pid, &status, WNOHANG);
    done = (r == g_pid) || (r < 0 && errno != EINTR);
    if (done) {
        g_pid = 0;
    }
#endif
    if (!done) {
        return false;
    }
    g_running = false;
    if (path) {
        *path = g_path;
    }
    if (seconds) {
        *seconds = std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - g_started).count();
    }
    g_path.clear();
    return true;
}

void XeniaStop()
{
    if (!g_running) {
        return;
    }
#ifdef _WIN32
    // Ask Xenia to close like its own close button would; end it if it hangs
    HWND w = g_embed_wnd;
    if (!w) {
        FindCtx c = { g_pid_win, NULL };
        EnumWindows(FindXeniaWindow, (LPARAM)&c);
        w = c.found;
    }
    if (w) {
        PostMessageW(w, WM_CLOSE, 0, 0);
        g_stop_deadline = GetTickCount64() + 5000;
    } else {
        TerminateProcess(g_proc, 0);
    }
#else
    kill(g_pid, SIGTERM);
#endif
}
