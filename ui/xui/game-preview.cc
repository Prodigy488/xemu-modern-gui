//
// xemu User Interface - Game library preview videos
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "game-preview.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif

#include <epoxy/gl.h>

const char *const kPreviewExtensions[] = { ".mp4", ".m4v", ".wmv", ".avi",
                                           ".mov", ".mkv", nullptr };

struct PreviewPlayer::Impl {
    std::string path;
    std::thread thread;
    std::atomic<bool> stop{ false };

    std::mutex lock;
    std::vector<uint8_t> frame; // RGBA, written by the decoder
    int frame_w = 0, frame_h = 0;
    bool frame_new = false;
    std::string error;

    GLuint tex = 0;
    int tex_w = 0, tex_h = 0;

    void Publish(const uint8_t *src, int w, int h, long pitch, bool bgrx);
    void SetError(const std::string &e)
    {
        std::lock_guard<std::mutex> g(lock);
        error = e;
    }
    void Run();
};

// Copy a decoded frame (BGRX rows, any pitch, possibly bottom-up) into the
// shared RGBA buffer, downscaling very large videos to keep uploads cheap.
void PreviewPlayer::Impl::Publish(const uint8_t *src, int w, int h, long pitch,
                                  bool bgrx)
{
    int step = 1;
    while (w / step > 1280) {
        step++;
    }
    int ow = w / step, oh = h / step;
    std::lock_guard<std::mutex> g(lock);
    frame.resize((size_t)ow * oh * 4);
    for (int y = 0; y < oh; y++) {
        const uint8_t *row = src + (long)(y * step) * pitch;
        uint8_t *dst = &frame[(size_t)y * ow * 4];
        for (int x = 0; x < ow; x++) {
            const uint8_t *p = row + (size_t)x * step * 4;
            dst[x * 4 + 0] = bgrx ? p[2] : p[0];
            dst[x * 4 + 1] = p[1];
            dst[x * 4 + 2] = bgrx ? p[0] : p[2];
            dst[x * 4 + 3] = 255;
        }
    }
    frame_w = ow;
    frame_h = oh;
    frame_new = true;
}

#ifdef _WIN32

template <class T> static void SafeRelease(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Media Foundation is loaded on first use instead of linked, so xemu still
// starts on Windows "N" editions that ship without it.
struct MfApi {
    bool loaded = false;
    HRESULT(WINAPI *Startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI *Shutdown)() = nullptr;
    HRESULT(WINAPI *CreateAttributes)(IMFAttributes **, UINT32) = nullptr;
    HRESULT(WINAPI *CreateMediaType)(IMFMediaType **) = nullptr;
    HRESULT(WINAPI *CreateSourceReaderFromURL)(LPCWSTR, IMFAttributes *,
                                               IMFSourceReader **) = nullptr;
};

static bool LoadMf(MfApi &api)
{
    static std::mutex load_lock;
    static MfApi cached;
    static bool tried = false;
    std::lock_guard<std::mutex> g(load_lock);
    if (!tried) {
        tried = true;
        HMODULE plat = LoadLibraryW(L"mfplat.dll");
        HMODULE rw = LoadLibraryW(L"mfreadwrite.dll");
        if (plat && rw) {
            cached.Startup = (decltype(cached.Startup))(void (*)(void))
                GetProcAddress(plat, "MFStartup");
            cached.Shutdown = (decltype(cached.Shutdown))(void (*)(void))
                GetProcAddress(plat, "MFShutdown");
            cached.CreateAttributes = (decltype(cached.CreateAttributes))(
                void (*)(void))GetProcAddress(plat, "MFCreateAttributes");
            cached.CreateMediaType = (decltype(cached.CreateMediaType))(
                void (*)(void))GetProcAddress(plat, "MFCreateMediaType");
            cached.CreateSourceReaderFromURL =
                (decltype(cached.CreateSourceReaderFromURL))(void (*)(void))
                    GetProcAddress(rw, "MFCreateSourceReaderFromURL");
            cached.loaded = cached.Startup && cached.Shutdown &&
                            cached.CreateAttributes && cached.CreateMediaType &&
                            cached.CreateSourceReaderFromURL;
        }
    }
    api = cached;
    return api.loaded;
}

static std::wstring Widen(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) {
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    }
    return w;
}

