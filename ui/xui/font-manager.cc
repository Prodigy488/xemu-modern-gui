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
#include "font-manager.hh"
#include "viewport-manager.hh"
#include "i18n.hh"

#include <cstdio>
#include <string>
#include <vector>

#include "data/Roboto-Medium.ttf.h"
#include "data/RobotoCondensed-Regular.ttf.h"
#include "data/RussoOne-Regular.ttf.h"
#include "data/font_awesome_6_1_1_solid.min.otf.h"
#include "data/abxy.ttf.h"

FontManager g_font_mgr;

FontManager::FontManager()
{
    m_dash_font = nullptr;
    m_dash_font_small = nullptr;
    m_last_viewport_scale = 1;
    m_last_pixel_density = 1;
}

// Latin with accents (Western and Central Europe), Cyrillic, and
// punctuation like dashes, quotes and the ellipsis. Roboto has these.
static const ImWchar kTextRanges[] = {
    0x0020, 0x00FF, 0x0100, 0x017F, 0x0400, 0x045F, 0x2010, 0x2027,
    0x2030, 0x203A, 0x20AC, 0x20AC, 0x2122, 0x2122, 0x2190, 0x2193, 0,
};

// Japanese, Chinese and Korean come from a font Windows already has, with
// only the characters the translation uses (plus kana), to keep the font
// texture small.
static std::vector<unsigned char> s_cjk_data;
static std::string s_cjk_path;

static bool LoadCjkFont(int *font_no)
{
    std::string path = XuiLanguageFontPath(font_no);
    if (path.empty()) {
        return false;
    }
    if (path == s_cjk_path && !s_cjk_data.empty()) {
        return true;
    }
    s_cjk_data.clear();
    s_cjk_path = path;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    unsigned char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        s_cjk_data.insert(s_cjk_data.end(), buf, buf + n);
    }
    fclose(f);
    return !s_cjk_data.empty();
}

void FontManager::Rebuild()
{
    ImGuiIO &io = ImGui::GetIO();

    io.Fonts->Clear();
    XuiLanguageUpdate();
    m_language = XuiLanguageCode();

    float scale = g_viewport_mgr.m_scale;
    float pixel_density = g_viewport_mgr.m_pixel_density;

    int cjk_font_no = 0;
    bool cjk = XuiLanguageScript() && LoadCjkFont(&cjk_font_no);
    const ImWchar *cjk_ranges = (const ImWchar *)XuiLanguageGlyphRanges();
    auto merge_cjk = [&](float size) {
        if (!cjk || !cjk_ranges[0]) {
            return;
        }
        ImFontConfig c;
        c.FontDataOwnedByAtlas = false;
        c.RasterizerDensity = pixel_density;
        c.MergeMode = true;
        c.FontNo = cjk_font_no;
        c.OversampleH = 1; // plenty for these, and half the texture
        c.OversampleV = 1;
        io.Fonts->AddFontFromMemoryTTF(s_cjk_data.data(),
                                       (int)s_cjk_data.size(), size * scale,
                                       &c, cjk_ranges);
    };

    {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.RasterizerDensity = pixel_density;
        m_default_font = io.Fonts->AddFontFromMemoryTTF(
            (void *)Roboto_Medium_data, Roboto_Medium_size,
            16.0f * scale, &config, kTextRanges);
        {
            // Icons in the regular UI font too (game library toolbar etc.)
            ImFontConfig icon_config;
            icon_config.FontDataOwnedByAtlas = false;
            icon_config.RasterizerDensity = pixel_density;
            icon_config.MergeMode = true;
            icon_config.PixelSnapH = true;
            icon_config.GlyphMinAdvanceX = 16.0f * scale;
            icon_config.GlyphOffset = ImVec2(0, 1 * scale);
            static const ImWchar fa_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
            io.Fonts->AddFontFromMemoryTTF(
                (void *)font_awesome_6_1_1_solid_min_data,
                font_awesome_6_1_1_solid_min_size, 14.0f * scale,
                &icon_config, fa_ranges);
        }
        merge_cjk(16.0f);
        m_menu_font_small = io.Fonts->AddFontFromMemoryTTF(
            (void *)RobotoCondensed_Regular_data, RobotoCondensed_Regular_size,
            22.0f * scale, &config, kTextRanges);
        merge_cjk(22.0f);
    }
    {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.RasterizerDensity = pixel_density;
        config.MergeMode = true;
        config.GlyphOffset =
            ImVec2(0, 13 * scale);
        config.GlyphMaxAdvanceX = 24.0f * scale;
        static const ImWchar icon_ranges[] = { 0xf900, 0xf903, 0 };
        io.Fonts->AddFontFromMemoryTTF((void *)abxy_data, abxy_size,
                                       40.0f * scale,
                                       &config, icon_ranges);
    }
    {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.RasterizerDensity = pixel_density;
        m_menu_font_medium = io.Fonts->AddFontFromMemoryTTF(
            (void *)RobotoCondensed_Regular_data, RobotoCondensed_Regular_size,
            26.0f * scale, &config, kTextRanges);
        merge_cjk(26.0f);
        m_menu_font = io.Fonts->AddFontFromMemoryTTF(
            (void *)RobotoCondensed_Regular_data, RobotoCondensed_Regular_size,
            34.0f * scale, &config, kTextRanges);
        merge_cjk(34.0f);
    }
    {
        // Russo One (SIL OFL): squared, blocky letters in the spirit of the
        // original Xbox's menu. Western European text only, so other
        // languages keep the regular fonts there.
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.RasterizerDensity = pixel_density;
        static const ImWchar dash_ranges[] = { 0x0020, 0x00FF, 0x2010, 0x2027, 0x2030,
                                               0x203A, 0x20AC, 0x20AC, 0x2122, 0x2122, 0 };
        m_dash_font = io.Fonts->AddFontFromMemoryTTF(
            (void *)RussoOne_Regular_data, RussoOne_Regular_size, 28.0f * scale, &config,
            dash_ranges);
        m_dash_font_small = io.Fonts->AddFontFromMemoryTTF(
            (void *)RussoOne_Regular_data, RussoOne_Regular_size, 19.0f * scale, &config,
            dash_ranges);
    }
    {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.RasterizerDensity = pixel_density;
        config.MergeMode = true;
        config.GlyphOffset =
            ImVec2(0, -3 * scale);
        config.GlyphMinAdvanceX = 32.0f * scale;
        static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
        io.Fonts->AddFontFromMemoryTTF((void *)font_awesome_6_1_1_solid_min_data,
                                       font_awesome_6_1_1_solid_min_size,
                                       18.0f * scale,
                                       &config, icon_ranges);
    }
    {
        ImFontConfig config = ImFontConfig();
        config.OversampleH = config.OversampleV = 1;
        config.PixelSnapH = true;
        config.RasterizerDensity = pixel_density;
        config.SizePixels = 13.0f * scale;
        m_fixed_width_font = io.Fonts->AddFontDefault(&config);
    }

    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
}

void FontManager::Update()
{
    XuiLanguageUpdate();
    if (g_viewport_mgr.m_scale != m_last_viewport_scale ||
        g_viewport_mgr.m_pixel_density != m_last_pixel_density ||
        m_language != XuiLanguageCode()) {
        Rebuild();
        m_last_viewport_scale = g_viewport_mgr.m_scale;
        m_last_pixel_density = g_viewport_mgr.m_pixel_density;
    }
}
