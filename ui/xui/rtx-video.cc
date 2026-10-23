//
// xemu User Interface - NVIDIA RTX Video Super Resolution
//
// The frame is copied into a Direct3D 11 texture shared with OpenGL
// (WGL_NV_DX_interop2), scaled by the D3D11 video processor with NVIDIA's
// super resolution extension switched on, and handed back to OpenGL. This
// is the same driver interface Chromium and mpv use for RTX Video.
//
// The driver gives no direct sign of whether it applied super resolution,
// so on first use a test image is scaled with and without the extension:
// if the results are identical the driver did not engage it (feature off
// in the NVIDIA settings, or not an RTX GPU) and the caller falls back to
// another scaler. RGB input is tried first; if the driver only engages it
// for video formats, frames go through NV12.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#include "rtx-video.hh"

#ifndef _WIN32

bool RtxVideoUpscale(GLuint, int, int, int, int, GLuint *)
{
    return false;
}
void RtxVideoEndFrame(void)
{
}
const char *RtxVideoStatus(void)
{
    return "Needs Windows and an NVIDIA RTX graphics card";
}
void RtxVideoRetry(void)
{
}
void RtxVideoShutdown(void)
{
}

#else

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <epoxy/wgl.h>
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#ifndef WGL_ACCESS_READ_ONLY_NV
#define WGL_ACCESS_READ_ONLY_NV 0x00000000
#define WGL_ACCESS_READ_WRITE_NV 0x00000001
#endif

namespace
{

// NVIDIA "post-processing engine" video processor extension
const GUID kNvidiaPpeGuid = { 0xd43ce1b3, 0x1f4b, 0x48ac,
                              { 0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7 } };
struct NvidiaPpeExtension {
    UINT version; // 1
    UINT method;  // 2 = super resolution
    UINT enable;
};

typedef HANDLE(WINAPI *PfnOpenDevice)(void *dx_device);
typedef BOOL(WINAPI *PfnCloseDevice)(HANDLE device);
typedef HANDLE(WINAPI *PfnRegisterObject)(HANDLE device, void *dx_object,
                                          GLuint name, GLenum type,
                                          GLenum access);
typedef BOOL(WINAPI *PfnUnregisterObject)(HANDLE device, HANDLE object);
typedef BOOL(WINAPI *PfnLockObjects)(HANDLE device, GLint count,
                                     HANDLE *objects);

PfnOpenDevice wglDXOpenDevice;
PfnCloseDevice wglDXCloseDevice;
PfnRegisterObject wglDXRegisterObject;
PfnUnregisterObject wglDXUnregisterObject;
PfnLockObjects wglDXLockObjects, wglDXUnlockObjects;

ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<ID3D11VideoDevice> g_vdev;
ComPtr<ID3D11VideoContext> g_vctx;
HANDLE g_gl_dev;

enum class State { Untried, Ready, Failed };
State g_state = State::Untried;
std::string g_status = "Not used yet: choose it while a game is running";
bool g_nv12; // feed the driver NV12 instead of RGB
std::string g_err; // the step that failed, for the status line and log

bool Err(const char *step, HRESULT hr = S_OK)
{
    char b[160];
    if (hr != S_OK) {
        snprintf(b, sizeof(b), "%s (0x%08lX)", step, (unsigned long)hr);
    } else {
        snprintf(b, sizeof(b), "%s", step);
    }
    g_err = b;
    return false;
}

D3D11_VIDEO_PROCESSOR_COLOR_SPACE RgbFull()
{
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs = {};
    cs.RGB_Range = 0; // 0-255
    return cs;
}

D3D11_VIDEO_PROCESSOR_COLOR_SPACE Yuv709()
{
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs = {};
    cs.YCbCr_Matrix = 1; // BT.709
    cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    return cs;
}

ComPtr<ID3D11Texture2D> MakeTexture(int w, int h, DXGI_FORMAT fmt)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &t))) {
        return nullptr;
    }
    return t;
}

