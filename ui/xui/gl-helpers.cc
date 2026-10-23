//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "ui/xemu-widescreen.h"
#include "gl-helpers.hh"
#include "common.hh"
#include "data/controller_mask.png.h"
#include "data/controller_mask_s.png.h"
#include "data/logo_sdf.png.h"
#include "data/xemu_64x64.png.h"
#include "data/xmu_mask.png.h"
#include "notifications.hh"
#include "../xemu-xbe.h"
#include "stb_image.h"
#include <fpng.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <vector>

#include "ui/shader/xemu-logo-frag.h"
#include "rtx-video.hh"

Fbo *controller_fbo, *xmu_fbo, *logo_fbo;
GLuint g_controller_duke_tex, g_controller_s_tex, g_logo_tex, g_icon_tex, g_xmu_tex;

enum class ShaderType {
    Blit,
    BlitGamma, // FIMXE: Move to nv2a_get_framebuffer_surface
    Mask,
    Logo,
    FsrEasu, // AMD FidelityFX Super Resolution 1.0: edge-adaptive upscale
    FsrRcas, // AMD FidelityFX Super Resolution 1.0: robust contrast-adaptive sharpen
    Sgsr,    // Snapdragon Game Super Resolution 1
    Lanczos, // Lanczos-3 with anti-ringing
    Bicubic, // Catmull-Rom bicubic
    Nis,     // NVIDIA Image Scaling (NVScaler)
};

typedef struct DecalShader_
{
    int flip;
    float scale;
    uint32_t time;
    GLuint prog, vao, vbo, ebo;
    GLint flipy_loc;
    GLint tex_loc;
    GLint scale_offset_loc;
    GLint tex_scale_offset_loc;
    GLint color_primary_loc;
    GLint color_secondary_loc;
    GLint color_fill_loc;
    GLint time_loc;
    GLint scale_loc;
    GLint in_size_loc;
    GLint out_size_loc;
    GLint sharpness_loc;
    GLint viewport_info_loc;
    GLint coef_loc;
    GLint nis_loc[3];
    GLint palette_loc[256];
} DecalShader;

static DecalShader *g_decal_shader,
                   *g_logo_shader,
                   *g_framebuffer_shader,
                   *g_fsr_easu_shader,
                   *g_fsr_rcas_shader,
                   *g_sgsr_shader,
                   *g_lanczos_shader,
                   *g_bicubic_shader,
                   *g_nis_shader;

// NVIDIA Image Scaling filter banks (6 taps x 64 phases), loaded once
static GLuint g_nis_coef_tex;
static GLuint CreateNisCoefTexture(void);

// Intermediate targets for the FSR path, (re)allocated when sizes change
static Fbo *g_fsr_gamma_fbo, *g_fsr_easu_fbo;

GLint Fbo::vp[4];
GLint Fbo::original_fbo;
bool Fbo::blend;

DecalShader *NewDecalShader(enum ShaderType type);
void DeleteDecalShader(DecalShader *s);

static GLint GetCurrentFbo()
{
    GLint fbo;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, (GLint*)&fbo);
    return fbo;
}

Fbo::Fbo(int width, int height)
{
    w = width;
    h = height;

    // Allocate the texture
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);

    GLint original = GetCurrentFbo();

    // Allocate the framebuffer object
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
    GLenum DrawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, DrawBuffers);

    glBindFramebuffer(GL_FRAMEBUFFER, original);
}

Fbo::~Fbo()
{
    glDeleteTextures(1, &tex);
    glDeleteFramebuffers(1, &fbo);
}

void Fbo::Target()
{
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);

    original_fbo = GetCurrentFbo();
    blend = glIsEnabled(GL_BLEND);
    if (!blend) {
        glEnable(GL_BLEND);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Fbo::Restore()
{
    if (!blend) {
        glDisable(GL_BLEND);
    }

    // Restore default framebuffer, viewport, blending function
    glBindFramebuffer(GL_FRAMEBUFFER, original_fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static GLuint InitTexture(unsigned char *data, int width, int height,
                          int channels)
{
    GLuint tex;
    glGenTextures(1, &tex);
    assert(tex != 0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    return tex;
}

static GLuint LoadTextureFromMemory(const unsigned char *buf, unsigned int size, bool flip=true)
{
    // Flip vertically so textures are loaded according to GL convention.
    stbi_set_flip_vertically_on_load(flip);

    int width, height, channels = 0;
    unsigned char *data = stbi_load_from_memory(buf, size, &width, &height, &channels, 4);
    assert(data != NULL);

    GLuint tex = InitTexture(data, width, height, channels);
    stbi_image_free(data);

    return tex;
}

static GLuint Shader(GLenum type, const char *src)
{
    char err_buf[512];
    GLuint shader = glCreateShader(type);
    assert(shader && "Failed to create shader");

    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glGetShaderInfoLog(shader, sizeof(err_buf), NULL, err_buf);
        fprintf(stderr,
                "Shader compilation failed: %s\n\n"
                "[Shader Source]\n"
                "%s\n",
                err_buf, src);
        assert(0);
    }

    return shader;
}

DecalShader *NewDecalShader(enum ShaderType type)
{
    // Allocate shader wrapper object
    DecalShader *s = new DecalShader;
    assert(s != NULL);
    s->flip = 0;
    s->scale = 1.4;
    s->time = 0;

    const char *vert_src = R"(
#version 150 core
uniform bool in_FlipY;
uniform vec4 in_ScaleOffset;
uniform vec4 in_TexScaleOffset;
in vec2 in_Position;
in vec2 in_Texcoord;
out vec2 Texcoord;
void main() {
    vec2 t = in_Texcoord;
    if (in_FlipY) t.y = 1-t.y;
    Texcoord = t*in_TexScaleOffset.xy + in_TexScaleOffset.zw;
    gl_Position = vec4(in_Position*in_ScaleOffset.xy+in_ScaleOffset.zw, 0.0, 1.0);
}
)";
    GLuint vert = Shader(GL_VERTEX_SHADER, vert_src);
    assert(vert != 0);

    //     const char *image_frag_src = R"(
    // #version 150 core
    // uniform sampler2D tex;
    // in  vec2 Texcoord;
    // out vec4 out_Color;
    // void main() {
    //     out_Color.rgba = texture(tex, Texcoord);
    // }
    // )";

    const char *image_gamma_frag_src = R"(
#version 400 core
uniform sampler2D tex;
uniform uint palette[256];
float gamma_ch(int ch, float col)
{
    return float(bitfieldExtract(palette[uint(col * 255.0)], ch*8, 8)) / 255.0;
}

vec4 gamma(vec4 col)
{
    return vec4(gamma_ch(0, col.r), gamma_ch(1, col.g), gamma_ch(2, col.b), col.a);
}
in  vec2 Texcoord;
out vec4 out_Color;
void main() {
    out_Color.rgba = gamma(texture(tex, Texcoord));
}
)";

    // Simple 2-color decal shader
    // - in_ColorFill is first pass
    // - Red channel of the texture is used as primary color, mixed with 1-Red for
    //   secondary color.
    // - Blue is a lazy alpha removal for now
    // - Alpha channel passed through
    const char *mask_frag_src = R"(
#version 150 core
uniform sampler2D tex;
uniform vec4 in_ColorPrimary;
uniform vec4 in_ColorSecondary;
uniform vec4 in_ColorFill;
in  vec2 Texcoord;
out vec4 out_Color;
void main() {
    vec4 t = texture(tex, Texcoord);
    out_Color.rgba = in_ColorFill.rgba;
    out_Color.rgb += mix(in_ColorSecondary.rgb, in_ColorPrimary.rgb, t.r);
    out_Color.a += t.a - t.b;
}
)";


    // AMD FidelityFX Super Resolution 1.0 (FSR 1), ported to GLSL from
    // ffx_fsr1.h (Copyright (c) 2021 Advanced Micro Devices, Inc., MIT
    // License). Taps are read with texelFetch rather than textureGather so
    // the shaders only need GLSL 4.00, matching the rest of this file.
    //
    // EASU: edge-adaptive spatial upsampling. Analyses luma gradients in a
    // 12-tap window to find edge direction and length, then resamples with
    // a stretched, rotated Lanczos-like kernel and clamps to the local
    // min/max to avoid ringing.
    const char *fsr_easu_frag_src = R"(
#version 400 core
uniform sampler2D tex;
uniform vec2 in_InSize;
uniform vec2 in_OutSize;
in  vec2 Texcoord;
out vec4 out_Color;

vec4 Tap(ivec2 p)
{
    return texelFetch(tex, clamp(p, ivec2(0), ivec2(in_InSize) - 1), 0);
}

float Luma(vec3 c)
{
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

void EasuSet(inout vec2 dir, inout float len, float w,
             float lA, float lB, float lC, float lD, float lE)
{
    //   a
    // b c d
    //   e
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = lenX > 0.0 ? 1.0 / lenX : 0.0;
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;

    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = lenY > 0.0 ? 1.0 / lenY : 0.0;
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

void EasuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len,
             float lob, float clp, vec3 c)
{
    vec2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * -dir.y) + (off.y * dir.x);
    v *= len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void main()
{
    // Output pixel center mapped into input texel space
    vec2 pp = gl_FragCoord.xy * (in_InSize / in_OutSize) - 0.5;
    vec2 fp = floor(pp);
    pp -= fp;
    ivec2 i0 = ivec2(fp);

    //     b c
    //   e f g h
    //   i j k l
    //     n o
    vec3 b = Tap(i0 + ivec2( 0, -1)).rgb;
    vec3 c = Tap(i0 + ivec2( 1, -1)).rgb;
    vec3 e = Tap(i0 + ivec2(-1,  0)).rgb;
    vec4 f4 = Tap(i0);
    vec3 f = f4.rgb;
    vec3 g = Tap(i0 + ivec2( 1,  0)).rgb;
    vec3 h = Tap(i0 + ivec2( 2,  0)).rgb;
    vec3 i = Tap(i0 + ivec2(-1,  1)).rgb;
    vec3 j = Tap(i0 + ivec2( 0,  1)).rgb;
    vec3 k = Tap(i0 + ivec2( 1,  1)).rgb;
    vec3 l = Tap(i0 + ivec2( 2,  1)).rgb;
    vec3 n = Tap(i0 + ivec2( 0,  2)).rgb;
    vec3 o = Tap(i0 + ivec2( 1,  2)).rgb;

    float bL = Luma(b), cL = Luma(c), eL = Luma(e), fL = Luma(f);
    float gL = Luma(g), hL = Luma(h), iL = Luma(i), jL = Luma(j);
    float kL = Luma(k), lL = Luma(l), nL = Luma(n), oL = Luma(o);

    // Accumulate edge direction and length from the four bilinear quads
    vec2 dir = vec2(0.0);
    float len = 0.0;
    EasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    EasuSet(dir, len, pp.x * (1.0 - pp.y),         cL, fL, gL, hL, kL);
    EasuSet(dir, len, (1.0 - pp.x) * pp.y,         fL, iL, jL, kL, nL);
    EasuSet(dir, len, pp.x * pp.y,                 gL, jL, kL, lL, oL);

    // Normalize direction, defaulting to +x when there is no gradient
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = zro ? 1.0 : inversesqrt(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;

    // Shape the kernel from edge length
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) /
                    max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    vec3 aC = vec3(0.0);
    float aW = 0.0;
    EasuTap(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);
    EasuTap(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);
    EasuTap(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, i);
    EasuTap(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
    EasuTap(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
    EasuTap(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);
    EasuTap(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
    EasuTap(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);
    EasuTap(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);
    EasuTap(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
    EasuTap(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);
    EasuTap(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);

    // Deringing: clamp to the min/max of the nearest 2x2 input texels
    vec3 mn = min(min(f, g), min(j, k));
    vec3 mx = max(max(f, g), max(j, k));
    out_Color = vec4(min(mx, max(mn, aC / aW)), f4.a);
}
)";

    // RCAS: robust contrast-adaptive sharpening, run at output resolution
    // after EASU. Sharpening is limited per pixel so it cannot clip.
    const char *fsr_rcas_frag_src = R"(
#version 400 core
uniform sampler2D tex;
uniform vec2 in_InSize;
uniform float in_Sharpness;
in  vec2 Texcoord;
out vec4 out_Color;

#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

vec4 Tap(ivec2 p)
{
    return texelFetch(tex, clamp(p, ivec2(0), ivec2(in_InSize) - 1), 0);
}

float Luma(vec3 c)
{
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

void main()
{
    ivec2 ip = ivec2(Texcoord * in_InSize);

    //   b
    // d e f
    //   h
    vec3 b = Tap(ip + ivec2( 0, -1)).rgb;
    vec3 d = Tap(ip + ivec2(-1,  0)).rgb;
    vec4 e4 = Tap(ip);
    vec3 e = e4.rgb;
    vec3 f = Tap(ip + ivec2( 1,  0)).rgb;
    vec3 h = Tap(ip + ivec2( 0,  1)).rgb;

    // Largest negative lobe that will not clip, per channel
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    // As in current ffx_fsr1.h, the center pixel is included in the limits
    vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));
    vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-FSR_RCAS_LIMIT,
                     min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) *
                 in_Sharpness; // FSR_RCAS_DENOISE is off by default

    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    vec3 c = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
    out_Color = vec4(c, e4.a);
}
)";

    // Snapdragon Game Super Resolution 1 (SGSR 1), from the official GLSL
    // sgsr1_shader_mobile_edge_direction.frag (Copyright (c) 2025,
    // Qualcomm Innovation Center, Inc., BSD-3-Clause), RGBA mode with edge
    // direction disabled (the default). Precision qualifiers dropped for
    // desktop GLSL.
    const char *sgsr_frag_src = R"(
#version 400 core
uniform sampler2D tex;
uniform vec4 in_ViewportInfo; // 1/w, 1/h, w, h of the input
in  vec2 Texcoord;
out vec4 out_Color;

#define OperationMode 1
#define EdgeThreshold (8.0/255.0)
#define EdgeSharpness 2.0

float fastLanczos2(float x)
{
    float wA = x - 4.0;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}

vec2 weightY(float dx, float dy, float c, float std)
{
    float x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);
    float w = fastLanczos2(x);
    return vec2(w, w * c);
}

void main()
{
    vec4 color;
    color.xyz = textureLod(tex, Texcoord, 0.0).xyz;

    vec2 imgCoord = (Texcoord * in_ViewportInfo.zw) + vec2(-0.5, 0.5);
    vec2 imgCoordPixel = floor(imgCoord);
    vec2 coord = imgCoordPixel * in_ViewportInfo.xy;
    vec2 pl = imgCoord - imgCoordPixel;
    vec4 left = textureGather(tex, coord, OperationMode);

    float edgeVote = abs(left.z - left.y) + abs(color[OperationMode] - left.y) +
                     abs(color[OperationMode] - left.z);
    if (edgeVote > EdgeThreshold) {
        coord.x += in_ViewportInfo.x;

        vec4 right = textureGather(tex, coord + vec2(in_ViewportInfo.x, 0.0), OperationMode);
        vec4 upDown;
        upDown.xy = textureGather(tex, coord + vec2(0.0, -in_ViewportInfo.y), OperationMode).wz;
        upDown.zw = textureGather(tex, coord + vec2(0.0, in_ViewportInfo.y), OperationMode).yx;

        float mean = (left.y + left.z + right.x + right.w) * 0.25;
        left = left - vec4(mean);
        right = right - vec4(mean);
        upDown = upDown - vec4(mean);
        color.w = color[OperationMode] - mean;

        float sum = (((((abs(left.x) + abs(left.y)) + abs(left.z)) + abs(left.w)) +
                      (((abs(right.x) + abs(right.y)) + abs(right.z)) + abs(right.w))) +
                     (((abs(upDown.x) + abs(upDown.y)) + abs(upDown.z)) + abs(upDown.w)));
        float sumMean = 1.014185e+01 / sum;
        float std = (sumMean * sumMean);

        vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, std);
        aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, std);
        aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, std);
        aWY += weightY(pl.x, pl.y - 2.0, upDown.w, std);
        aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, std);
        aWY += weightY(pl.x, pl.y - 1.0, left.y, std);
        aWY += weightY(pl.x, pl.y, left.z, std);
        aWY += weightY(pl.x + 1.0, pl.y, left.w, std);
        aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, std);
        aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, std);
        aWY += weightY(pl.x - 2.0, pl.y, right.z, std);
        aWY += weightY(pl.x - 1.0, pl.y, right.w, std);

        float finalY = aWY.y / aWY.x;
        float maxY = max(max(left.y, left.z), max(right.x, right.w));
        float minY = min(min(left.y, left.z), min(right.x, right.w));
        float deltaY = clamp(EdgeSharpness * finalY, minY, maxY) - color.w;

        // smooth high contrast input
        deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);

        color.x = clamp((color.x + deltaY), 0.0, 1.0);
        color.y = clamp((color.y + deltaY), 0.0, 1.0);
        color.z = clamp((color.z + deltaY), 0.0, 1.0);
    }

    color.w = 1.0;
    out_Color = color;
}
)";

    // Lanczos-3 (6x6 taps), clamped to the nearest 2x2 texels to avoid
    // ringing halos.
    const char *lanczos_frag_src = R"(