void PreviewPlayer::Impl::Run()
{
    MfApi mf;
    if (!LoadMf(mf)) {
        SetError("Preview videos need Windows Media Foundation (on Windows "
                 "N editions, install the Media Feature Pack)");
        return;
    }
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool com = SUCCEEDED(hr);
    if (FAILED(mf.Startup(MF_VERSION, MFSTARTUP_LITE))) {
        SetError("Windows Media Foundation is not available");
        if (com) {
            CoUninitialize();
        }
        return;
    }

    IMFAttributes *attr = nullptr;
    IMFSourceReader *reader = nullptr;
    IMFMediaType *type = nullptr;
    IMFMediaType *current = nullptr;
    UINT32 w = 0, h = 0;
    LONG default_stride = 0;

    do {
        if (FAILED(mf.CreateAttributes(&attr, 1)) ||
            FAILED(attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
                                   TRUE))) {
            SetError("Could not set up the video decoder");
            break;
        }
        std::wstring wpath = Widen(path);
        if (FAILED(mf.CreateSourceReaderFromURL(wpath.c_str(), attr, &reader))) {
            SetError("This video format can't be played (try an MP4 or WMV)");
            break;
        }
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                   TRUE);
        if (FAILED(mf.CreateMediaType(&type)) ||
            FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
            FAILED(reader->SetCurrentMediaType(
                (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, type)) ||
            FAILED(reader->GetCurrentMediaType(
                (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current)) ||
            FAILED(MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &w, &h)) ||
            w == 0 || h == 0) {
            SetError("This video has no playable picture");
            break;
        }
        UINT32 stride_attr = 0;
        if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride_attr))) {
            default_stride = (LONG)stride_attr;
        } else {
            default_stride = (LONG)w * 4;
        }

        auto start = std::chrono::steady_clock::now();
        LONGLONG first_ts = -1;
        while (!stop) {
            DWORD stream = 0, flags = 0;
            LONGLONG ts = 0;
            IMFSample *sample = nullptr;
            hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                    0, &stream, &flags, &ts, &sample);
            if (FAILED(hr)) {
                SetError("The video stopped playing (decoder error)");
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                // Loop
                PROPVARIANT pos;
                PropVariantInit(&pos);
                pos.vt = VT_I8;
                pos.hVal.QuadPart = 0;
                reader->SetCurrentPosition(GUID_NULL, pos);
                PropVariantClear(&pos);
                start = std::chrono::steady_clock::now();
                first_ts = -1;
                SafeRelease(sample);
                continue;
            }
            if (!sample) {
                continue;
            }
            if (first_ts < 0) {
                first_ts = ts;
            }
            // Wait until this frame is due (timestamps are in 100 ns units)
            auto due = start + std::chrono::microseconds((ts - first_ts) / 10);
            while (!stop && std::chrono::steady_clock::now() < due) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            IMFMediaBuffer *buf = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
                IMF2DBuffer *buf2d = nullptr;
                BYTE *scan0 = nullptr;
                LONG pitch = 0;
                if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&buf2d))) &&
                    SUCCEEDED(buf2d->Lock2D(&scan0, &pitch))) {
                    Publish(scan0, (int)w, (int)h, pitch, true);
                    buf2d->Unlock2D();
                } else {
                    BYTE *data = nullptr;
                    DWORD len = 0;
                    if (SUCCEEDED(buf->Lock(&data, NULL, &len))) {
                        LONG p = default_stride;
                        const BYTE *first = data;
                        if (p < 0) { // bottom-up rows
                            first = data + (size_t)(-p) * (h - 1);
                        }
                        if (len >= (DWORD)(labs(p) * (LONG)h)) {
                            Publish(first, (int)w, (int)h, p, true);
                        }
                        buf->Unlock();
                    }
                }
                SafeRelease(buf2d);
                SafeRelease(buf);
            }
            SafeRelease(sample);
        }
    } while (0);

    SafeRelease(current);
    SafeRelease(type);
    SafeRelease(reader);
    SafeRelease(attr);
    mf.Shutdown();
    if (com) {
        CoUninitialize();
    }
}

bool PreviewPlayer::Supported()
{
    return true;
}

#else

void PreviewPlayer::Impl::Run()
{
    SetError("Preview videos are only supported on Windows");
}

bool PreviewPlayer::Supported()
{
    return false;
}

#endif

PreviewPlayer::PreviewPlayer() : m(new Impl) {}

PreviewPlayer::~PreviewPlayer()
{
    Close(false); // the GL context may already be gone
    delete m;
}

void PreviewPlayer::Open(const std::string &path)
{
    Close();
    m->path = path;
    m->stop = false;
    {
        std::lock_guard<std::mutex> g(m->lock);
        m->error.clear();
        m->frame.clear();
        m->frame_new = false;
    }
    m->thread = std::thread([this] { m->Run(); });
}

void PreviewPlayer::Close(bool release_gl)
{
    m->stop = true;
    if (m->thread.joinable()) {
        m->thread.join();
    }
    if (m->tex && release_gl) {
        glDeleteTextures(1, &m->tex);
    }
    m->tex = 0;
    m->tex_w = m->tex_h = 0;
    m->path.clear();
}

bool PreviewPlayer::IsOpen() const
{
    return !m->path.empty();
}

const std::string &PreviewPlayer::Path() const
{
    return m->path;
}

std::string PreviewPlayer::Error() const
{
    std::lock_guard<std::mutex> g(m->lock);
    return m->error;
}

unsigned int PreviewPlayer::Update(int *w, int *h)
{
    std::lock_guard<std::mutex> g(m->lock);
    if (m->frame_new && !m->frame.empty()) {
        if (!m->tex) {
            glGenTextures(1, &m->tex);
            glBindTexture(GL_TEXTURE_2D, m->tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, m->tex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (m->tex_w != m->frame_w || m->tex_h != m->frame_h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m->frame_w, m->frame_h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, m->frame.data());
            m->tex_w = m->frame_w;
            m->tex_h = m->frame_h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m->frame_w, m->frame_h,
                            GL_RGBA, GL_UNSIGNED_BYTE, m->frame.data());
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        m->frame_new = false;
    }
    if (w) {
        *w = m->tex_w;
    }
    if (h) {
        *h = m->tex_h;
    }
    return m->tex;
}