// One D3D11 video processor stage: in (iw x ih, in_fmt) -> out (ow x oh)
struct Stage {
    ComPtr<ID3D11VideoProcessorEnumerator> en;
    ComPtr<ID3D11VideoProcessor> vp;
    ComPtr<ID3D11VideoProcessorInputView> in_view;
    ComPtr<ID3D11VideoProcessorOutputView> out_view;

    bool Build(ID3D11Texture2D *in, int iw, int ih, DXGI_FORMAT in_fmt,
               D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs, ID3D11Texture2D *out,
               int ow, int oh, DXGI_FORMAT out_fmt,
               D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs, bool super_res)
    {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate = { 60, 1 };
        cd.InputWidth = (UINT)iw;
        cd.InputHeight = (UINT)ih;
        cd.OutputFrameRate = { 60, 1 };
        cd.OutputWidth = (UINT)ow;
        cd.OutputHeight = (UINT)oh;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
        HRESULT hr = g_vdev->CreateVideoProcessorEnumerator(&cd, &en);
        if (FAILED(hr)) {
            return Err("video processor enumerator", hr);
        }
        UINT flags = 0;
        if (FAILED(en->CheckVideoProcessorFormat(in_fmt, &flags)) ||
            !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
            return Err("input format not supported");
        }
        flags = 0;
        if (FAILED(en->CheckVideoProcessorFormat(out_fmt, &flags)) ||
            !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            return Err("output format not supported");
        }
        hr = g_vdev->CreateVideoProcessor(en.Get(), 0, &vp);
        if (FAILED(hr)) {
            return Err("create video processor", hr);
        }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv = {};
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        hr = g_vdev->CreateVideoProcessorInputView(in, en.Get(), &iv, &in_view);
        if (FAILED(hr)) {
            return Err("video processor input view", hr);
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov = {};
        ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = g_vdev->CreateVideoProcessorOutputView(out, en.Get(), &ov,
                                                    &out_view);
        if (FAILED(hr)) {
            return Err("video processor output view", hr);
        }
        RECT src = { 0, 0, iw, ih }, dst = { 0, 0, ow, oh };
        g_vctx->VideoProcessorSetStreamFrameFormat(
            vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        g_vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &src);
        g_vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst);
        g_vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dst);
        g_vctx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &in_cs);
        g_vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &out_cs);
        // Keep the driver's other "enhancements" (denoise etc.) out of it
        g_vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
        if (super_res) {
            NvidiaPpeExtension ext = { 1, 2, 1 };
            HRESULT he = g_vctx->VideoProcessorSetStreamExtension(
                vp.Get(), 0, &kNvidiaPpeGuid, sizeof(ext), &ext);
            if (FAILED(he)) {
                return Err("enable super resolution", he);
            }
        }
        return true;
    }

    bool Blit()
    {
        D3D11_VIDEO_PROCESSOR_STREAM st = {};
        st.Enable = TRUE;
        st.pInputSurface = in_view.Get();
        return SUCCEEDED(
            g_vctx->VideoProcessorBlt(vp.Get(), out_view.Get(), 0, 1, &st));
    }
};

// RGB in -> (NV12 ->) RGB out at the output size
struct Pipeline {
    int tw = 0, th = 0, dw = 0, dh = 0;
    ComPtr<ID3D11Texture2D> in_tex, mid_tex, out_tex;
    Stage conv, scale;