#version 400 core
uniform sampler2D tex;
uniform vec2 in_InSize;
uniform vec2 in_OutSize;
in  vec2 Texcoord;
out vec4 out_Color;

const float PI = 3.14159265358979;

vec4 Tap(ivec2 p)
{
    return texelFetch(tex, clamp(p, ivec2(0), ivec2(in_InSize) - 1), 0);
}

float Lanczos3(float x)
{
    x = abs(x);
    if (x < 1e-5) {
        return 1.0;
    }
    if (x >= 3.0) {
        return 0.0;
    }
    float px = PI * x;
    return 3.0 * sin(px) * sin(px / 3.0) / (px * px);
}

void main()
{
    vec2 pp = gl_FragCoord.xy * (in_InSize / in_OutSize) - 0.5;
    vec2 fp = floor(pp);
    vec2 f = pp - fp;
    ivec2 i0 = ivec2(fp);

    float wx[6], wy[6];
    for (int i = 0; i < 6; i++) {
        wx[i] = Lanczos3(float(i - 2) - f.x);
        wy[i] = Lanczos3(float(i - 2) - f.y);
    }
    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    for (int y = 0; y < 6; y++) {
        for (int x = 0; x < 6; x++) {
            float w = wx[x] * wy[y];
            acc += Tap(i0 + ivec2(x - 2, y - 2)) * w;
            wsum += w;
        }
    }
    vec4 c = acc / wsum;
    vec4 a = Tap(i0), b = Tap(i0 + ivec2(1, 0));
    vec4 d = Tap(i0 + ivec2(0, 1)), e = Tap(i0 + ivec2(1, 1));
    vec4 mn = min(min(a, b), min(d, e)), mx = max(max(a, b), max(d, e));
    out_Color = clamp(c, mn, mx);
}
)";

    // Catmull-Rom bicubic (4x4 taps)
    const char *bicubic_frag_src = R"(
#version 400 core
uniform sampler2D tex;
uniform vec2 in_InSize;
uniform vec2 in_OutSize;
in  vec2 Texcoord;
out vec4 out_Color;

vec4 Tap(ivec2 p)
{
    return texelFetch(tex, clamp(p, ivec2(0), ivec2(in_InSize) - 1), 0);
}

float CatmullRom(float x)
{
    x = abs(x);
    if (x < 1.0) {
        return 1.5 * x * x * x - 2.5 * x * x + 1.0;
    }
    if (x < 2.0) {
        return -0.5 * x * x * x + 2.5 * x * x - 4.0 * x + 2.0;
    }
    return 0.0;
}

void main()
{
    vec2 pp = gl_FragCoord.xy * (in_InSize / in_OutSize) - 0.5;
    vec2 fp = floor(pp);
    vec2 f = pp - fp;
    ivec2 i0 = ivec2(fp);

    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    for (int y = -1; y <= 2; y++) {
        float wy = CatmullRom(float(y) - f.y);
        for (int x = -1; x <= 2; x++) {
            float w = CatmullRom(float(x) - f.x) * wy;
            acc += Tap(i0 + ivec2(x, y)) * w;
            wsum += w;
        }
    }
    out_Color = clamp(acc / wsum, 0.0, 1.0);
}
)";


    // NVIDIA Image Scaling (NVScaler) v1.0.3, ported from NIS_Scaler.h
    // (Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES, MIT license) to a
    // per-pixel fragment shader: the same 6-tap polyphase scaler, 4
    // directional filters, edge detection and luma-adaptive sharpening,
    // without the compute shader's shared-memory tiling.
    const char *nis_frag_src = R"(
#version 400 core
uniform sampler2D tex;
uniform sampler2D coef;   // 4x64: scaler taps 0-3, 4-5 | USM taps 0-3, 4-5
uniform vec2 in_InSize;
uniform vec2 in_OutSize;
uniform vec4 in_Nis0;     // detect ratio, detect threshold, min contrast ratio, ratio norm
uniform vec4 in_Nis1;     // sharp start Y, sharp scale Y, strength min, strength scale
uniform vec4 in_Nis2;     // limit min, limit scale, contrast boost, eps
in  vec2 Texcoord;
out vec4 out_Color;

#define kDetectRatio in_Nis0.x
#define kDetectThres in_Nis0.y
#define kMinContrastRatio in_Nis0.z
#define kRatioNorm in_Nis0.w
#define kSharpStartY in_Nis1.x
#define kSharpScaleY in_Nis1.y
#define kSharpStrengthMin in_Nis1.z
#define kSharpStrengthScale in_Nis1.w
#define kSharpLimitMin in_Nis2.x
#define kSharpLimitScale in_Nis2.y
#define kContrastBoost in_Nis2.z
#define kEps in_Nis2.w
#define kPhaseCount 64

float p[36];
#define P(r, c) p[(r) * 6 + (c)]

