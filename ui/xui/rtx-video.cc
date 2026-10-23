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
#include <algorithm>
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
    fflush(stderr);
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

bool CheckEngagement();
void AbandonDevice();

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

    // Reuse the device from an earlier check unless the driver reset it
    if (g_dev && SUCCEEDED(g_dev->GetDeviceRemovedReason())) {
        return CheckEngagement();
    }
    if (g_dev) {
        AbandonDevice();
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
    return CheckEngagement();
}

bool CheckEngagement()
{
    // Does the driver actually apply super resolution?
    double rgb = EngagementTest(false);
    double yuv = rgb > 0.4 ? -1 : EngagementTest(true);
    fprintf(stderr, "rtx-video: engagement test: rgb %.3f, nv12 %.3f\n", rgb,
            yuv);
    fflush(stderr);
    if (rgb > 0.4) {
        g_nv12 = false;
    } else if (yuv > 0.4) {
        g_nv12 = true;
    } else {
        return Fail("Not switched on in the driver: turn on RTX Video "
                    "Super Resolution in the NVIDIA app (System > Video) "
                    "or NVIDIA Control Panel, then press Check again. "
                    "Needs an RTX graphics card. Using NIS for now.");
    }

    if (!g_gl_dev) {
        g_gl_dev = wglDXOpenDevice(g_dev.Get());
    }
    if (!g_gl_dev) {
        return Fail("The NVIDIA driver's OpenGL/Direct3D sharing failed");
    }
    g_state = State::Ready;
    g_status = g_nv12 ? "Active (video mode)" : "Active";
    fprintf(stderr, "rtx-video: active (%s input)\n", g_nv12 ? "NV12" : "RGB");
    fflush(stderr);
    return true;
}

// ---- Resources shared with OpenGL ---------------------------------------
//
// Tearing down textures shared between OpenGL and Direct3D crashed in the
// driver when the window changed size (going fullscreen). So shared
// textures are never released while xemu runs: they are allocated with
// room to spare and reused for every size, with the video processor (a
// Direct3D-only object, safe to recreate) told which part to use. If a
// bigger size is ever needed, new ones are added and the old ones are just
// left alone.

void Log(const char *msg)
{
    fprintf(stderr, "rtx-video: %s\n", msg);
    fflush(stderr);
}

struct Shared {
    int cap_in_w = 0, cap_in_h = 0, cap_out_w = 0, cap_out_h = 0;
    ComPtr<ID3D11Texture2D> in_tex, mid_tex, out_tex;
    GLuint gl_in = 0, gl_out = 0, gl_in_fbo = 0;
    HANDLE h_in = nullptr, h_out = nullptr;
    bool cpu_input = false; // input copied through the CPU instead
};
Shared g_sh;
std::vector<Shared> g_retired; // kept, never unregistered (see above)
// Video processor setups by size (Direct3D only, cheap), so a game that
// alternates between render sizes doesn't rebuild every frame
struct Setup {
    int tw, th, dw, dh;
    bool nv12;
    Stage conv, scale;
};
std::vector<Setup> g_setups;
Setup *g_cur;
bool g_out_locked;
std::vector<uint8_t> g_cpu_buf, g_cpu_flip;

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

// Make sure the shared textures can hold tw x th in and dw x dh out
bool EnsureCapacity(int tw, int th, int dw, int dh)
{
    if (g_sh.h_out && tw <= g_sh.cap_in_w && th <= g_sh.cap_in_h &&
        dw <= g_sh.cap_out_w && dh <= g_sh.cap_out_h) {
        return true;
    }
    Shared n;
    // Room for 4x internal resolution in and a 4K (16:10) screen out
    n.cap_in_w = std::max({ tw, g_sh.cap_in_w, 2560 });
    n.cap_in_h = std::max({ th, g_sh.cap_in_h, 1920 });
    n.cap_out_w = std::max({ dw, g_sh.cap_out_w, 3840 });
    n.cap_out_h = std::max({ dh, g_sh.cap_out_h, 2400 });
    n.cap_in_w = (n.cap_in_w + 1) & ~1; // NV12 needs even sizes
    n.cap_in_h = (n.cap_in_h + 1) & ~1;
    char m[128];
    snprintf(m, sizeof(m), "allocating shared textures %dx%d in, %dx%d out",
             n.cap_in_w, n.cap_in_h, n.cap_out_w, n.cap_out_h);
    Log(m);
    n.in_tex = MakeTexture(n.cap_in_w, n.cap_in_h, DXGI_FORMAT_B8G8R8A8_UNORM);
    n.out_tex = MakeTexture(n.cap_out_w, n.cap_out_h, DXGI_FORMAT_B8G8R8A8_UNORM);
    n.mid_tex = MakeTexture(n.cap_in_w, n.cap_in_h, DXGI_FORMAT_NV12);
    if (!n.in_tex || !n.out_tex) {
        return Err("create Direct3D textures");
    }
    n.gl_out = NewTexture();
    n.h_out = wglDXRegisterObject(g_gl_dev, n.out_tex.Get(), n.gl_out,
                                  GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
    if (!n.h_out) {
        return Err("share output texture with OpenGL",
                   HRESULT_FROM_WIN32(GetLastError()));
    }
    // Input: shared texture with an FBO to blit into (checked while
    // locked: an unlocked shared texture has no storage on the GL side)
    n.gl_in = NewTexture();
    n.h_in = wglDXRegisterObject(g_gl_dev, n.in_tex.Get(), n.gl_in,
                                 GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
    bool ok = false;
    if (n.h_in && wglDXLockObjects(g_gl_dev, 1, &n.h_in)) {
        GLint prev;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
        glGenFramebuffers(1, &n.gl_in_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, n.gl_in_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, n.gl_in, 0);
        ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) ==
             GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev);
        wglDXUnlockObjects(g_gl_dev, 1, &n.h_in);
    }
    if (!ok) {
        Log("shared input texture unusable; copying input through the CPU");
        n.cpu_input = true;
    }
    if (g_sh.h_out) {
        g_retired.push_back(std::move(g_sh));
    }
    g_sh = std::move(n);
    return true;
}