    bool Build(int in_w, int in_h, int out_w, int out_h, bool nv12,
               bool super_res)
    {
        tw = in_w;
        th = in_h;
        dw = out_w;
        dh = out_h;
        in_tex = MakeTexture(tw, th, DXGI_FORMAT_B8G8R8A8_UNORM);
        out_tex = MakeTexture(dw, dh, DXGI_FORMAT_B8G8R8A8_UNORM);
        if (!in_tex || !out_tex) {
            return Err("create Direct3D textures");
        }
        if (!nv12) {
            return scale.Build(in_tex.Get(), tw, th, DXGI_FORMAT_B8G8R8A8_UNORM,
                               RgbFull(), out_tex.Get(), dw, dh,
                               DXGI_FORMAT_B8G8R8A8_UNORM, RgbFull(),
                               super_res);
        }
        int mw = (tw + 1) & ~1, mh = (th + 1) & ~1; // NV12 needs even sizes
        mid_tex = MakeTexture(mw, mh, DXGI_FORMAT_NV12);
        if (!mid_tex) {
            return Err("create NV12 texture");
        }
        return
               conv.Build(in_tex.Get(), tw, th, DXGI_FORMAT_B8G8R8A8_UNORM,
                          RgbFull(), mid_tex.Get(), tw, th, DXGI_FORMAT_NV12,
                          Yuv709(), false) &&
               scale.Build(mid_tex.Get(), tw, th, DXGI_FORMAT_NV12, Yuv709(),
                           out_tex.Get(), dw, dh, DXGI_FORMAT_B8G8R8A8_UNORM,
                           RgbFull(), super_res);
    }

    bool Run()
    {
        if (mid_tex && !conv.Blit()) {
            return false;
        }
        return scale.Blit();
    }
};

// ---- Engagement test ---------------------------------------------------