float GetY(vec3 c)
{
    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
}

float LoadY(ivec2 q)
{
    return GetY(texelFetch(tex, clamp(q, ivec2(0), ivec2(in_InSize) - 1), 0).rgb);
}

void Scaler(int phase, out float c[6])
{
    vec4 a = texelFetch(coef, ivec2(0, phase), 0);
    vec4 b = texelFetch(coef, ivec2(1, phase), 0);
    c[0] = a.x; c[1] = a.y; c[2] = a.z; c[3] = a.w; c[4] = b.x; c[5] = b.y;
}

void Usm(int phase, out float c[6])
{
    vec4 a = texelFetch(coef, ivec2(2, phase), 0);
    vec4 b = texelFetch(coef, ivec2(3, phase), 0);
    c[0] = a.x; c[1] = a.y; c[2] = a.z; c[3] = a.w; c[4] = b.x; c[5] = b.y;
}

// Edge weights from the 3x3 luma window starting at (r, c)
vec4 GetEdgeMap(int r, int c)
{
    float g_0 = abs(P(r, c) + P(r, c + 1) + P(r, c + 2) - P(r + 2, c) - P(r + 2, c + 1) - P(r + 2, c + 2));
    float g_45 = abs(P(r + 1, c) + P(r, c) + P(r, c + 1) - P(r + 2, c + 1) - P(r + 2, c + 2) - P(r + 1, c + 2));
    float g_90 = abs(P(r, c) + P(r + 1, c) + P(r + 2, c) - P(r, c + 2) - P(r + 1, c + 2) - P(r + 2, c + 2));
    float g_135 = abs(P(r + 1, c) + P(r + 2, c) + P(r + 2, c + 1) - P(r, c + 1) - P(r, c + 2) - P(r + 1, c + 2));

    float g_0_90_max = max(g_0, g_90);
    float g_0_90_min = min(g_0, g_90);
    float g_45_135_max = max(g_45, g_135);
    float g_45_135_min = min(g_45, g_135);
    if (g_0_90_max + g_45_135_max == 0.0) {
        return vec4(0.0);
    }
    float e_0_90 = min(g_0_90_max / (g_0_90_max + g_45_135_max), 1.0);
    float e_45_135 = 1.0 - e_0_90;

    bool c_0_90 = (g_0_90_max > (g_0_90_min * kDetectRatio)) && (g_0_90_max > kDetectThres) && (g_0_90_max > g_45_135_min);
    bool c_45_135 = (g_45_135_max > (g_45_135_min * kDetectRatio)) && (g_45_135_max > kDetectThres) && (g_45_135_max > g_0_90_min);
    bool c_g_0_90 = g_0_90_max == g_0;
    bool c_g_45_135 = g_45_135_max == g_45;

    float f_e_0_90 = (c_0_90 && c_45_135) ? e_0_90 : 1.0;
    float f_e_45_135 = (c_0_90 && c_45_135) ? e_45_135 : 1.0;

    float weight_0 = (c_0_90 && c_g_0_90) ? f_e_0_90 : 0.0;
    float weight_90 = (c_0_90 && !c_g_0_90) ? f_e_0_90 : 0.0;
    float weight_45 = (c_45_135 && c_g_45_135) ? f_e_45_135 : 0.0;
    float weight_135 = (c_45_135 && !c_g_45_135) ? f_e_45_135 : 0.0;
    return vec4(weight_0, weight_90, weight_45, weight_135);
}

float CalcLTI(float p0, float p1, float p2, float p3, float p4, float p5, int phase_index)
{
    bool selector = (phase_index <= kPhaseCount / 2);
    float sel = selector ? p0 : p3;
    float a_min = min(min(p1, p2), sel);
    float a_max = max(max(p1, p2), sel);
    sel = selector ? p2 : p5;
    float b_min = min(min(p3, p4), sel);
    float b_max = max(max(p3, p4), sel);
    float a_cont = a_max - a_min;
    float b_cont = b_max - b_min;
    float cont_ratio = max(a_cont, b_cont) / (min(a_cont, b_cont) + kEps);
    return (1.0 - clamp((cont_ratio - kMinContrastRatio) * kRatioNorm, 0.0, 1.0)) * kContrastBoost;
}

float EvalPoly6(float pxl[6], int phase_int)
{
    phase_int = clamp(phase_int, 0, kPhaseCount - 1);
    float cs[6], cu[6];
    Scaler(phase_int, cs);
    Usm(phase_int, cu);
    float y = 0.0, y_usm = 0.0;
    for (int i = 0; i < 6; ++i) {
        y += cs[i] * pxl[i];
        y_usm += cu[i] * pxl[i];
    }
    // piece-wise ramp: sharpen and limit as a function of luma
    float y_scale = 1.0 - clamp((y - kSharpStartY) * kSharpScaleY, 0.0, 1.0);
    float y_sharpness = y_scale * kSharpStrengthScale + kSharpStrengthMin;
    y_usm *= y_sharpness;
    float y_sharpness_limit = (y_scale * kSharpLimitScale + kSharpLimitMin) * y;
    y_usm = min(y_sharpness_limit, max(-y_sharpness_limit, y_usm));
    // reduce ringing
    y_usm *= CalcLTI(pxl[0], pxl[1], pxl[2], pxl[3], pxl[4], pxl[5], phase_int);
    return y + y_usm;
}

float FilterNormal(int phase_x, int phase_y)
{
    float cx[6], cy[6];
    Scaler(phase_x, cx);
    Scaler(phase_y, cy);
    float h_acc = 0.0;
    for (int j = 0; j < 6; ++j) {
        float v_acc = 0.0;
        for (int i = 0; i < 6; ++i) {
            v_acc += P(i, j) * cy[i];
        }
        h_acc += v_acc * cx[j];
    }
    return h_acc;
}

float AddDirFilters(float fx, float fy, int phase_x, int phase_y, vec4 w)
{
    float f = 0.0;
    float v[6];
    if (w.x > 0.0) { // 0 deg
        for (int i = 0; i < 6; ++i) {
            v[i] = mix(P(i, 2), P(i, 3), fx);
        }
        f += EvalPoly6(v, phase_y) * w.x;
    }
    if (w.y > 0.0) { // 90 deg
        for (int i = 0; i < 6; ++i) {
            v[i] = mix(P(2, i), P(3, i), fy);
        }
        f += EvalPoly6(v, phase_x) * w.y;
    }
    if (w.z > 0.0) { // 45 deg
        float b45 = 0.5 + 0.5 * (fx - fy);
        float t[7];
        t[1] = mix(P(2, 1), P(1, 2), b45);
        t[3] = mix(P(3, 2), P(2, 3), b45);
        t[5] = mix(P(4, 3), P(3, 4), b45);
        b45 -= 0.5;
        float a = (b45 >= 0.0) ? P(0, 2) : P(2, 0);
        float b = (b45 >= 0.0) ? P(1, 3) : P(3, 1);
        float c = (b45 >= 0.0) ? P(2, 4) : P(4, 2);
        float d = (b45 >= 0.0) ? P(3, 5) : P(5, 3);
        t[0] = mix(P(1, 1), a, abs(b45));
        t[2] = mix(P(2, 2), b, abs(b45));
        t[4] = mix(P(3, 3), c, abs(b45));
        t[6] = mix(P(4, 4), d, abs(b45));
        float p45 = fx + fy;
        int o = 0;
        if (p45 >= 1.0) {
            o = 1;
            p45 -= 1.0;
        }
        for (int i = 0; i < 6; i++) {
            v[i] = t[i + o];
        }
        f += EvalPoly6(v, int(p45 * 64.0)) * w.z;
    }
    if (w.w > 0.0) { // 135 deg
        float b135 = 0.5 * (fx + fy);
        float t[7];
        t[1] = mix(P(3, 1), P(4, 2), b135);
        t[3] = mix(P(2, 2), P(3, 3), b135);
        t[5] = mix(P(1, 3), P(2, 4), b135);
        b135 -= 0.5;
        float a = (b135 >= 0.0) ? P(5, 2) : P(3, 0);
        float b = (b135 >= 0.0) ? P(4, 3) : P(2, 1);
        float c = (b135 >= 0.0) ? P(3, 4) : P(1, 2);
        float d = (b135 >= 0.0) ? P(2, 5) : P(0, 3);
        t[0] = mix(P(4, 1), a, abs(b135));
        t[2] = mix(P(3, 2), b, abs(b135));
        t[4] = mix(P(2, 3), c, abs(b135));
        t[6] = mix(P(1, 4), d, abs(b135));
        float p135 = 1.0 + (fx - fy);
        int o = 0;
        if (p135 >= 1.0) {
            o = 1;
            p135 -= 1.0;
        }
        for (int i = 0; i < 6; i++) {
            v[i] = t[i + o];
        }
        f += EvalPoly6(v, int(p135 * 64.0)) * w.w;
    }
    return f;
}

