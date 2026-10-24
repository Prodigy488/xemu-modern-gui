//
// xemu User Interface - interface language
//
// The interface is written in English. tr("English text") returns the text
// in the chosen language, or the English text when there's no translation.
// Translations live in ui/xui/lang/<code>.json and are compiled in by
// scripts/xui-i18n.py (see ui/xui/lang/README.md).
//
// tr() leaves a leading icon and an ImGui "##id" suffix alone and translates
// what's between, so tr(ICON_FA_PLAY " Play##card") works. Use trid() for
// window and popup titles, so the ImGui ID doesn't change with the language:
// both OpenPopup() and BeginPopupModal() must then use trid().
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#if defined(__GNUC__)
#define XUI_FORMAT_ARG __attribute__((format_arg(1)))
#else
#define XUI_FORMAT_ARG
#endif

// Translated text. The pointer stays valid for the life of the program.
const char *tr(const char *english) XUI_FORMAT_ARG;
// Translated title + "###" + the English text, for a stable ImGui ID
const char *trid(const char *english);
// Combo items, "One\0Two\0" (ends with \0\0), each translated
const char *tr_items(const char *zero_separated);
// Marks a string for translation where it's defined (arrays of names...),
// for tr() to translate where it's shown
#define N_(s) s

struct XuiLanguage {
    const char *code;    // "" (follow Windows), "en", "de", "pt-BR"...
    const char *native;  // "Deutsch"
    const char *english; // "German"
};
const std::vector<XuiLanguage> &XuiLanguages();

// The language in use: the setting, or Windows' language when it's "",
// or "en"
const char *XuiLanguageCode();
// Applies g_config.display.ui.language; call once a frame. Returns true
// when the language changed (the fonts then need rebuilding).
bool XuiLanguageUpdate();
// Language picked by Windows (or LANG elsewhere), from the list, or "en"
const char *XuiSystemLanguage();

// Script that needs a font beyond Roboto: 0 none, 'j' Japanese,
// 'c' Chinese (Simplified), 'k' Korean
char XuiLanguageScript();
// Glyph ranges (ImGui format: pairs, 0-terminated) the language needs on
// top of Latin: every character its translations use, plus kana for
// Japanese.
const uint16_t *XuiLanguageGlyphRanges();
// A system font with the script's characters ("" if none was found), and
// the font's index in a .ttc collection
std::string XuiLanguageFontPath(int *font_no);

// For language pickers: the name shown for XuiLanguages()[i] ("Deutsch -
// German"; the first is "Windows language (Deutsch)"), the index of the
// setting, and choosing one (saved to the setting)
std::string XuiLanguageItemName(int i);
int XuiLanguageSettingIndex();
void XuiLanguageChoose(int i);
// "Language", plus " / Language" when not in English, so it can be found
const char *XuiLanguageLabel();

// How much of the interface the language covers, 0..1 (for the picker)
float XuiLanguageCoverage(const char *code);

// For tests: use translations from a table instead of the built-in ones
struct XuiTrPair { const char *en, *tr; };
void XuiLanguageSetForTest(const char *code);