// A game-like test image: gradients, hard edges, thin lines and small
// text-sized detail (super resolution leaves flat areas alone)
std::vector<uint32_t> TestPattern(int w, int h)
{
    std::vector<uint32_t> px((size_t)w * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int r = x * 255 / w, g = y * 255 / h, b = 128;
            if (((x / 24) + (y / 24)) % 2 == 0 && y > h / 2) {
                r = g = b = 230; // checker blocks
            }
            if ((x + 2 * y) % 37 < 2 || (3 * x - y + 999) % 53 < 1) {
                r = g = b = 20; // thin diagonal lines
            }
            if (y < h / 4 && x > w / 2 && ((x * 7 ^ y * 13) & 0x10)) {
                r = 250, g = 240, b = 40; // fine "text" texture
            }
            px[(size_t)y * w + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    return px;
}

bool ReadBack(ID3D11Texture2D *tex, int w, int h, std::vector<uint8_t> &out)
{
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) {
        return false;
    }
    g_ctx->CopyResource(st.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        return false;
    }
    out.resize((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        memcpy(&out[(size_t)y * w * 4], (uint8_t *)m.pData + (size_t)y * m.RowPitch,
               (size_t)w * 4);
    }
    g_ctx->Unmap(st.Get(), 0);
    return true;
}

// Mean difference (0-255) between plain and super resolution scaling, or
// -1 if the test could not run
double EngagementTest(bool nv12)
{
    const int iw = 640, ih = 480, ow = 1280, oh = 960;
    std::vector<uint32_t> pattern = TestPattern(iw, ih);
    std::vector<uint8_t> res[2];
    for (int i = 0; i < 2; i++) {
        Pipeline p;
        if (!p.Build(iw, ih, ow, oh, nv12, i == 1)) {
            return -1;
        }
        g_ctx->UpdateSubresource(p.in_tex.Get(), 0, nullptr, pattern.data(),
                                 iw * 4, 0);
        for (int n = 0; n < 3; n++) { // give the driver a few frames
            if (!p.Run()) {
                return -1;
            }
        }
        if (!ReadBack(p.out_tex.Get(), ow, oh, res[i])) {
            return -1;
        }
    }
    double sum = 0;
    for (size_t k = 0; k < res[0].size(); k++) {
        if ((k & 3) != 3) {
            sum += fabs((double)res[0][k] - res[1][k]);
        }
    }
    return sum / (res[0].size() / 4 * 3);
}

// ---- Device setup ------------------------------------------------------

bool Fail(const char *why)
{
    g_state = State::Failed;
    g_status = why;
    fprintf(stderr, "rtx-video: %s\n", why);
    return false;
}

bool LoadInterop()
{
    wglDXOpenDevice = (PfnOpenDevice)SDL_GL_GetProcAddress("wglDXOpenDeviceNV");
    wglDXCloseDevice =
        (PfnCloseDevice)SDL_GL_GetProcAddress("wglDXCloseDeviceNV");
    wglDXRegisterObject =
        (PfnRegisterObject)SDL_GL_GetProcAddress("wglDXRegisterObjectNV");
    wglDXUnregisterObject =
        (PfnUnregisterObject)SDL_GL_GetProcAddress("wglDXUnregisterObjectNV");
    wglDXLockObjects =
        (PfnLockObjects)SDL_GL_GetProcAddress("wglDXLockObjectsNV");
    wglDXUnlockObjects =
        (PfnLockObjects)SDL_GL_GetProcAddress("wglDXUnlockObjectsNV");
    return wglDXOpenDevice && wglDXCloseDevice && wglDXRegisterObject &&
           wglDXUnregisterObject && wglDXLockObjects && wglDXUnlockObjects;
}

bool Init()
{
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    if (!vendor || !strstr(vendor, "NVIDIA")) {
        return Fail("Needs an NVIDIA RTX graphics card");
    }
    if (!LoadInterop()) {
        return Fail("The NVIDIA driver's OpenGL/Direct3D sharing isn't "
                    "available; update the driver");
    }

    // The NVIDIA adapter (the one running xemu's OpenGL)
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return Fail("Direct3D isn't available");
    }
    ComPtr<IDXGIAdapter1> adapter, nv;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
         i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(adapter->GetDesc1(&d)) && d.VendorId == 0x10DE) {
            nv = adapter;
            break;
        }
    }
    if (!nv) {
        return Fail("Needs an NVIDIA RTX graphics card");
    }
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nv.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 levels, 1, D3D11_SDK_VERSION, &g_dev, nullptr,
                                 &g_ctx)) ||
        FAILED(g_dev.As(&g_vdev)) || FAILED(g_ctx.As(&g_vctx))) {
        return Fail("Couldn't start the Direct3D video processor");
    }

    // Does the driver actually apply super resolution?
    double rgb = EngagementTest(false);
    double yuv = rgb > 0.4 ? -1 : EngagementTest(true);
    fprintf(stderr, "rtx-video: engagement test: rgb %.3f, nv12 %.3f\n", rgb,
            yuv);
    if (rgb > 0.4) {
        g_nv12 = false;
    } else if (yuv > 0.4) {
        g_nv12 = true;
    } else {
        g_vctx.Reset();
        g_vdev.Reset();
        g_ctx.Reset();
        g_dev.Reset();
        return Fail("Not switched on in the driver: turn on RTX Video "
                    "Super Resolution in the NVIDIA app (System > Video) "
                    "or NVIDIA Control Panel, then press Check again. "
                    "Needs an RTX graphics card. Using NIS for now.");
    }

    g_gl_dev = wglDXOpenDevice(g_dev.Get());
    if (!g_gl_dev) {
        g_vctx.Reset();
        g_vdev.Reset();
        g_ctx.Reset();
        g_dev.Reset();
        return Fail("The NVIDIA driver's OpenGL/Direct3D sharing failed");
    }
    g_state = State::Ready;
    g_status = g_nv12 ? "Active (video mode)" : "Active";
    fprintf(stderr, "rtx-video: active (%s input)\n", g_nv12 ? "NV12" : "RGB");
    return true;
}

// ---- Per-size resources shared with OpenGL -----------------------------

Pipeline g_pipe;
GLuint g_gl_in, g_gl_out, g_gl_in_fbo;
HANDLE g_h_in, g_h_out;
bool g_out_locked;

void Log(const char *msg)
{
    fprintf(stderr, "rtx-video: %s\n", msg);
    fflush(stderr);
}