void main()
{
    vec2 src = gl_FragCoord.xy * (in_InSize / in_OutSize) - 0.5;
    vec2 fl = floor(src);
    float fx = src.x - fl.x, fy = src.y - fl.y;
    int phase_x = min(int(fx * float(kPhaseCount)), kPhaseCount - 1);
    int phase_y = min(int(fy * float(kPhaseCount)), kPhaseCount - 1);
    ivec2 i0 = ivec2(fl) - 2;

    // 6x6 luma support, rows are y
    for (int r = 0; r < 6; r++) {
        for (int c = 0; c < 6; c++) {
            P(r, c) = LoadY(i0 + ivec2(c, r));
        }
    }
    // Edge weights of the 2x2 texels around the sample, interpolated
    vec4 e00 = GetEdgeMap(1, 1), e01 = GetEdgeMap(1, 2);
    vec4 e10 = GetEdgeMap(2, 1), e11 = GetEdgeMap(2, 2);
    vec4 w = mix(mix(e00, e01, fx), mix(e10, e11, fx), fy);
    float baseWeight = 1.0 - w.x - w.y - w.z - w.w;

    float opY = FilterNormal(phase_x, phase_y) * baseWeight;
    opY += AddDirFilters(fx, fy, phase_x, phase_y, w);

    // Chroma from a bilinear tap, luma from the scaler
    vec4 op = textureLod(tex, (src + 0.5) / in_InSize, 0.0);
    float y = GetY(op.rgb);
    op.rgb = clamp(op.rgb + (opY - y), 0.0, 1.0);
    out_Color = op;
}
)";

    const char *frag_src = NULL;
    switch (type) {
    case ShaderType::Mask: frag_src = mask_frag_src; break;
    // case ShaderType::Blit: frag_src = image_frag_src; break;
    case ShaderType::BlitGamma: frag_src = image_gamma_frag_src; break;
    case ShaderType::Logo: frag_src = xemu_logo_frag_src; break;
    case ShaderType::FsrEasu: frag_src = fsr_easu_frag_src; break;
    case ShaderType::FsrRcas: frag_src = fsr_rcas_frag_src; break;
    case ShaderType::Sgsr: frag_src = sgsr_frag_src; break;
    case ShaderType::Lanczos: frag_src = lanczos_frag_src; break;
    case ShaderType::Bicubic: frag_src = bicubic_frag_src; break;
    case ShaderType::Nis: frag_src = nis_frag_src; break;
    default: assert(0);
    }
    GLuint frag = Shader(GL_FRAGMENT_SHADER, frag_src);
    assert(frag != 0);

    // Link vertex and fragment shaders
    s->prog = glCreateProgram();
    glAttachShader(s->prog, vert);
    glAttachShader(s->prog, frag);
    glBindFragDataLocation(s->prog, 0, "out_Color");
    glLinkProgram(s->prog);
    glUseProgram(s->prog);

    // Flag shaders for deletion when program is deleted
    glDeleteShader(vert);
    glDeleteShader(frag);

    s->flipy_loc = glGetUniformLocation(s->prog, "in_FlipY");
    s->scale_offset_loc = glGetUniformLocation(s->prog, "in_ScaleOffset");
    s->tex_scale_offset_loc =
        glGetUniformLocation(s->prog, "in_TexScaleOffset");
    s->tex_loc = glGetUniformLocation(s->prog, "tex");
    s->color_primary_loc = glGetUniformLocation(s->prog, "in_ColorPrimary");
    s->color_secondary_loc = glGetUniformLocation(s->prog, "in_ColorSecondary");
    s->color_fill_loc = glGetUniformLocation(s->prog, "in_ColorFill");
    s->time_loc = glGetUniformLocation(s->prog, "iTime");
    s->scale_loc = glGetUniformLocation(s->prog, "scale");
    s->in_size_loc = glGetUniformLocation(s->prog, "in_InSize");
    s->out_size_loc = glGetUniformLocation(s->prog, "in_OutSize");
    s->sharpness_loc = glGetUniformLocation(s->prog, "in_Sharpness");
    s->viewport_info_loc = glGetUniformLocation(s->prog, "in_ViewportInfo");
    s->coef_loc = glGetUniformLocation(s->prog, "coef");
    s->nis_loc[0] = glGetUniformLocation(s->prog, "in_Nis0");
    s->nis_loc[1] = glGetUniformLocation(s->prog, "in_Nis1");
    s->nis_loc[2] = glGetUniformLocation(s->prog, "in_Nis2");
    for (int i = 0; i < 256; i++) {
        char name[64];
        snprintf(name, sizeof(name), "palette[%d]", i);
        s->palette_loc[i] = glGetUniformLocation(s->prog, name);
    }

    const GLfloat verts[6][4] = {
        //  x      y      s      t
        { -1.0f, -1.0f,  0.0f,  0.0f }, // BL
        { -1.0f,  1.0f,  0.0f,  1.0f }, // TL
        {  1.0f,  1.0f,  1.0f,  1.0f }, // TR
        {  1.0f, -1.0f,  1.0f,  0.0f }, // BR
    };
    const GLint indicies[] = { 0, 1, 2, 3 };

    glGenVertexArrays(1, &s->vao);
    glBindVertexArray(s->vao);

    glGenBuffers(1, &s->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts,  GL_STATIC_COPY);

    glGenBuffers(1, &s->ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indicies), indicies, GL_STATIC_DRAW);

    GLint loc = glGetAttribLocation(s->prog, "in_Position");
    if (loc >= 0) {
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), (void*)0);
        glEnableVertexAttribArray(loc);
    }

    loc = glGetAttribLocation(s->prog, "in_Texcoord");
    if (loc >= 0) {
        glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), (void*)(2*sizeof(GLfloat)));
        glEnableVertexAttribArray(loc);
    }

    return s;
}

static void RenderDecal(DecalShader *s, float x, float y, float w, float h,
                        float tex_x, float tex_y, float tex_w, float tex_h,
                        uint32_t primary, uint32_t secondary, uint32_t fill)
{
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    float ww = vp[2], wh = vp[3];

    x = (int)x;
    y = (int)y;
    w = (int)w;
    h = (int)h;
    tex_x = (int)tex_x;
    tex_y = (int)tex_y;
    tex_w = (int)tex_w;
    tex_h = (int)tex_h;

    int tw_i, th_i;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &tw_i);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th_i);
    float tw = tw_i, th = th_i;

#define COL(color, c) (float)(((color) >> ((c)*8)) & 0xff) / 255.0
    if (s->flipy_loc >= 0) {
        glUniform1i(s->flipy_loc, s->flip);
    }
    if (s->scale_offset_loc >= 0) {
        glUniform4f(s->scale_offset_loc, w / ww, h / wh, -1 + ((2 * x + w) / ww),
                    -1 + ((2 * y + h) / wh));
    }
    if (s->tex_scale_offset_loc >= 0) {
        glUniform4f(s->tex_scale_offset_loc, tex_w / tw, tex_h / th, tex_x / tw,
                    tex_y / th);
    }
    if (s->tex_loc >= 0) {
        glUniform1i(s->tex_loc, 0);
    }
    if (s->color_primary_loc >= 0) {
        glUniform4f(s->color_primary_loc, COL(primary, 3), COL(primary, 2),
                    COL(primary, 1), COL(primary, 0));
    }
    if (s->color_secondary_loc >= 0) {
        glUniform4f(s->color_secondary_loc, COL(secondary, 3), COL(secondary, 2),
                    COL(secondary, 1), COL(secondary, 0));
    }
    if (s->color_fill_loc >= 0) {
        glUniform4f(s->color_fill_loc, COL(fill, 3), COL(fill, 2), COL(fill, 1),
                    COL(fill, 0));
    }
    if (s->time_loc >= 0) {
        glUniform1f(s->time_loc, s->time/1000.0f);
    }
    if (s->scale_loc >= 0) {
        glUniform1f(s->scale_loc, s->scale);
    }
#undef COL
    glDrawElements(GL_TRIANGLE_FAN, 4, GL_UNSIGNED_INT, NULL);
}

struct rect {
    int x, y, w, h;
};

static const struct rect tex_items[] = {
    { 0, 148, 467, 364 }, // obj_controller
    { 0, 81, 67, 67 }, // obj_lstick
    { 0, 14, 67, 67 }, // obj_rstick
    { 67, 104, 68, 44 }, // obj_port_socket
    { 67, 76, 28, 28 }, // obj_port_lbl_1
    { 67, 48, 28, 28 }, // obj_port_lbl_2
    { 67, 20, 28, 28 }, // obj_port_lbl_3
    { 95, 76, 28, 28 }, // obj_port_lbl_4
    { 0, 0, 512, 512 } // obj_xmu
};

enum tex_item_names {
    obj_controller,
    obj_lstick,
    obj_rstick,
    obj_port_socket,
    obj_port_lbl_1,
    obj_port_lbl_2,
    obj_port_lbl_3,
    obj_port_lbl_4,
    obj_xmu
};

void InitCustomRendering(void)
{
    glActiveTexture(GL_TEXTURE0);
    g_controller_duke_tex =
        LoadTextureFromMemory(controller_mask_data, controller_mask_size);
    g_controller_s_tex =
        LoadTextureFromMemory(controller_mask_s_data, controller_mask_s_size);
    g_decal_shader = NewDecalShader(ShaderType::Mask);
    controller_fbo = new Fbo(512, 512);

    g_xmu_tex = LoadTextureFromMemory(xmu_mask_data, xmu_mask_size);
    xmu_fbo = new Fbo(512, 256);

    g_logo_tex = LoadTextureFromMemory(logo_sdf_data, logo_sdf_size);
    g_logo_shader = NewDecalShader(ShaderType::Logo);
    logo_fbo = new Fbo(512, 512);

    g_icon_tex = LoadTextureFromMemory(xemu_64x64_data, xemu_64x64_size, false);

    g_framebuffer_shader = NewDecalShader(ShaderType::BlitGamma);
    g_fsr_easu_shader = NewDecalShader(ShaderType::FsrEasu);
    g_fsr_rcas_shader = NewDecalShader(ShaderType::FsrRcas);
    g_sgsr_shader = NewDecalShader(ShaderType::Sgsr);
    g_lanczos_shader = NewDecalShader(ShaderType::Lanczos);
    g_bicubic_shader = NewDecalShader(ShaderType::Bicubic);
    g_nis_shader = NewDecalShader(ShaderType::Nis);
    g_nis_coef_tex = CreateNisCoefTexture();
}

static void RenderMeter(DecalShader *s, float x, float y, float width,
                        float height, float p, uint32_t color_bg,
                        uint32_t color_fg)
{
    RenderDecal(s, x, y, width, height, 0, 0, 1, 1, 0, 0, color_bg);
    RenderDecal(s, x, y, width * p, height, 0, 0, 1, 1, 0, 0, color_fg);
}