// Point the video processor at the given sizes (Direct3D only)
bool SetupStages(Setup &u)
{
    if (!u.nv12) {
        return u.scale.Build(g_sh.in_tex.Get(), u.tw, u.th,
                             DXGI_FORMAT_B8G8R8A8_UNORM, RgbFull(),
                             g_sh.out_tex.Get(), u.dw, u.dh,
                             DXGI_FORMAT_B8G8R8A8_UNORM, RgbFull(), true);
    }
    if (!g_sh.mid_tex) {
        return Err("create NV12 texture");
    }
    return u.conv.Build(g_sh.in_tex.Get(), u.tw, u.th,
                        DXGI_FORMAT_B8G8R8A8_UNORM, RgbFull(),
                        g_sh.mid_tex.Get(), u.tw, u.th, DXGI_FORMAT_NV12,
                        Yuv709(), false) &&
           u.scale.Build(g_sh.mid_tex.Get(), u.tw, u.th, DXGI_FORMAT_NV12,
                         Yuv709(), g_sh.out_tex.Get(), u.dw, u.dh,
                         DXGI_FORMAT_B8G8R8A8_UNORM, RgbFull(), true);
}

Setup *FindSetup(int tw, int th, int dw, int dh)
{
    for (auto &u : g_setups) {
        if (u.tw == tw && u.th == th && u.dw == dw && u.dh == dh) {
            return &u;
        }
    }
    if (g_setups.size() >= 6) {
        g_setups.erase(g_setups.begin());
    }
    Setup u;
    u.tw = tw;
    u.th = th;
    u.dw = dw;
    u.dh = dh;
    u.nv12 = g_nv12;
    if (!SetupStages(u)) {
        // Some sizes only work with the video (NV12) input path
        std::string first = g_err;
        u.conv = Stage();
        u.scale = Stage();
        u.nv12 = true;
        if (g_nv12 || !SetupStages(u)) {
            if (!g_nv12) {
                g_err = first + "; NV12: " + g_err;
            }
            return nullptr;
        }
    }
    char m[128];
    snprintf(m, sizeof(m), "%dx%d -> %dx%d ready (%s input, %s)", tw, th, dw,
             dh, u.nv12 ? "NV12" : "RGB", g_sh.cpu_input ? "CPU copy" : "shared");
    Log(m);
    g_setups.push_back(std::move(u));
    return &g_setups.back();
}

// Stop using RTX Video without releasing anything shared (see above)
bool Disable(const char *why)
{
    if (g_out_locked) {
        wglDXUnlockObjects(g_gl_dev, 1, &g_sh.h_out);
        g_out_locked = false;
    }
    g_setups.clear();
    g_cur = nullptr;
    return Fail(why);
}