void ReleaseSized()
{
    if (g_out_locked) {
        wglDXUnlockObjects(g_gl_dev, 1, &g_h_out);
        g_out_locked = false;
    }
    // Let both APIs finish with the shared textures before they go away
    glFinish();
    if (g_ctx) {
        g_ctx->ClearState();
        g_ctx->Flush();
    }
    if (g_gl_in_fbo) {
        glDeleteFramebuffers(1, &g_gl_in_fbo);
        g_gl_in_fbo = 0;
    }
    if (g_h_in) {
        wglDXUnregisterObject(g_gl_dev, g_h_in);
        g_h_in = nullptr;
    }
    if (g_h_out) {
        wglDXUnregisterObject(g_gl_dev, g_h_out);
        g_h_out = nullptr;
    }
    if (g_gl_in) {
        glDeleteTextures(1, &g_gl_in);
        g_gl_in = 0;
    }
    if (g_gl_out) {
        glDeleteTextures(1, &g_gl_out);
        g_gl_out = 0;
    }
    g_pipe = Pipeline();
}

GLuint NewTexture()
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

// The input normally goes GPU to GPU through a shared texture; if the
// driver won't share that one, it is copied through the CPU instead
// (slower, but only the small native-resolution image).
bool g_cpu_input;
std::vector<uint8_t> g_cpu_buf;

bool BuildSized(int tw, int th, int dw, int dh)
{
    ReleaseSized();
    Log("released old textures");
    if (!g_pipe.Build(tw, th, dw, dh, g_nv12, true)) {
        std::string first = g_err;
        g_pipe = Pipeline();
        // Some sizes only work with the video (NV12) input path
        if (g_nv12 || !g_pipe.Build(tw, th, dw, dh, true, true)) {
            if (!g_nv12) {
                g_err = first + "; NV12: " + g_err;
            }
            return false;
        }
        g_nv12 = true;
    }
    Log("video processor ready");
    g_gl_out = NewTexture();
    g_h_out = wglDXRegisterObject(g_gl_dev, g_pipe.out_tex.Get(), g_gl_out,
                                  GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
    if (!g_h_out) {
        return Err("share output texture with OpenGL",
                   HRESULT_FROM_WIN32(GetLastError()));
    }

    // Input: shared texture with an FBO to blit into (checked while locked;
    // an unlocked shared texture has no storage on the OpenGL side)
    g_cpu_input = false;
    g_gl_in = NewTexture();
    g_h_in = wglDXRegisterObject(g_gl_dev, g_pipe.in_tex.Get(), g_gl_in,
                                 GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
    bool ok = false;
    if (g_h_in && wglDXLockObjects(g_gl_dev, 1, &g_h_in)) {
        GLint prev;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
        glGenFramebuffers(1, &g_gl_in_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl_in_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, g_gl_in, 0);
        ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) ==
             GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev);
        wglDXUnlockObjects(g_gl_dev, 1, &g_h_in);
    }
    if (!ok) {
        fprintf(stderr, "rtx-video: shared input texture unusable "
                        "(registered %d); copying input through the CPU\n",
                g_h_in != nullptr);
        if (g_gl_in_fbo) {
            glDeleteFramebuffers(1, &g_gl_in_fbo);
            g_gl_in_fbo = 0;
        }
        if (g_h_in) {
            wglDXUnregisterObject(g_gl_dev, g_h_in);
            g_h_in = nullptr;
        }
        g_cpu_input = true;
        g_cpu_buf.resize((size_t)tw * th * 4);
    }
    fprintf(stderr, "rtx-video: %dx%d -> %dx%d ready (%s input, %s)\n", tw, th,
            dw, dh, g_nv12 ? "NV12" : "RGB", g_cpu_input ? "CPU copy" : "shared");
    return true;
}

void ReleaseAll()
{
    if (g_gl_dev) {
        ReleaseSized();
        wglDXCloseDevice(g_gl_dev);
        g_gl_dev = nullptr;
    }
    g_vctx.Reset();
    g_vdev.Reset();
    g_ctx.Reset();
    g_dev.Reset();
}

} // namespace