static void RenderDukeController(float frame_x, float frame_y, uint32_t primary_color,
                      uint32_t secondary_color, ControllerState *state)
{
    // Location within the controller texture of masked button locations,
    // relative to the origin of the controller
    const struct rect jewel      = { 177, 172, 113, 118 };
    const struct rect lstick_ctr = {  93, 246,   0,   0 };
    const struct rect rstick_ctr = { 342, 148,   0,   0 };
    const struct rect buttons[12] = {
        { 367, 187, 30, 38 }, // A
        { 368, 229, 30, 38 }, // B
        { 330, 204, 30, 38 }, // X
        { 331, 247, 30, 38 }, // Y
        {  82, 121, 31, 47 }, // D-Left
        { 104, 160, 44, 25 }, // D-Up
        { 141, 121, 31, 47 }, // D-Right
        { 104, 105, 44, 25 }, // D-Down
        { 187,  94, 34, 24 }, // Back
        { 246,  94, 36, 26 }, // Start
        { 348, 288, 30, 38 }, // White
        { 386, 268, 30, 38 }, // Black
    };

    uint8_t alpha = 0;
    uint32_t now = SDL_GetTicks();
    float t;

    glUseProgram(g_decal_shader->prog);
    glBindVertexArray(g_decal_shader->vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_controller_duke_tex);

    // Add a 5 pixel space around the controller so we can wiggle the controller
    // around to visualize rumble in action
    frame_x += 5;
    frame_y += 5;
    float original_frame_x = frame_x;
    float original_frame_y = frame_y;

    // Floating point versions that will get scaled
    float rumble_l = 0;
    float rumble_r = 0;

    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ZERO);

    uint32_t jewel_color = secondary_color;

    // Check to see if the guide button is pressed
    const uint32_t animate_guide_button_duration = 2000;
    if (state->buttons & CONTROLLER_BUTTON_GUIDE) {
        state->animate_guide_button_end = now + animate_guide_button_duration;
    }

    if (now < state->animate_guide_button_end) {
        t = 1.0f - (float)(state->animate_guide_button_end-now)/(float)animate_guide_button_duration;
        float sin_wav = (1-sin(M_PI * t / 2.0f));

        // Animate guide button by highlighting logo jewel and fading out over time
        alpha = sin_wav * 255.0f;
        jewel_color = primary_color + alpha;

        // Add a little extra flare: wiggle the frame around while we rumble
        frame_x += ((float)(rand() % 5)-2.5) * (1-t);
        frame_y += ((float)(rand() % 5)-2.5) * (1-t);
        rumble_l = rumble_r = sin_wav;
    }

    // Render controller texture
    RenderDecal(g_decal_shader, frame_x + 0, frame_y + 0,
                tex_items[obj_controller].w, tex_items[obj_controller].h,
                tex_items[obj_controller].x, tex_items[obj_controller].y,
                tex_items[obj_controller].w, tex_items[obj_controller].h,
                primary_color, secondary_color, 0);

    glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE); // Blend with controller cutouts
    RenderDecal(g_decal_shader, frame_x + jewel.x, frame_y + jewel.y, jewel.w,
                jewel.h, 0, 0, 1, 1, 0, 0, jewel_color);

    // The controller has alpha cutouts where the buttons are. Draw a surface
    // behind the buttons if they are activated
    for (int i = 0; i < 12; i++) {
        if (state->buttons & (1 << i)) {
            RenderDecal(g_decal_shader, frame_x + buttons[i].x,
                        frame_y + buttons[i].y, buttons[i].w, buttons[i].h, 0,
                        0, 1, 1, 0, 0, primary_color + 0xff);
        }
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Blend with controller

    // Render left thumbstick
    float w = tex_items[obj_lstick].w;
    float h = tex_items[obj_lstick].h;
    float c_x = frame_x+lstick_ctr.x;
    float c_y = frame_y+lstick_ctr.y;
    float lstick_x = (float)state->axis[CONTROLLER_AXIS_LSTICK_X]/32768.0;
    float lstick_y = (float)state->axis[CONTROLLER_AXIS_LSTICK_Y]/32768.0;
    RenderDecal(g_decal_shader, (int)(c_x - w / 2.0f + 10.0f * lstick_x),
                (int)(c_y - h / 2.0f + 10.0f * lstick_y), w, h,
                tex_items[obj_lstick].x, tex_items[obj_lstick].y, w, h,
                (state->buttons & CONTROLLER_BUTTON_LSTICK) ? secondary_color :
                                                              primary_color,
                (state->buttons & CONTROLLER_BUTTON_LSTICK) ? primary_color :
                                                              secondary_color,
                0);

    // Render right thumbstick
    w = tex_items[obj_rstick].w;
    h = tex_items[obj_rstick].h;
    c_x = frame_x+rstick_ctr.x;
    c_y = frame_y+rstick_ctr.y;
    float rstick_x = (float)state->axis[CONTROLLER_AXIS_RSTICK_X]/32768.0;
    float rstick_y = (float)state->axis[CONTROLLER_AXIS_RSTICK_Y]/32768.0;
    RenderDecal(g_decal_shader, (int)(c_x - w / 2.0f + 10.0f * rstick_x),
                (int)(c_y - h / 2.0f + 10.0f * rstick_y), w, h,
                tex_items[obj_rstick].x, tex_items[obj_rstick].y, w, h,
                (state->buttons & CONTROLLER_BUTTON_RSTICK) ? secondary_color :
                                                              primary_color,
                (state->buttons & CONTROLLER_BUTTON_RSTICK) ? primary_color :
                                                              secondary_color,
                0);

    glBlendFunc(GL_ONE, GL_ZERO); // Don't blend, just overwrite values in buffer

    // Render trigger bars
    float ltrig = state->axis[CONTROLLER_AXIS_LTRIG] / 32767.0;
    float rtrig = state->axis[CONTROLLER_AXIS_RTRIG] / 32767.0;
    const uint32_t animate_trigger_duration = 1000;
    if ((ltrig > 0) || (rtrig > 0)) {
        state->animate_trigger_end = now + animate_trigger_duration;
        rumble_l = fmax(rumble_l, ltrig);
        rumble_r = fmax(rumble_r, rtrig);
    }

    // Animate trigger alpha down after a period of inactivity
    alpha = 0x80;
    if (state->animate_trigger_end > now) {
        t = 1.0f - (float)(state->animate_trigger_end-now)/(float)animate_trigger_duration;
        float sin_wav = (1-sin(M_PI * t / 2.0f));
        alpha += fmin(sin_wav * 0x40, 0x80);
    }

    RenderMeter(g_decal_shader, original_frame_x + 10,
                original_frame_y + tex_items[obj_controller].h + 20, 150, 5,
                ltrig, primary_color + alpha, primary_color + 0xff);
    RenderMeter(g_decal_shader,
                original_frame_x + tex_items[obj_controller].w - 160,
                original_frame_y + tex_items[obj_controller].h + 20, 150, 5,
                rtrig, primary_color + alpha, primary_color + 0xff);

    // Apply rumble updates
    state->rumble_l = (int)(rumble_l * (float)0xffff);
    state->rumble_r = (int)(rumble_r * (float)0xffff);

    glBindVertexArray(0);
    glUseProgram(0);
}

static void RenderControllerS(float frame_x, float frame_y, uint32_t primary_color,
                        uint32_t secondary_color, ControllerState *state)
{
    // Location within the controller texture of masked button locations,
    // relative to the origin of the controller
    const struct rect jewel = { 194, 213, 84, 84 };
    const struct rect lstick_ctr = { 103, 254, 0, 0 };
    const struct rect rstick_ctr = { 295, 176, 0, 0 };
    const struct rect buttons[12] = {
        { 347, 200, 34, 34 }, // A
        { 381, 235, 34, 34 }, // B
        { 313, 235, 34, 34 }, // X
        { 347, 270, 34, 34 }, // Y
        { 123, 165, 31, 26 }, // D-Left
        { 150, 187, 26, 31 }, // D-Up
        { 173, 165, 31, 26 }, // D-Right
        { 150, 135, 26, 31 }, // D-Down
        { 45, 195, 20, 24 }, // Back
        { 70, 163, 26, 26 }, // Start
        { 352, 145, 30, 30 }, // White
        { 388, 172, 30, 30 }, // Black
    };

    uint8_t alpha = 0;
    uint32_t now = SDL_GetTicks();
    float t;

    glUseProgram(g_decal_shader->prog);
    glBindVertexArray(g_decal_shader->vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_controller_s_tex);

    // Add a 5 pixel space around the controller so we can wiggle the controller
    // around to visualize rumble in action
    frame_x += 5;
    frame_y += 5;
    float original_frame_x = frame_x;
    float original_frame_y = frame_y;

    // Floating point versions that will get scaled
    float rumble_l = 0;
    float rumble_r = 0;

    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ZERO);

    uint32_t jewel_color = secondary_color;

    // Check to see if the guide button is pressed
    const uint32_t animate_guide_button_duration = 2000;
    if (state->buttons & CONTROLLER_BUTTON_GUIDE) {
        state->animate_guide_button_end =
            now + animate_guide_button_duration;
    }

    if (now < state->animate_guide_button_end) {
        t = 1.0f - (float)(state->animate_guide_button_end - now) /
                       (float)animate_guide_button_duration;
        float sin_wav = (1 - sin(M_PI * t / 2.0f));

        // Animate guide button by highlighting logo jewel and fading out over
        // time
        alpha = sin_wav * 255.0f;
        jewel_color = primary_color + alpha;

        // Add a little extra flare: wiggle the frame around while we rumble
        frame_x += ((float)(rand() % 5) - 2.5) * (1 - t);
        frame_y += ((float)(rand() % 5) - 2.5) * (1 - t);
        rumble_l = rumble_r = sin_wav;
    }

    // Render controller texture
    RenderDecal(g_decal_shader, frame_x + 0, frame_y + 0,
                tex_items[obj_controller].w, tex_items[obj_controller].h,
                tex_items[obj_controller].x, tex_items[obj_controller].y,
                tex_items[obj_controller].w, tex_items[obj_controller].h,
                primary_color, secondary_color, 0);

    glBlendFunc(GL_ONE_MINUS_DST_ALPHA,
                GL_ONE); // Blend with controller cutouts
    RenderDecal(g_decal_shader, frame_x + jewel.x, frame_y + jewel.y, jewel.w,
                jewel.h, 0, 0, 1, 1, 0, 0, jewel_color);

    // The controller has alpha cutouts where the buttons are. Draw a surface
    // behind the buttons if they are activated
    for (int i = 0; i < 12; i++) {
        if (state->buttons & (1 << i)) {
            RenderDecal(g_decal_shader, frame_x + buttons[i].x,
                        frame_y + buttons[i].y, buttons[i].w, buttons[i].h, 0,
                        0, 1, 1, 0, 0, primary_color + 0xff);
        }
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Blend with controller

    // Render left thumbstick
    float w = tex_items[obj_lstick].w;
    float h = tex_items[obj_lstick].h;
    float c_x = frame_x + lstick_ctr.x;
    float c_y = frame_y + lstick_ctr.y;
    float lstick_x = (float)state->axis[CONTROLLER_AXIS_LSTICK_X] / 32768.0;
    float lstick_y = (float)state->axis[CONTROLLER_AXIS_LSTICK_Y] / 32768.0;
    RenderDecal(
        g_decal_shader, (int)(c_x - w / 2.0f + 10.0f * lstick_x),
        (int)(c_y - h / 2.0f + 10.0f * lstick_y), w, h, tex_items[obj_lstick].x,
        tex_items[obj_lstick].y, w, h,
        (state->buttons & CONTROLLER_BUTTON_LSTICK) ? secondary_color :
                                                         primary_color,
        (state->buttons & CONTROLLER_BUTTON_LSTICK) ? primary_color :
                                                         secondary_color,
        0);

    // Render right thumbstick
    w = tex_items[obj_rstick].w;
    h = tex_items[obj_rstick].h;
    c_x = frame_x + rstick_ctr.x;
    c_y = frame_y + rstick_ctr.y;
    float rstick_x = (float)state->axis[CONTROLLER_AXIS_RSTICK_X] / 32768.0;
    float rstick_y = (float)state->axis[CONTROLLER_AXIS_RSTICK_Y] / 32768.0;
    RenderDecal(
        g_decal_shader, (int)(c_x - w / 2.0f + 10.0f * rstick_x),
        (int)(c_y - h / 2.0f + 10.0f * rstick_y), w, h, tex_items[obj_rstick].x,
        tex_items[obj_rstick].y, w, h,
        (state->buttons & CONTROLLER_BUTTON_RSTICK) ? secondary_color :
                                                         primary_color,
        (state->buttons & CONTROLLER_BUTTON_RSTICK) ? primary_color :
                                                         secondary_color,
        0);

    glBlendFunc(GL_ONE,
                GL_ZERO); // Don't blend, just overwrite values in buffer

    // Render trigger bars
    float ltrig = state->axis[CONTROLLER_AXIS_LTRIG] / 32767.0;
    float rtrig = state->axis[CONTROLLER_AXIS_RTRIG] / 32767.0;
    const uint32_t animate_trigger_duration = 1000;
    if ((ltrig > 0) || (rtrig > 0)) {
        state->animate_trigger_end = now + animate_trigger_duration;
        rumble_l = fmax(rumble_l, ltrig);
        rumble_r = fmax(rumble_r, rtrig);
    }

    // Animate trigger alpha down after a period of inactivity
    alpha = 0x80;
    if (state->animate_trigger_end > now) {
        t = 1.0f - (float)(state->animate_trigger_end - now) /
                       (float)animate_trigger_duration;
        float sin_wav = (1 - sin(M_PI * t / 2.0f));
        alpha += fmin(sin_wav * 0x40, 0x80);
    }

    RenderMeter(g_decal_shader, original_frame_x + 10,
                original_frame_y + tex_items[obj_controller].h + 20, 150, 5,
                ltrig, primary_color + alpha, primary_color + 0xff);
    RenderMeter(g_decal_shader,
                original_frame_x + tex_items[obj_controller].w - 160,
                original_frame_y + tex_items[obj_controller].h + 20, 150, 5,
                rtrig, primary_color + alpha, primary_color + 0xff);

    // Apply rumble updates
    state->rumble_l = (int)(rumble_l * (float)0xffff);
    state->rumble_r = (int)(rumble_r * (float)0xffff);

    glBindVertexArray(0);
    glUseProgram(0);
}

