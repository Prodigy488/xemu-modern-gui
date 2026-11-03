//
// xemu User Interface - turning songs into what the Xbox dashboard plays
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "soundtrack-convert.hh"
#include "soundtrack-db.hh"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "../xemu-settings.h"

namespace fs = std::filesystem;

namespace
{
std::string g_chosen_ffmpeg;
bool g_chosen_loaded = false;

fs::path ChosenFile()
{
    return fs::u8path(xemu_settings_get_base_path()) / "soundtrack-ffmpeg.txt";
}

void LoadChosen()
{
    if (g_chosen_loaded) {
        return;
    }
    g_chosen_loaded = true;
    std::ifstream f(ChosenFile());
    std::getline(f, g_chosen_ffmpeg);
    while (!g_chosen_ffmpeg.empty() &&
           (g_chosen_ffmpeg.back() == '\r' || g_chosen_ffmpeg.back() == '\n')) {
        g_chosen_ffmpeg.pop_back();
    }
}

bool IsFile(const fs::path &p)
{
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

#ifdef _WIN32
std::wstring Wide(const std::string &s)
{
    if (s.empty()) {
        return L"";
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
#endif

std::string LastLines(const std::string &log_path)
{
    std::ifstream f(fs::u8path(log_path), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    if (text.size() > 300) {
        text = text.substr(text.size() - 300);
    }
    return text;
}

// Run a program to the end with its output going to log_path. False if it
// couldn't be started; *code is its exit status (-1 if it was cancelled).
bool RunProgram(const std::vector<std::string> &args, const std::string &log_path,
                const std::atomic<bool> *cancel, int *code)
{
#ifdef _WIN32
    std::wstring cmd;
    for (size_t i = 0; i < args.size(); i++) {
        if (i) {
            cmd += L' ';
        }
        std::wstring a = Wide(args[i]);
        cmd += L'"';
        size_t backslashes = 0;
        for (wchar_t c : a) {
            if (c == L'\\') {
                backslashes++;
                cmd += c;
                continue;
            }
            if (c == L'"') {
                cmd.append(backslashes + 1, L'\\');
            }
            backslashes = 0;
            cmd += c;
        }
        cmd.append(backslashes, L'\\');
        cmd += L'"';
    }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE log = CreateFileW(fs::u8path(log_path).wstring().c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = log;
    si.hStdError = log;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    if (!ok) {
        if (log != INVALID_HANDLE_VALUE) {
            CloseHandle(log);
        }
        return false;
    }
    for (;;) {
        DWORD w = WaitForSingleObject(pi.hProcess, 50);
        if (w == WAIT_OBJECT_0) {
            break;
        }
        if (cancel && cancel->load()) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            *code = -1;
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            if (log != INVALID_HANDLE_VALUE) {
                CloseHandle(log);
            }
            return true;
        }
    }
    DWORD ec = 1;
    GetExitCodeProcess(pi.hProcess, &ec);
    *code = (int)ec;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }
    return true;
#else
    std::vector<char *> argv;
    for (auto &a : args) {
        argv.push_back(const_cast<char *>(a.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) {
            dup2(nul, 0);
        }
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }
    for (;;) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            *code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
            return *code != 127;
        }
        if (cancel && cancel->load()) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            *code = -1;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif
}

// ---- Windows Media Foundation ----------------------------------------------

#ifdef _WIN32
// MinGW's headers only declare the transcode profile, so it is spelled out here
// (same method order as the Windows SDK's IMFTranscodeProfile)
struct IMyTranscodeProfile : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetAudioAttributes(IMFAttributes *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAudioAttributes(IMFAttributes **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVideoAttributes(IMFAttributes *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVideoAttributes(IMFAttributes **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetContainerAttributes(IMFAttributes *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetContainerAttributes(IMFAttributes **) = 0;
};

// kMfTranscodeContainerType, kMfContainerAsf and
// kMfAudioWma2 (the WAVE format tag 0x0161)
static const GUID kMfTranscodeContainerType = { 0x150ff23f, 0x4abc, 0x478b,
    { 0xac, 0x4f, 0xe1, 0x91, 0x6f, 0xba, 0x1c, 0xca } };
static const GUID kMfContainerAsf = { 0x430f6f6e, 0xb6bf, 0x4fc1,
    { 0xa8, 0xf5, 0xe5, 0xdb, 0x9a, 0xde, 0x40, 0xf9 } };
static const GUID kMfAudioWma2 = { 0x00000161, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

typedef HRESULT(WINAPI *PFN_Startup)(ULONG, DWORD);
typedef HRESULT(WINAPI *PFN_Shutdown)(void);
typedef HRESULT(WINAPI *PFN_CreateAttributes)(IMFAttributes **, UINT32);
typedef HRESULT(WINAPI *PFN_CreateSourceResolver)(IMFSourceResolver **);
typedef HRESULT(WINAPI *PFN_CreateTranscodeProfile)(IMyTranscodeProfile **);
typedef HRESULT(WINAPI *PFN_CreateTranscodeTopology)(IMFMediaSource *, LPCWSTR,
                                                     IMyTranscodeProfile *, IMFTopology **);
typedef HRESULT(WINAPI *PFN_CreateMediaSession)(IMFAttributes *, IMFMediaSession **);
typedef HRESULT(WINAPI *PFN_GetOutputTypes)(REFGUID, DWORD, IMFAttributes *, IMFCollection **);

struct MfApi {
    bool ok = false;
    PFN_Startup Startup = nullptr;
    PFN_Shutdown Shutdown = nullptr;
    PFN_CreateAttributes CreateAttributes = nullptr;
    PFN_CreateSourceResolver CreateSourceResolver = nullptr;
    PFN_CreateTranscodeProfile CreateTranscodeProfile = nullptr;
    PFN_CreateTranscodeTopology CreateTranscodeTopology = nullptr;
    PFN_CreateMediaSession CreateMediaSession = nullptr;
    PFN_GetOutputTypes GetOutputTypes = nullptr;
};

// The DLLs are loaded when needed so xemu still starts on Windows N editions
const MfApi &Mf()
{
    static MfApi api;
    static bool tried = false;
    if (tried) {
        return api;
    }
    tried = true;
    HMODULE plat = LoadLibraryW(L"mfplat.dll");
    HMODULE mf = LoadLibraryW(L"mf.dll");
    if (!plat || !mf) {
        return api;
    }
    api.Startup = (PFN_Startup)GetProcAddress(plat, "MFStartup");
    api.Shutdown = (PFN_Shutdown)GetProcAddress(plat, "MFShutdown");
    api.CreateAttributes = (PFN_CreateAttributes)GetProcAddress(plat, "MFCreateAttributes");
    api.CreateSourceResolver =
        (PFN_CreateSourceResolver)GetProcAddress(mf, "MFCreateSourceResolver");
    api.CreateTranscodeProfile =
        (PFN_CreateTranscodeProfile)GetProcAddress(mf, "MFCreateTranscodeProfile");
    api.CreateTranscodeTopology =
        (PFN_CreateTranscodeTopology)GetProcAddress(mf, "MFCreateTranscodeTopology");
    api.CreateMediaSession = (PFN_CreateMediaSession)GetProcAddress(mf, "MFCreateMediaSession");
    api.GetOutputTypes =
        (PFN_GetOutputTypes)GetProcAddress(mf, "MFTranscodeGetAudioOutputAvailableTypes");
    api.ok = api.Startup && api.Shutdown && api.CreateAttributes && api.CreateSourceResolver &&
             api.CreateTranscodeProfile && api.CreateTranscodeTopology &&
             api.CreateMediaSession && api.GetOutputTypes;
    return api;
}

template <typename T>
void Rel(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool MfConvert(const std::string &in, const std::string &out, std::string *err)
{
    const MfApi &api = Mf();
    if (!api.ok) {
        *err = "Windows Media Foundation isn't available on this PC";
        return false;
    }
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool co_done = SUCCEEDED(co);
    bool started = SUCCEEDED(api.Startup(MF_VERSION, MFSTARTUP_FULL));
    IMFSourceResolver *resolver = nullptr;
    IUnknown *unk = nullptr;
    IMFMediaSource *source = nullptr;
    IMFCollection *types = nullptr;
    IMFMediaType *chosen = nullptr;
    IMyTranscodeProfile *profile = nullptr;
    IMFAttributes *container = nullptr;
    IMFTopology *topology = nullptr;
    IMFMediaSession *session = nullptr;
    bool ok = false;
    std::wstring win = fs::u8path(in).wstring();
    std::wstring wout = fs::u8path(out).wstring();
    do {
        if (!started) {
            *err = "Windows Media Foundation couldn't start";
            break;
        }
        if (FAILED(api.CreateSourceResolver(&resolver))) {
            *err = "Windows Media Foundation couldn't start";
            break;
        }
        MF_OBJECT_TYPE ot = MF_OBJECT_INVALID;
        HRESULT hr = resolver->CreateObjectFromURL(win.c_str(), MF_RESOLUTION_MEDIASOURCE,
                                                   nullptr, &ot, &unk);
        if (FAILED(hr) || !unk) {
            *err = "Windows can't read this song (unsupported format?)";
            break;
        }
        if (FAILED(unk->QueryInterface(IID_IMFMediaSource, (void **)&source))) {
            *err = "Windows can't read this song";
            break;
        }
        hr = api.GetOutputTypes(kMfAudioWma2, MFT_ENUM_FLAG_ALL, nullptr, &types);
        DWORD count = 0;
        if (FAILED(hr) || !types || FAILED(types->GetElementCount(&count)) || !count) {
            *err = "This Windows has no WMA encoder (is it an N edition?). "
                   "Put ffmpeg next to xemu or choose it above instead.";
            break;
        }
        // The format the Xbox dashboard makes: 44.1 kHz, stereo, 16-bit, 128 kbps.
        // Take the nearest the encoder offers.
        long best_score = -1;
        for (DWORD i = 0; i < count; i++) {
            IUnknown *item = nullptr;
            if (FAILED(types->GetElement(i, &item)) || !item) {
                continue;
            }
            IMFMediaType *mt = nullptr;
            if (SUCCEEDED(item->QueryInterface(IID_IMFMediaType, (void **)&mt))) {
                UINT32 rate = 0, ch = 0, bits = 0, avg = 0;
                mt->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                mt->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                mt->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
                mt->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &avg);
                long score = 0;
                score += rate == 44100 ? 0 : 1000000;
                score += ch == 2 ? 0 : 100000;
                score += bits == 16 ? 0 : 10000;
                score += labs((long)avg - 16000);
                if (best_score < 0 || score < best_score) {
                    best_score = score;
                    Rel(chosen);
                    chosen = mt;
                    mt = nullptr;
                }
                Rel(mt);
            }
            item->Release();
        }
        if (!chosen) {
            *err = "The Windows WMA encoder didn't offer a usable format";
            break;
        }
        if (FAILED(api.CreateTranscodeProfile(&profile)) ||
            FAILED(profile->SetAudioAttributes(chosen)) ||
            FAILED(api.CreateAttributes(&container, 1)) ||
            FAILED(container->SetGUID(kMfTranscodeContainerType, kMfContainerAsf)) ||
            FAILED(profile->SetContainerAttributes(container))) {
            *err = "Windows couldn't set up the WMA conversion";
            break;
        }
        std::error_code ec;
        fs::remove(fs::u8path(out), ec);
        hr = api.CreateTranscodeTopology(source, wout.c_str(), profile, &topology);
        if (FAILED(hr) || !topology) {
            *err = "Windows can't convert this song to WMA";
            break;
        }
        if (FAILED(api.CreateMediaSession(nullptr, &session)) ||
            FAILED(session->SetTopology(0, topology))) {
            *err = "Windows couldn't start the conversion";
            break;
        }
        GUID null_guid = {};
        PROPVARIANT start;
        PropVariantInit(&start);
        if (FAILED(session->Start(&null_guid, &start))) {
            *err = "Windows couldn't start the conversion";
            break;
        }
        bool done = false;
        while (!done) {
            IMFMediaEvent *ev = nullptr;
            if (FAILED(session->GetEvent(0, &ev)) || !ev) {
                *err = "The conversion stopped unexpectedly";
                break;
            }
            MediaEventType type = MEUnknown;
            HRESULT status = S_OK;
            ev->GetType(&type);
            ev->GetStatus(&status);
            ev->Release();
            if (FAILED(status)) {
                *err = "Windows couldn't convert this song";
                break;
            }
            if (type == MESessionEnded) {
                ok = true;
                done = true;
            } else if (type == MEError) {
                *err = "Windows couldn't convert this song";
                break;
            }
        }
        if (session) {
            session->Close();
        }
    } while (false);
    if (session) {
        session->Shutdown();
    }
    if (source) {
        source->Shutdown();
    }
    Rel(session);
    Rel(topology);
    Rel(container);
    Rel(profile);
    Rel(chosen);
    Rel(types);
    Rel(source);
    Rel(unk);
    Rel(resolver);
    if (started) {
        api.Shutdown();
    }
    if (co_done) {
        CoUninitialize();
    }
    return ok;
}
#endif // _WIN32

uint32_t BE32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

uint32_t Synchsafe(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((p[1] & 0x7F) << 14) | ((p[2] & 0x7F) << 7) |
           (p[3] & 0x7F);
}

std::string Latin1ToUtf8(const uint8_t *p, size_t n)
{
    std::string out;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == 0) {
            break;
        }
        if (p[i] < 0x80) {
            out.push_back((char)p[i]);
        } else {
            out.push_back((char)(0xC0 | (p[i] >> 6)));
            out.push_back((char)(0x80 | (p[i] & 0x3F)));
        }
    }
    return out;
}

// An ID3 text frame: encoding byte, then the text
std::string Id3Text(const uint8_t *p, size_t n)
{
    if (n < 2) {
        return "";
    }
    int enc = p[0];
    p++;
    n--;
    if (enc == 0) {
        return Latin1ToUtf8(p, n);
    }
    if (enc == 3) {
        std::string s((const char *)p, n);
        size_t z = s.find('\0');
        return z == std::string::npos ? s : s.substr(0, z);
    }
    // UTF-16, with a byte order mark or big-endian
    bool le = false;
    if (enc == 1 && n >= 2) {
        if (p[0] == 0xFF && p[1] == 0xFE) {
            le = true;
            p += 2;
            n -= 2;
        } else if (p[0] == 0xFE && p[1] == 0xFF) {
            p += 2;
            n -= 2;
        }
    }
    std::u16string u;
    for (size_t i = 0; i + 1 < n; i += 2) {
        char16_t c = le ? (char16_t)(p[i] | (p[i + 1] << 8)) : (char16_t)((p[i] << 8) | p[i + 1]);
        if (!c) {
            break;
        }
        u.push_back(c);
    }
    return StUtf16ToUtf8(u);
}

std::string ReadId3Title(const std::string &path)
{
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) {
        return "";
    }
    uint8_t h[10];
    f.read((char *)h, 10);
    if (f.gcount() == 10 && !memcmp(h, "ID3", 3) && h[3] >= 2 && h[3] <= 4) {
        uint32_t size = Synchsafe(h + 6);
        if (size > (8u << 20)) {
            size = 8u << 20;
        }
        std::vector<uint8_t> tag(size);
        f.read((char *)tag.data(), size);
        size = (uint32_t)f.gcount();
        size_t pos = 0;
        int ver = h[3];
        size_t hdr = ver == 2 ? 6 : 10;
        while (pos + hdr <= size) {
            const uint8_t *fr = &tag[pos];
            if (fr[0] == 0) {
                break;
            }
            uint32_t fsz = ver == 2 ? (uint32_t)((fr[3] << 16) | (fr[4] << 8) | fr[5])
                           : ver == 4 ? Synchsafe(fr + 4)
                                      : BE32(fr + 4);
            if (pos + hdr + fsz > size) {
                break;
            }
            bool title = ver == 2 ? !memcmp(fr, "TT2", 3) : !memcmp(fr, "TIT2", 4);
            if (title) {
                std::string t = Id3Text(fr + hdr, fsz);
                if (!t.empty()) {
                    return t;
                }
            }
            pos += hdr + fsz;
        }
    }
    // ID3v1 at the end of the file
    f.clear();
    f.seekg(-128, std::ios::end);
    uint8_t v1[128];
    f.read((char *)v1, 128);
    if (f.gcount() == 128 && !memcmp(v1, "TAG", 3)) {
        std::string t = Latin1ToUtf8(v1 + 3, 30);
        while (!t.empty() && t.back() == ' ') {
            t.pop_back();
        }
        return t;
    }
    return "";
}
} // namespace

std::string SoundtrackChosenFfmpeg()
{
    LoadChosen();
    return g_chosen_ffmpeg;
}

void SoundtrackSetFfmpeg(const std::string &path)
{
    LoadChosen();
    g_chosen_ffmpeg = path;
    std::ofstream f(ChosenFile(), std::ios::trunc);
    f << path << "\n";
}

std::string SoundtrackFindFfmpeg()
{
    LoadChosen();
    if (!g_chosen_ffmpeg.empty() && IsFile(fs::u8path(g_chosen_ffmpeg))) {
        return g_chosen_ffmpeg;
    }
#ifdef _WIN32
    const char *name = "ffmpeg.exe";
#else
    const char *name = "ffmpeg";
#endif
    if (const char *base = SDL_GetBasePath()) {
        fs::path p = fs::u8path(base) / name;
        if (IsFile(p)) {
            return p.u8string();
        }
    }
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr);
    if (n > 0 && n < MAX_PATH) {
        return fs::path(buf).u8string();
    }
#else
    if (const char *path = getenv("PATH")) {
        std::string s = path;
        size_t start = 0;
        while (start <= s.size()) {
            size_t end = s.find(':', start);
            if (end == std::string::npos) {
                end = s.size();
            }
            std::string dir = s.substr(start, end - start);
            if (!dir.empty()) {
                fs::path p = fs::u8path(dir) / name;
                if (IsFile(p)) {
                    return p.u8string();
                }
            }
            start = end + 1;
        }
    }
#endif
    return "";
}

bool SoundtrackCanConvert(std::string *how)
{
    if (!SoundtrackFindFfmpeg().empty()) {
        *how = "ffmpeg";
        return true;
    }
#ifdef _WIN32
    if (Mf().ok) {
        *how = "Windows Media Foundation";
        return true;
    }
#endif
    how->clear();
    return false;
}

bool SoundtrackConvert(const std::string &in, const std::string &out, std::string *err,
                       const std::atomic<bool> *cancel)
{
    std::string ffmpeg = SoundtrackFindFfmpeg();
    std::error_code ec;
    fs::remove(fs::u8path(out), ec);
    if (!ffmpeg.empty()) {
        std::string log = out + ".log";
        int code = 0;
        std::vector<std::string> args = {
            ffmpeg, "-y", "-nostdin", "-v", "error", "-i", in, "-vn", "-map_metadata", "-1",
            "-c:a", "wmav2", "-b:a", "128k", "-ar", "44100", "-ac", "2", "-f", "asf", out
        };
        if (!RunProgram(args, log, cancel, &code)) {
            *err = "Couldn't start ffmpeg (" + ffmpeg + ")";
            return false;
        }
        if (code == -1) {
            *err = "Cancelled";
            fs::remove(fs::u8path(out), ec);
            fs::remove(fs::u8path(log), ec);
            return false;
        }
        bool ok = code == 0 && IsFile(fs::u8path(out));
        if (!ok) {
            std::string why = LastLines(log);
            *err = why.empty() ? "ffmpeg couldn't convert this song" : why;
            fs::remove(fs::u8path(out), ec);
        }
        fs::remove(fs::u8path(log), ec);
        return ok;
    }
#ifdef _WIN32
    if (Mf().ok) {
        bool ok = MfConvert(in, out, err);
        if (!ok) {
            fs::remove(fs::u8path(out), ec);
        }
        return ok;
    }
#endif
    *err = "No converter found. Put ffmpeg next to xemu, or choose it in this window.";
    return false;
}

bool SoundtrackWmaLengthMs(const std::string &path, uint32_t *ms)
{
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) {
        return false;
    }
    std::vector<uint8_t> b(1 << 16);
    f.read((char *)b.data(), b.size());
    size_t n = (size_t)f.gcount();
    // ASF header object, then its objects; the File Properties object has the
    // play duration (100 ns units) and the preroll (ms)
    static const uint8_t kHeader[16] = { 0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                         0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C };
    static const uint8_t kFileProps[16] = { 0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
                                            0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65 };
    if (n < 30 || memcmp(b.data(), kHeader, 16)) {
        return false;
    }
    size_t pos = 30;
    while (pos + 24 <= n) {
        uint64_t size;
        memcpy(&size, &b[pos + 16], 8);
        if (!memcmp(&b[pos], kFileProps, 16)) {
            if (pos + 24 + 64 > n) {
                return false;
            }
            uint64_t play, preroll;
            memcpy(&play, &b[pos + 24 + 40], 8);
            memcpy(&preroll, &b[pos + 24 + 56], 8);
            uint64_t total = play / 10000;
            total = total > preroll ? total - preroll : 0;
            *ms = (uint32_t)std::min<uint64_t>(total, 0xFFFFFFFFull);
            return true;
        }
        if (size < 24) {
            return false;
        }
        pos += (size_t)size;
    }
    return false;
}

std::string SoundtrackTitleOf(const std::string &path)
{
    std::string t = ReadId3Title(path);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\0')) {
        t.pop_back();
    }
    if (!t.empty()) {
        return t;
    }
    return fs::u8path(path).stem().u8string();
}