bool RtxVideoUpscale(GLuint src_fbo, int tw, int th, int dw, int dh,
                     GLuint *out_tex)
{
    if (g_state == State::Untried) {
        Init();
    }
    if (g_state != State::Ready) {
        return false;
    }
    if (g_out_locked) {
        RtxVideoEndFrame();
    }
    if (tw > 8192 || th > 8192 || dw > 8192 || dh > 8192) {
        return false;
    }
    if (tw != g_pipe.tw || th != g_pipe.th || dw != g_pipe.dw ||
        dh != g_pipe.dh || !g_h_out) {
        // While the window is resizing (e.g. going fullscreen) the size
        // changes every frame: wait until it settles, using NIS meanwhile,
        // instead of rebuilding the shared textures each frame
        static int want_w, want_h, want_tw, want_th;
        static Uint64 since;
        Uint64 now = SDL_GetTicks();
        if (dw != want_w || dh != want_h || tw != want_tw || th != want_th) {
            want_w = dw;
            want_h = dh;
            want_tw = tw;
            want_th = th;
            since = now;
        }
        if (g_h_out && now - since < 400) {
            return false;
        }
        g_err.clear();
        char m[96];
        snprintf(m, sizeof(m), "resizing to %dx%d -> %dx%d", tw, th, dw, dh);
        Log(m);
        if (!BuildSized(tw, th, dw, dh)) {
            char b[320];
            snprintf(b, sizeof(b),
                     "Couldn't set up %dx%d -> %dx%d: %s. Using NIS.", tw, th,
                     dw, dh, g_err.empty() ? "unknown step" : g_err.c_str());
            ReleaseAll();
            Fail(b);
            return false;
        }
    }

    // 1. OpenGL copies the frame in, top row first as Direct3D expects
    if (g_cpu_input) {
        GLint prev_read, pack;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, tw, th, GL_BGRA, GL_UNSIGNED_BYTE, g_cpu_buf.data());
        glPixelStorei(GL_PACK_ALIGNMENT, pack);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
        // OpenGL rows are bottom first: hand them over in reverse
        const uint8_t *last = g_cpu_buf.data() + (size_t)(th - 1) * tw * 4;
        static std::vector<uint8_t> flipped;
        flipped.resize(g_cpu_buf.size());
        for (int y = 0; y < th; y++) {
            memcpy(&flipped[(size_t)y * tw * 4], last - (size_t)y * tw * 4,
                   (size_t)tw * 4);
        }
        g_ctx->UpdateSubresource(g_pipe.in_tex.Get(), 0, nullptr,
                                 flipped.data(), tw * 4, 0);
    } else {
        if (!wglDXLockObjects(g_gl_dev, 1, &g_h_in)) {
            ReleaseAll();
            Fail("Lost the OpenGL/Direct3D sharing; using NIS");
            return false;
        }
        GLint prev_read, prev_draw;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl_in_fbo);
        glBlitFramebuffer(0, 0, tw, th, 0, th, tw, 0, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
        wglDXUnlockObjects(g_gl_dev, 1, &g_h_in);
    }

    // 2. The driver upscales it
    if (!g_pipe.Run()) {
        ReleaseAll();
        Fail("The video processor stopped working; using NIS");
        return false;
    }

    // 3. OpenGL reads the result
    if (!wglDXLockObjects(g_gl_dev, 1, &g_h_out)) {
        ReleaseAll();
        Fail("Lost the OpenGL/Direct3D sharing; using NIS");
        return false;
    }
    g_out_locked = true;
    *out_tex = g_gl_out;
    return true;
}

void RtxVideoEndFrame(void)
{
    if (g_out_locked) {
        wglDXUnlockObjects(g_gl_dev, 1, &g_h_out);
        g_out_locked = false;
    }
}

const char *RtxVideoStatus(void)
{
    return g_status.c_str();
}

void RtxVideoRetry(void)
{
    ReleaseAll();
    g_state = State::Untried;
    g_status = "Checking...";
}

void RtxVideoShutdown(void)
{
    ReleaseAll();
}

#endif