void RenderController(float frame_x, float frame_y, uint32_t primary_color,
                      uint32_t secondary_color, ControllerState *state)
{
    if (strcmp(bound_drivers[state->bound], DRIVER_S) == 0)
        RenderControllerS(frame_x, frame_y, primary_color, secondary_color,
                          state);
    else if (strcmp(bound_drivers[state->bound], DRIVER_DUKE) == 0)
        RenderDukeController(frame_x, frame_y, primary_color, secondary_color,
                             state);
}

void RenderControllerPort(float frame_x, float frame_y, int i,
                          uint32_t port_color)
{
    glUseProgram(g_decal_shader->prog);
    glBindVertexArray(g_decal_shader->vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_controller_duke_tex);
    glBlendFunc(GL_ONE, GL_ZERO);

    // Render port socket
    RenderDecal(g_decal_shader, frame_x, frame_y, tex_items[obj_port_socket].w,
                tex_items[obj_port_socket].h, tex_items[obj_port_socket].x,
                tex_items[obj_port_socket].y, tex_items[obj_port_socket].w,
                tex_items[obj_port_socket].h, port_color, port_color, 0);

    frame_x += (tex_items[obj_port_socket].w-tex_items[obj_port_lbl_1].w)/2;
    frame_y += tex_items[obj_port_socket].h + 8;

    // Render port label
    RenderDecal(
        g_decal_shader, frame_x, frame_y, tex_items[obj_port_lbl_1 + i].w,
        tex_items[obj_port_lbl_1 + i].h, tex_items[obj_port_lbl_1 + i].x,
        tex_items[obj_port_lbl_1 + i].y, tex_items[obj_port_lbl_1 + i].w,
        tex_items[obj_port_lbl_1 + i].h, port_color, port_color, 0);

    glBindVertexArray(0);
    glUseProgram(0);
}

void RenderXmu(float frame_x, float frame_y, uint32_t primary_color,
               uint32_t secondary_color)
{
    glUseProgram(g_decal_shader->prog);
    glBindVertexArray(g_decal_shader->vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_xmu_tex);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ZERO);

    // Render xmu
    RenderDecal(g_decal_shader, frame_x, frame_y, 256, 256,
                tex_items[obj_xmu].x, tex_items[obj_xmu].y,
                tex_items[obj_xmu].w, tex_items[obj_xmu].h, primary_color,
                secondary_color, 0);

    glBindVertexArray(0);
    glUseProgram(0);
}

void RenderLogo(uint32_t time)
{
    uint32_t color = 0x62ca13ff;

    g_logo_shader->time = time;
    glUseProgram(g_logo_shader->prog);
    glBindVertexArray(g_decal_shader->vao);
    glBlendFunc(GL_ONE, GL_ZERO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_logo_tex);
    RenderDecal(g_logo_shader, 0, 0, 512, 512, 0, 0, 128, 128, color,
        color, 0x00000000);
    glBindVertexArray(0);
    glUseProgram(0);
}

// Scale <src> proportionally to fit in <max>
void ScaleDimensions(int src_width, int src_height, int max_width, int max_height, int *out_width, int *out_height)
{
    float w_ratio = (float)max_width/(float)max_height;
    float t_ratio = (float)src_width/(float)src_height;

    if (w_ratio >= t_ratio) {
        *out_width = (float)max_width * t_ratio/w_ratio;
        *out_height = max_height;
    } else {
        *out_width = max_width;
        *out_height = (float)max_height * w_ratio/t_ratio;
    }
}

// Upload the DAC gamma palette in one call instead of 256 separate
// glUniform1ui calls per frame.
static void UploadPalette(DecalShader *s)
{
    const uint8_t *palette = nv2a_get_dac_palette();
    GLuint entries[256];
    for (int i = 0; i < 256; i++) {
        entries[i] = (palette[i * 3 + 2] << 16) | (palette[i * 3 + 1] << 8) |
                     palette[i * 3];
    }
    glUniform1uiv(s->palette_loc[0], 256, entries);
}

static Fbo *EnsureFbo(Fbo *fbo, int w, int h)
{
    if (fbo && fbo->w == w && fbo->h == h) {
        return fbo;
    }
    delete fbo;
    return new Fbo(w, h);
}

static void DrawFullscreenPass(DecalShader *s, GLuint src_tex, float sx,
                               float sy, int flip = 0)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUseProgram(s->prog);
    glBindVertexArray(s->vao);
    glUniform1i(s->flipy_loc, flip);
    glUniform4f(s->scale_offset_loc, sx, sy, 0, 0);
    glUniform4f(s->tex_scale_offset_loc, 1.0, 1.0, 0, 0);
    glUniform1i(s->tex_loc, 0);
    glDrawElements(GL_TRIANGLE_FAN, 4, GL_UNSIGNED_INT, NULL);
}