// The driver reset the device: start over with a new one, leaving the old
// device and everything shared with it alone (releasing them is what the
// driver can't cope with)
void AbandonDevice()
{
    Log("graphics device was reset; starting a new one");
    g_setups.clear();
    g_cur = nullptr;
    g_out_locked = false;
    if (g_sh.h_out) {
        g_retired.push_back(std::move(g_sh));
        g_sh = Shared();
    }
    g_vctx.Detach();
    g_vdev.Detach();
    g_ctx.Detach();
    g_dev.Detach();
    g_gl_dev = nullptr;
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
    if (tw < 1 || th < 1 || dw < 1 || dh < 1 || tw > 8192 || th > 8192 ||
        dw > 8192 || dh > 8192) {
        return false;
    }
    // The display moved to another GPU or the driver reset
    HRESULT removed = g_dev->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        char b[160];
        snprintf(b, sizeof(b),
                 "The graphics driver reset (0x%08lX); press Check again. "
                 "Using NIS.", (unsigned long)removed);
        return Disable(b);
    }
    bool fits = g_sh.h_out && tw <= g_sh.cap_in_w && th <= g_sh.cap_in_h &&
                dw <= g_sh.cap_out_w && dh <= g_sh.cap_out_h;
    if (!fits) {
        // Bigger shared textures are needed. While the window is still
        // resizing the size changes every frame, so wait until it settles
        // (using NIS meanwhile) rather than allocating for every step.
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
        if (g_sh.h_out && now - since < 400) {
            return false;
        }
        g_err.clear();
        if (!EnsureCapacity(tw, th, dw, dh)) {
            char b[320];
            snprintf(b, sizeof(b),
                     "Couldn't set up %dx%d -> %dx%d: %s. Using NIS.", tw, th,
                     dw, dh, g_err.empty() ? "unknown step" : g_err.c_str());
            return Disable(b);
        }
        g_setups.clear(); // they point at the old textures
        g_cur = nullptr;
    }
    if (!g_cur || g_cur->tw != tw || g_cur->th != th || g_cur->dw != dw ||
        g_cur->dh != dh) {
        g_err.clear();
        g_cur = FindSetup(tw, th, dw, dh);
        if (!g_cur) {
            char b[320];
            snprintf(b, sizeof(b),
                     "Couldn't set up %dx%d -> %dx%d: %s. Using NIS.", tw, th,
                     dw, dh, g_err.empty() ? "unknown step" : g_err.c_str());
            return Disable(b);
        }
    }

    // 1. Copy the frame in, top row first as Direct3D expects
    if (g_sh.cpu_input) {
        g_cpu_buf.resize((size_t)tw * th * 4);
        g_cpu_flip.resize(g_cpu_buf.size());
        GLint prev_read, pack;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, tw, th, GL_BGRA, GL_UNSIGNED_BYTE, g_cpu_buf.data());
        glPixelStorei(GL_PACK_ALIGNMENT, pack);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
        const uint8_t *last = g_cpu_buf.data() + (size_t)(th - 1) * tw * 4;
        for (int y = 0; y < th; y++) {
            memcpy(&g_cpu_flip[(size_t)y * tw * 4], last - (size_t)y * tw * 4,
                   (size_t)tw * 4);
        }
        D3D11_BOX box = { 0, 0, 0, (UINT)tw, (UINT)th, 1 };
        g_ctx->UpdateSubresource(g_sh.in_tex.Get(), 0, &box, g_cpu_flip.data(),
                                 tw * 4, 0);
    } else {
        if (!wglDXLockObjects(g_gl_dev, 1, &g_sh.h_in)) {
            return Disable("Lost the OpenGL/Direct3D sharing; press Check "
                           "again. Using NIS.");
        }
        GLint prev_read, prev_draw;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_sh.gl_in_fbo);
        glBlitFramebuffer(0, 0, tw, th, 0, th, tw, 0, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
        wglDXUnlockObjects(g_gl_dev, 1, &g_sh.h_in);
    }

    // 2. The driver upscales it into the top-left dw x dh of the output
    if ((g_cur->nv12 && !g_cur->conv.Blit()) || !g_cur->scale.Blit()) {
        return Disable("The video processor stopped working; press Check "
                       "again. Using NIS.");
    }

    // 3. OpenGL reads the result
    if (!wglDXLockObjects(g_gl_dev, 1, &g_sh.h_out)) {
        return Disable("Lost the OpenGL/Direct3D sharing; press Check again. "
                       "Using NIS.");
    }
    g_out_locked = true;
    *out_tex = g_sh.gl_out;
    return true;
}

void RtxVideoEndFrame(void)
{
    if (g_out_locked) {
        wglDXUnlockObjects(g_gl_dev, 1, &g_sh.h_out);
        g_out_locked = false;
    }
}

const char *RtxVideoStatus(void)
{
    return g_status.c_str();
}

void RtxVideoRetry(void)
{
    if (g_state == State::Ready) {
        Disable("Checking...");
    }
    g_state = State::Untried;
    g_status = "Checking...";
}

void RtxVideoShutdown(void)
{
    // Nothing: shared textures are reclaimed when xemu exits
}

#endif