// NVIDIA Image Scaling filter banks from NIS_Config.h: 6-tap scaler and
// unsharp-mask coefficients for 64 phases.
//
// NVIDIA Image Scaling SDK v1.0.3
// Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.
static const float kNisCoefScale[64][6] = {
    { 0.0f, 0.0f, 1.0000f, 0.0f, 0.0f, 0.0f },
    { 0.0029f, -0.0127f, 1.0000f, 0.0132f, -0.0034f, 0.0f },
    { 0.0063f, -0.0249f, 0.9985f, 0.0269f, -0.0068f, 0.0f },
    { 0.0088f, -0.0361f, 0.9956f, 0.0415f, -0.0103f, 0.0005f },
    { 0.0117f, -0.0474f, 0.9932f, 0.0562f, -0.0142f, 0.0005f },
    { 0.0142f, -0.0576f, 0.9897f, 0.0713f, -0.0181f, 0.0005f },
    { 0.0166f, -0.0674f, 0.9844f, 0.0874f, -0.0220f, 0.0010f },
    { 0.0186f, -0.0762f, 0.9785f, 0.1040f, -0.0264f, 0.0015f },
    { 0.0205f, -0.0850f, 0.9727f, 0.1206f, -0.0308f, 0.0020f },
    { 0.0225f, -0.0928f, 0.9648f, 0.1382f, -0.0352f, 0.0024f },
    { 0.0239f, -0.1006f, 0.9575f, 0.1558f, -0.0396f, 0.0029f },
    { 0.0254f, -0.1074f, 0.9487f, 0.1738f, -0.0439f, 0.0034f },
    { 0.0264f, -0.1138f, 0.9390f, 0.1929f, -0.0488f, 0.0044f },
    { 0.0278f, -0.1191f, 0.9282f, 0.2119f, -0.0537f, 0.0049f },
    { 0.0288f, -0.1245f, 0.9170f, 0.2310f, -0.0581f, 0.0059f },
    { 0.0293f, -0.1294f, 0.9058f, 0.2510f, -0.0630f, 0.0063f },
    { 0.0303f, -0.1333f, 0.8926f, 0.2710f, -0.0679f, 0.0073f },
    { 0.0308f, -0.1367f, 0.8789f, 0.2915f, -0.0728f, 0.0083f },
    { 0.0308f, -0.1401f, 0.8657f, 0.3120f, -0.0776f, 0.0093f },
    { 0.0313f, -0.1426f, 0.8506f, 0.3330f, -0.0825f, 0.0103f },
    { 0.0313f, -0.1445f, 0.8354f, 0.3540f, -0.0874f, 0.0112f },
    { 0.0313f, -0.1460f, 0.8193f, 0.3755f, -0.0923f, 0.0122f },
    { 0.0313f, -0.1470f, 0.8022f, 0.3965f, -0.0967f, 0.0137f },
    { 0.0308f, -0.1479f, 0.7856f, 0.4185f, -0.1016f, 0.0146f },
    { 0.0303f, -0.1479f, 0.7681f, 0.4399f, -0.1060f, 0.0156f },
    { 0.0298f, -0.1479f, 0.7505f, 0.4614f, -0.1104f, 0.0166f },
    { 0.0293f, -0.1470f, 0.7314f, 0.4829f, -0.1147f, 0.0181f },
    { 0.0288f, -0.1460f, 0.7119f, 0.5049f, -0.1187f, 0.0190f },
    { 0.0278f, -0.1445f, 0.6929f, 0.5264f, -0.1226f, 0.0200f },
    { 0.0273f, -0.1431f, 0.6724f, 0.5479f, -0.1260f, 0.0215f },
    { 0.0264f, -0.1411f, 0.6528f, 0.5693f, -0.1299f, 0.0225f },
    { 0.0254f, -0.1387f, 0.6323f, 0.5903f, -0.1328f, 0.0234f },
    { 0.0244f, -0.1357f, 0.6113f, 0.6113f, -0.1357f, 0.0244f },
    { 0.0234f, -0.1328f, 0.5903f, 0.6323f, -0.1387f, 0.0254f },
    { 0.0225f, -0.1299f, 0.5693f, 0.6528f, -0.1411f, 0.0264f },
    { 0.0215f, -0.1260f, 0.5479f, 0.6724f, -0.1431f, 0.0273f },
    { 0.0200f, -0.1226f, 0.5264f, 0.6929f, -0.1445f, 0.0278f },
    { 0.0190f, -0.1187f, 0.5049f, 0.7119f, -0.1460f, 0.0288f },
    { 0.0181f, -0.1147f, 0.4829f, 0.7314f, -0.1470f, 0.0293f },
    { 0.0166f, -0.1104f, 0.4614f, 0.7505f, -0.1479f, 0.0298f },
    { 0.0156f, -0.1060f, 0.4399f, 0.7681f, -0.1479f, 0.0303f },
    { 0.0146f, -0.1016f, 0.4185f, 0.7856f, -0.1479f, 0.0308f },
    { 0.0137f, -0.0967f, 0.3965f, 0.8022f, -0.1470f, 0.0313f },
    { 0.0122f, -0.0923f, 0.3755f, 0.8193f, -0.1460f, 0.0313f },
    { 0.0112f, -0.0874f, 0.3540f, 0.8354f, -0.1445f, 0.0313f },
    { 0.0103f, -0.0825f, 0.3330f, 0.8506f, -0.1426f, 0.0313f },
    { 0.0093f, -0.0776f, 0.3120f, 0.8657f, -0.1401f, 0.0308f },
    { 0.0083f, -0.0728f, 0.2915f, 0.8789f, -0.1367f, 0.0308f },
    { 0.0073f, -0.0679f, 0.2710f, 0.8926f, -0.1333f, 0.0303f },
    { 0.0063f, -0.0630f, 0.2510f, 0.9058f, -0.1294f, 0.0293f },
    { 0.0059f, -0.0581f, 0.2310f, 0.9170f, -0.1245f, 0.0288f },
    { 0.0049f, -0.0537f, 0.2119f, 0.9282f, -0.1191f, 0.0278f },
    { 0.0044f, -0.0488f, 0.1929f, 0.9390f, -0.1138f, 0.0264f },
    { 0.0034f, -0.0439f, 0.1738f, 0.9487f, -0.1074f, 0.0254f },
    { 0.0029f, -0.0396f, 0.1558f, 0.9575f, -0.1006f, 0.0239f },
    { 0.0024f, -0.0352f, 0.1382f, 0.9648f, -0.0928f, 0.0225f },
    { 0.0020f, -0.0308f, 0.1206f, 0.9727f, -0.0850f, 0.0205f },
    { 0.0015f, -0.0264f, 0.1040f, 0.9785f, -0.0762f, 0.0186f },
    { 0.0010f, -0.0220f, 0.0874f, 0.9844f, -0.0674f, 0.0166f },
    { 0.0005f, -0.0181f, 0.0713f, 0.9897f, -0.0576f, 0.0142f },
    { 0.0005f, -0.0142f, 0.0562f, 0.9932f, -0.0474f, 0.0117f },
    { 0.0005f, -0.0103f, 0.0415f, 0.9956f, -0.0361f, 0.0088f },
    { 0.0f, -0.0068f, 0.0269f, 0.9985f, -0.0249f, 0.0063f },
    { 0.0f, -0.0034f, 0.0132f, 1.0000f, -0.0127f, 0.0029f },
};
static const float kNisCoefUsm[64][6] = {
    { 0.0f, -0.6001f, 1.2002f, -0.6001f, 0.0f, 0.0f },
    { 0.0029f, -0.6084f, 1.1987f, -0.5903f, -0.0029f, 0.0f },
    { 0.0049f, -0.6147f, 1.1958f, -0.5791f, -0.0068f, 0.0005f },
    { 0.0073f, -0.6196f, 1.1890f, -0.5659f, -0.0103f, 0.0f },
    { 0.0093f, -0.6235f, 1.1802f, -0.5513f, -0.0151f, 0.0f },
    { 0.0112f, -0.6265f, 1.1699f, -0.5352f, -0.0195f, 0.0005f },
    { 0.0122f, -0.6270f, 1.1582f, -0.5181f, -0.0259f, 0.0005f },
    { 0.0142f, -0.6284f, 1.1455f, -0.5005f, -0.0317f, 0.0005f },
    { 0.0156f, -0.6265f, 1.1274f, -0.4790f, -0.0386f, 0.0005f },
    { 0.0166f, -0.6235f, 1.1089f, -0.4570f, -0.0454f, 0.0010f },
    { 0.0176f, -0.6187f, 1.0879f, -0.4346f, -0.0532f, 0.0010f },
    { 0.0181f, -0.6138f, 1.0659f, -0.4102f, -0.0615f, 0.0015f },
    { 0.0190f, -0.6069f, 1.0405f, -0.3843f, -0.0698f, 0.0015f },
    { 0.0195f, -0.6006f, 1.0161f, -0.3574f, -0.0796f, 0.0020f },
    { 0.0200f, -0.5928f, 0.9893f, -0.3286f, -0.0898f, 0.0024f },
    { 0.0200f, -0.5820f, 0.9580f, -0.2988f, -0.1001f, 0.0029f },
    { 0.0200f, -0.5728f, 0.9292f, -0.2690f, -0.1104f, 0.0034f },
    { 0.0200f, -0.5620f, 0.8975f, -0.2368f, -0.1226f, 0.0039f },
    { 0.0205f, -0.5498f, 0.8643f, -0.2046f, -0.1343f, 0.0044f },
    { 0.0200f, -0.5371f, 0.8301f, -0.1709f, -0.1465f, 0.0049f },
    { 0.0195f, -0.5239f, 0.7944f, -0.1367f, -0.1587f, 0.0054f },
    { 0.0195f, -0.5107f, 0.7598f, -0.1021f, -0.1724f, 0.0059f },
    { 0.0190f, -0.4966f, 0.7231f, -0.0649f, -0.1865f, 0.0063f },
    { 0.0186f, -0.4819f, 0.6846f, -0.0288f, -0.1997f, 0.0068f },
    { 0.0186f, -0.4668f, 0.6460f, 0.0093f, -0.2144f, 0.0073f },
    { 0.0176f, -0.4507f, 0.6055f, 0.0479f, -0.2290f, 0.0083f },
    { 0.0171f, -0.4370f, 0.5693f, 0.0859f, -0.2446f, 0.0088f },
    { 0.0161f, -0.4199f, 0.5283f, 0.1255f, -0.2598f, 0.0098f },
    { 0.0161f, -0.4048f, 0.4883f, 0.1655f, -0.2754f, 0.0103f },
    { 0.0151f, -0.3887f, 0.4497f, 0.2041f, -0.2910f, 0.0107f },
    { 0.0142f, -0.3711f, 0.4072f, 0.2446f, -0.3066f, 0.0117f },
    { 0.0137f, -0.3555f, 0.3672f, 0.2852f, -0.3228f, 0.0122f },
    { 0.0132f, -0.3394f, 0.3262f, 0.3262f, -0.3394f, 0.0132f },
    { 0.0122f, -0.3228f, 0.2852f, 0.3672f, -0.3555f, 0.0137f },
    { 0.0117f, -0.3066f, 0.2446f, 0.4072f, -0.3711f, 0.0142f },
    { 0.0107f, -0.2910f, 0.2041f, 0.4497f, -0.3887f, 0.0151f },
    { 0.0103f, -0.2754f, 0.1655f, 0.4883f, -0.4048f, 0.0161f },
    { 0.0098f, -0.2598f, 0.1255f, 0.5283f, -0.4199f, 0.0161f },
    { 0.0088f, -0.2446f, 0.0859f, 0.5693f, -0.4370f, 0.0171f },
    { 0.0083f, -0.2290f, 0.0479f, 0.6055f, -0.4507f, 0.0176f },
    { 0.0073f, -0.2144f, 0.0093f, 0.6460f, -0.4668f, 0.0186f },
    { 0.0068f, -0.1997f, -0.0288f, 0.6846f, -0.4819f, 0.0186f },
    { 0.0063f, -0.1865f, -0.0649f, 0.7231f, -0.4966f, 0.0190f },
    { 0.0059f, -0.1724f, -0.1021f, 0.7598f, -0.5107f, 0.0195f },
    { 0.0054f, -0.1587f, -0.1367f, 0.7944f, -0.5239f, 0.0195f },
    { 0.0049f, -0.1465f, -0.1709f, 0.8301f, -0.5371f, 0.0200f },
    { 0.0044f, -0.1343f, -0.2046f, 0.8643f, -0.5498f, 0.0205f },
    { 0.0039f, -0.1226f, -0.2368f, 0.8975f, -0.5620f, 0.0200f },
    { 0.0034f, -0.1104f, -0.2690f, 0.9292f, -0.5728f, 0.0200f },
    { 0.0029f, -0.1001f, -0.2988f, 0.9580f, -0.5820f, 0.0200f },
    { 0.0024f, -0.0898f, -0.3286f, 0.9893f, -0.5928f, 0.0200f },
    { 0.0020f, -0.0796f, -0.3574f, 1.0161f, -0.6006f, 0.0195f },
    { 0.0015f, -0.0698f, -0.3843f, 1.0405f, -0.6069f, 0.0190f },
    { 0.0015f, -0.0615f, -0.4102f, 1.0659f, -0.6138f, 0.0181f },
    { 0.0010f, -0.0532f, -0.4346f, 1.0879f, -0.6187f, 0.0176f },
    { 0.0010f, -0.0454f, -0.4570f, 1.1089f, -0.6235f, 0.0166f },
    { 0.0005f, -0.0386f, -0.4790f, 1.1274f, -0.6265f, 0.0156f },
    { 0.0005f, -0.0317f, -0.5005f, 1.1455f, -0.6284f, 0.0142f },
    { 0.0005f, -0.0259f, -0.5181f, 1.1582f, -0.6270f, 0.0122f },
    { 0.0005f, -0.0195f, -0.5352f, 1.1699f, -0.6265f, 0.0112f },
    { 0.0f, -0.0151f, -0.5513f, 1.1802f, -0.6235f, 0.0093f },
    { 0.0f, -0.0103f, -0.5659f, 1.1890f, -0.6196f, 0.0073f },
    { 0.0005f, -0.0068f, -0.5791f, 1.1958f, -0.6147f, 0.0049f },
    { 0.0f, -0.0029f, -0.5903f, 1.1987f, -0.6084f, 0.0029f },
};

static GLuint CreateNisCoefTexture(void)
{
    float texels[64][4][4];
    for (int ph = 0; ph < 64; ph++) {
        const float *s = kNisCoefScale[ph], *u = kNisCoefUsm[ph];
        const float row[4][4] = { { s[0], s[1], s[2], s[3] },
                                  { s[4], s[5], 0, 0 },
                                  { u[0], u[1], u[2], u[3] },
                                  { u[4], u[5], 0, 0 } };
        memcpy(texels[ph], row, sizeof(row));
    }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 4, 64, 0, GL_RGBA, GL_FLOAT, texels);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

// NVScalerUpdateConfig (SDR) from NIS_Config.h: sharpness 0..1
static void SetNisUniforms(DecalShader *s, float sharpness)
{
    sharpness = sharpness < 0.0f ? 0.0f : (sharpness > 1.0f ? 1.0f : sharpness);
    float slider = sharpness - 0.5f;
    float max_scale = slider >= 0.0f ? 1.25f : 1.75f;
    float min_scale = slider >= 0.0f ? 1.25f : 1.0f;
    float limit_scale = slider >= 0.0f ? 1.25f : 1.0f;
    float detect_ratio = 2 * 1127.f / 1024.f;
    float detect_thres = 64.0f / 1024.0f;
    float min_contrast = 2.0f, max_contrast = 10.0f;
    float sharp_start_y = 0.45f, sharp_end_y = 0.9f;
    float strength_min = fmaxf(0.0f, 0.4f + slider * min_scale * 1.2f);
    float strength_max = 1.6f + slider * max_scale * 1.8f;
    float limit_min = fmaxf(0.1f, 0.14f + slider * limit_scale * 0.32f);
    float limit_max = 0.5f + slider * limit_scale * 0.6f;
    glUniform4f(s->nis_loc[0], detect_ratio, detect_thres, min_contrast,
                1.0f / (max_contrast - min_contrast));
    glUniform4f(s->nis_loc[1], sharp_start_y, 1.0f / (sharp_end_y - sharp_start_y),
                strength_min, strength_max - strength_min);
    glUniform4f(s->nis_loc[2], limit_min, limit_max - limit_min, 1.0f, 1.0f / 255.0f);
}

// Advanced upscaling path: gamma (native res) -> scaler (output res) ->
// RCAS sharpening (to target). The scaler is FSR 1 EASU, SGSR 1, Lanczos-3
// or Catmull-Rom. Returns false when it does not apply (no upscaling
// needed), in which case the caller falls back to the regular blit.
static bool RenderFramebufferUpscaled(GLint tex, int width, int height,
                                      bool flip, float scale[2],
                                      DecalShader *scaler, bool rtx)
{
    int tw, th;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);

    int dw = (int)(scale[0] * width + 0.5f);
    int dh = (int)(scale[1] * height + 0.5f);
    if (tw < 1 || th < 1 || dw < 1 || dh < 1 || (dw <= tw && dh <= th)) {
        return false;
    }

    GLint prev_draw_fbo, prev_read_fbo;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
    bool blend = glIsEnabled(GL_BLEND);
    if (blend) {
        glDisable(GL_BLEND);
    }

    g_fsr_gamma_fbo = EnsureFbo(g_fsr_gamma_fbo, tw, th);
    g_fsr_easu_fbo = EnsureFbo(g_fsr_easu_fbo, dw, dh);

    // Scalers that filter or gather need edge clamping, not a black border
    glBindTexture(GL_TEXTURE_2D, g_fsr_gamma_fbo->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Pass 1: apply DAC gamma and orientation at native resolution, so EASU
    // analyses the image as it will actually be displayed.
    glBindFramebuffer(GL_FRAMEBUFFER, g_fsr_gamma_fbo->fbo);
    glViewport(0, 0, tw, th);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    DecalShader *s = g_framebuffer_shader;
    glUseProgram(s->prog);
    glBindVertexArray(s->vao);
    glUniform1i(s->flipy_loc, flip);
    glUniform4f(s->scale_offset_loc, 1.0, 1.0, 0, 0);
    glUniform4f(s->tex_scale_offset_loc, 1.0, 1.0, 0, 0);
    glUniform1i(s->tex_loc, 0);
    UploadPalette(s);
    glDrawElements(GL_TRIANGLE_FAN, 4, GL_UNSIGNED_INT, NULL);

    // Pass 2: upscale to the exact on-screen size. RTX Video runs in the
    // driver; if it can't, NIS (the scaler passed in) takes over.
    GLuint rtx_tex = 0;
    bool rtx_ok = rtx && RtxVideoUpscale(g_fsr_gamma_fbo->fbo, tw, th, dw, dh,
                                         &rtx_tex);
    bool nis = scaler == g_nis_shader;
    if (!rtx_ok) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_fsr_easu_fbo->fbo);
        glViewport(0, 0, dw, dh);
        glUseProgram(scaler->prog);
        glUniform2f(scaler->in_size_loc, tw, th);
        glUniform2f(scaler->out_size_loc, dw, dh);
        glUniform4f(scaler->viewport_info_loc, 1.0f / tw, 1.0f / th, tw, th);
        if (nis) {
            glUniform1i(scaler->coef_loc, 1);
            SetNisUniforms(scaler, g_config.display.fsr_sharpness);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, g_nis_coef_tex);
        }
        DrawFullscreenPass(scaler, g_fsr_gamma_fbo->tex, 1.0, 1.0);
        if (nis) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE0);
        }
    }

    // Pass 3: RCAS sharpen into the original target with the usual
    // letterboxing
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read_fbo);
    if (blend) {
        glEnable(GL_BLEND);
    }
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    // Sharpening strength 0..1 (0 = off, 1 = AMD's maximum)
    float sharp = g_config.display.fsr_sharpness;
    sharp = sharp < 0.0f ? 0.0f : (sharp > 1.0f ? 1.0f : sharp);
    if (nis || rtx_ok) {
        sharp = 0.0f; // NIS and RTX Video sharpen themselves; RCAS just copies
    }
    glUseProgram(g_fsr_rcas_shader->prog);
    glUniform2f(g_fsr_rcas_shader->in_size_loc, dw, dh);
    glUniform1f(g_fsr_rcas_shader->sharpness_loc, sharp);
    if (rtx_ok) {
        // Direct3D's top-first rows: draw flipped
        DrawFullscreenPass(g_fsr_rcas_shader, rtx_tex, scale[0], scale[1], 1);
        RtxVideoEndFrame();
    } else {
        DrawFullscreenPass(g_fsr_rcas_shader, g_fsr_easu_fbo->tex, scale[0],
                           scale[1]);
    }
    return true;
}

void RenderFramebuffer(GLint tex, int width, int height, bool flip, float scale[2])
{
    DecalShader *scaler = NULL;
    bool rtx = false;
    switch (g_config.display.filtering) {
    case CONFIG_DISPLAY_FILTERING_FSR: scaler = g_fsr_easu_shader; break;
    case CONFIG_DISPLAY_FILTERING_SGSR: scaler = g_sgsr_shader; break;
    case CONFIG_DISPLAY_FILTERING_LANCZOS: scaler = g_lanczos_shader; break;
    case CONFIG_DISPLAY_FILTERING_BICUBIC: scaler = g_bicubic_shader; break;
    case CONFIG_DISPLAY_FILTERING_NIS: scaler = g_nis_shader; break;
    case CONFIG_DISPLAY_FILTERING_RTX: scaler = g_nis_shader; rtx = true; break;
    default: break;
    }
    if (scaler && !nv2a_get_screen_off() &&
        RenderFramebufferUpscaled(tex, width, height, flip, scale, scaler, rtx)) {
        return;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    switch (g_config.display.filtering) {
    case CONFIG_DISPLAY_FILTERING_NEAREST:
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    break;
    case CONFIG_DISPLAY_FILTERING_LINEAR:
    default: // advanced scalers fall back to linear when not upscaling
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    break;
    }

    DecalShader *s = g_framebuffer_shader;
    s->flip = flip;
    glViewport(0, 0, width, height);
    glUseProgram(s->prog);
    glBindVertexArray(s->vao);
    glUniform1i(s->flipy_loc, s->flip);
    glUniform4f(s->scale_offset_loc, scale[0], scale[1], 0, 0);
    glUniform4f(s->tex_scale_offset_loc, 1.0, 1.0, 0, 0);
    glUniform1i(s->tex_loc, 0);
    UploadPalette(s);

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!nv2a_get_screen_off()) {
        glDrawElements(GL_TRIANGLE_FAN, 4, GL_UNSIGNED_INT, NULL);
    }
}

static float GetDisplayAspectRatio(int width, int height)
{
    switch (g_config.display.ui.aspect_ratio) {
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_NATIVE:
        return (float)width/(float)height;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9:
        return 16.0f/9.0f;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3:
        return 4.0f/3.0f;
    case CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO:
    default:
        return xemu_get_widescreen() ? 16.0f/9.0f : 4.0f/3.0f;
    }
}

void RenderFramebuffer(GLint tex, int width, int height, bool flip)
{
    int tw, th;
    float scale[2];

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);

    // Calculate scaling factors
    if (g_config.display.ui.fit == CONFIG_DISPLAY_UI_FIT_STRETCH) {
        // Stretch to fit
        scale[0] = 1.0;
        scale[1] = 1.0;
    } else if (g_config.display.ui.fit == CONFIG_DISPLAY_UI_FIT_CENTER) {
        // Centered
        float t_ratio = GetDisplayAspectRatio(tw, th);
        scale[0] = t_ratio*(float)th/(float)width;
        scale[1] = (float)th/(float)height;
    } else {
        float t_ratio = GetDisplayAspectRatio(tw, th);
        float w_ratio = (float)width/(float)height;
        if (w_ratio >= t_ratio) {
            scale[0] = t_ratio/w_ratio;
            scale[1] = 1.0;
        } else {
            scale[0] = 1.0;
            scale[1] = w_ratio/t_ratio;
        }
    }

    RenderFramebuffer(tex, width, height, flip, scale);
}

bool RenderFramebufferToPng(GLuint tex, bool flip, std::vector<uint8_t> &png, int max_width, int max_height)
{
    int width, height;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);

    width = height * GetDisplayAspectRatio(width, height);

    if (!max_width) max_width = width;
    if (!max_height) max_height = height;
    ScaleDimensions(width, height, max_width, max_height, &width, &height);

    std::vector<uint8_t> pixels;
    pixels.resize(width * height * 3);

    Fbo fbo(width, height);
    fbo.Target();
    bool blend = glIsEnabled(GL_BLEND);
    if (blend) glDisable(GL_BLEND);
    float scale[2] = {1.0, 1.0};
    RenderFramebuffer(tex, width, height, !flip, scale);
    if (blend) glEnable(GL_BLEND);
    glPixelStorei(GL_PACK_ROW_LENGTH, width);
    glPixelStorei(GL_PACK_IMAGE_HEIGHT, height);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    fbo.Restore();

    return fpng::fpng_encode_image_to_memory(pixels.data(), width, height, 3, png);
}

void SaveScreenshot(GLuint tex, bool flip)
{
    Error *err = NULL;
    char fname[128];
    std::vector<uint8_t> png;

    if (RenderFramebufferToPng(tex, flip, png)) {
        time_t t = time(NULL);
        struct tm *tmp = localtime(&t);
        // Include the running title's ID so the game library can show each
        // game's screenshots: xemu-4D530064-2026-10-22-09-30-00.png
        char prefix[32] = "xemu-";
        struct xbe *xbe = xemu_get_xbe_info();
        if (xbe && xbe->cert) {
            snprintf(prefix, sizeof(prefix), "xemu-%08X-",
                     (unsigned)xbe->cert->m_titleid);
        }
        if (tmp) {
            char stamp[64];
            strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H-%M-%S", tmp);
            snprintf(fname, sizeof(fname), "%s%s.png", prefix, stamp);
        } else {
            strcpy(fname, "xemu.png");
        }

        const char *output_dir = g_config.general.screenshot_dir;
        if (!strlen(output_dir)) {
            output_dir = ".";
        }
        // FIXME: Check for existing path
        char *path = g_strdup_printf("%s/%s", output_dir, fname);
        FILE *fd = qemu_fopen(path, "wb");
        if (fd) {
            int s = fwrite(png.data(), png.size(), 1, fd);
            if (s != 1) {
                error_setg(&err, "Failed to write %s", path);
            }
            fclose(fd);
        } else {
            error_setg(&err, "Failed to open %s for writing", path);
        }
        g_free(path);
    } else {
        error_setg(&err, "Failed to encode PNG image");
    }

    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_report_err(err);
    } else {
        char *msg = g_strdup_printf("Screenshot Saved: %s", fname);
        xemu_queue_notification(msg);
        free(msg);
    }
}
