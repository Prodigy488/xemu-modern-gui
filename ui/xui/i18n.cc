//
// xemu User Interface - interface language
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "i18n.hh"
#include "common.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

#include "i18n-tables.inc"

static const std::vector<XuiLanguage> kLanguages = {
    { "",      "",                 "Windows language" },
    { "en",    "English",          "English" },
    { "es",    "Espa\xc3\xb1ol",   "Spanish" },
    { "pt-BR", "Portugu\xc3\xaas (Brasil)", "Portuguese (Brazil)" },
    { "fr",    "Fran\xc3\xa7" "ais", "French" },
    { "de",    "Deutsch",          "German" },
    { "it",    "Italiano",         "Italian" },
    { "pl",    "Polski",           "Polish" },
    { "ru",    "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9",
               "Russian" },
    { "ja",    "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", "Japanese" },
    { "zh-CN", "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87",
               "Chinese (Simplified)" },
    { "ko",    "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4", "Korean" },
};

const std::vector<XuiLanguage> &XuiLanguages() { return kLanguages; }

namespace {

struct LangState {
    std::unordered_map<std::string_view, const char *> table;
    // Results handed out, by the full text asked for. Never cleared, so
    // the pointers stay valid even after the language changes.
    std::unordered_map<std::string, std::string> cache, id_cache;
    std::vector<uint16_t> ranges;
    bool ranges_built = false;
};

std::mutex g_mutex;
std::map<std::string, LangState> g_states;
std::string g_code = "en";       // in use
std::string g_setting = "\x01";  // last setting seen (never a real value)
LangState *g_cur = nullptr;      // nullptr for English
bool g_test_override = false;

const XuiLangTable *FindTable(const std::string &code)
{
    for (const auto &t : kLangTables) {
        if (code == t.code) {
            return &t;
        }
    }
    return nullptr;
}

LangState *StateFor(const std::string &code)
{
    const XuiLangTable *t = FindTable(code);
    if (!t) {
        return nullptr;
    }
    auto it = g_states.find(code);
    if (it != g_states.end()) {
        return &it->second;
    }
    LangState &st = g_states[code];
    st.table.reserve(t->count * 2);
    for (int i = 0; i < t->count; i++) {
        st.table.emplace(t->pairs[i].en, t->pairs[i].tr);
    }
    return &st;
}

// Length in bytes of a leading icon (Font Awesome and the controller
// buttons use the private use area) plus the spaces after it
size_t IconPrefixLength(const char *s)
{
    size_t n = 0;
    for (;;) {
        const unsigned char *p = (const unsigned char *)s + n;
        if (p[0] == 0xEF && p[1] >= 0x80 && p[1] <= 0xA4 && p[2]) {
            n += 3; // U+F000..U+F8FF, and the U+F900 pad buttons
        } else if (p[0] == 0xEE && p[1] && p[2]) {
            n += 3; // U+E000..U+EFFF
        } else {
            break;
        }
        while (s[n] == ' ') {
            n++;
        }
    }
    return n;
}

bool IsEnglish() { return g_cur == nullptr; }

// Splits "<icon> Text##id" into prefix, core and suffix
void Split(const char *s, std::string_view *prefix, std::string_view *core,
           std::string_view *suffix)
{
    std::string_view all(s);
    size_t p = IconPrefixLength(s);
    size_t h = all.find("##", p);
    if (h == std::string_view::npos) {
        h = all.size();
    }
    while (p < h && (all[p] == ' ' || all[p] == '\n')) {
        p++; // leading and trailing spaces aren't translated
    }
    size_t e = h;
    while (e > p && (all[e - 1] == ' ' || all[e - 1] == '\n')) {
        e--;
    }
    *prefix = all.substr(0, p);
    *core = all.substr(p, e - p);
    *suffix = all.substr(e);
}

const char *Lookup(const char *s)
{
    std::string_view prefix, core, suffix;
    Split(s, &prefix, &core, &suffix);
    if (core.empty()) {
        return s;
    }
    auto it = g_cur->table.find(core);
    if (it == g_cur->table.end()) {
        return s;
    }
    if (prefix.empty() && suffix.empty()) {
        return it->second;
    }
    auto c = g_cur->cache.find(std::string(s));
    if (c != g_cur->cache.end()) {
        return c->second.c_str();
    }
    std::string out;
    out.reserve(prefix.size() + strlen(it->second) + suffix.size());
    out.append(prefix).append(it->second).append(suffix);
    return g_cur->cache.emplace(std::string(s), std::move(out))
        .first->second.c_str();
}

void Use(const std::string &code)
{
    g_code = code;
    g_cur = (code == "en") ? nullptr : StateFor(code);
    if (!g_cur) {
        g_code = "en";
    }
}

} // namespace

const char *tr(const char *english)
{
    if (!english || !*english) {
        return english;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (IsEnglish()) {
        return english;
    }
    return Lookup(english);
}

const char *trid(const char *english)
{
    if (!english) {
        return english;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    // English uses a cache of its own
    static std::unordered_map<std::string, std::string> en_cache;
    auto &cache = IsEnglish() ? en_cache : g_cur->id_cache;
    auto c = cache.find(english);
    if (c != cache.end()) {
        return c->second.c_str();
    }
    std::string_view prefix, core, suffix;
    Split(english, &prefix, &core, &suffix);
    std::string shown(english, std::string_view(english).find("##") ==
                                   std::string_view::npos
                               ? strlen(english)
                               : std::string_view(english).find("##"));
    if (!IsEnglish()) {
        shown = Lookup(shown.c_str());
    }
    std::string out = shown + "###" + english;
    return cache.emplace(english, std::move(out)).first->second.c_str();
}

const char *tr_items(const char *items)
{
    if (!items) {
        return items;
    }
    std::string all;
    for (const char *p = items; *p; p += strlen(p) + 1) {
        all.append(p).push_back('\0');
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (IsEnglish()) {
        return items;
    }
    std::string key = "\x01" + all;
    auto c = g_cur->id_cache.find(key);
    if (c != g_cur->id_cache.end()) {
        return c->second.c_str();
    }
    std::string out;
    for (const char *p = items; *p; p += strlen(p) + 1) {
        out.append(Lookup(p)).push_back('\0');
    }
    out.push_back('\0'); // c_str() adds the last one
    return g_cur->id_cache.emplace(key, std::move(out)).first->second.c_str();
}

const char *XuiSystemLanguage()
{
    static std::string code;
    if (!code.empty()) {
        return code.c_str();
    }
    code = "en";
#ifdef _WIN32
    LANGID id = GetUserDefaultUILanguage();
    int primary = PRIMARYLANGID(id), sub = SUBLANGID(id);
    switch (primary) {
    case LANG_SPANISH: code = "es"; break;
    case LANG_PORTUGUESE: code = "pt-BR"; break;
    case LANG_FRENCH: code = "fr"; break;
    case LANG_GERMAN: code = "de"; break;
    case LANG_ITALIAN: code = "it"; break;
    case LANG_POLISH: code = "pl"; break;
    case LANG_RUSSIAN: code = "ru"; break;
    case LANG_JAPANESE: code = "ja"; break;
    case LANG_KOREAN: code = "ko"; break;
    case LANG_CHINESE:
        // Simplified only (mainland China, Singapore)
        if (sub == SUBLANG_CHINESE_SIMPLIFIED ||
            sub == SUBLANG_CHINESE_SINGAPORE) {
            code = "zh-CN";
        }
        break;
    default: break;
    }
#else
    const char *env = getenv("LC_ALL");
    if (!env || !*env) env = getenv("LC_MESSAGES");
    if (!env || !*env) env = getenv("LANG");
    std::string l = env ? env : "";
    auto starts = [&](const char *p) { return l.rfind(p, 0) == 0; };
    if (starts("es")) code = "es";
    else if (starts("pt")) code = "pt-BR";
    else if (starts("fr")) code = "fr";
    else if (starts("de")) code = "de";
    else if (starts("it")) code = "it";
    else if (starts("pl")) code = "pl";
    else if (starts("ru")) code = "ru";
    else if (starts("ja")) code = "ja";
    else if (starts("ko")) code = "ko";
    else if (starts("zh_CN") || starts("zh_SG") || starts("zh-CN"))
        code = "zh-CN";
#endif
    return code.c_str();
}

const char *XuiLanguageCode()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_code.c_str();
}

bool XuiLanguageUpdate()
{
    if (g_test_override) {
        return false;
    }
    const char *setting = g_config.display.ui.language;
    if (!setting) {
        setting = "";
    }
    if (g_setting == setting) {
        return false;
    }
    g_setting = setting;
    std::string want = *setting ? setting : XuiSystemLanguage();
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string before = g_code;
    Use(want);
    return g_code != before;
}

void XuiLanguageSetForTest(const char *code)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_test_override = true;
    Use(code);
}

char XuiLanguageScript()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_code == "ja") return 'j';
    if (g_code == "zh-CN") return 'c';
    if (g_code == "ko") return 'k';
    return 0;
}

const uint16_t *XuiLanguageGlyphRanges()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    static const uint16_t none[] = { 0 };
    if (!g_cur) {
        return none;
    }
    if (g_cur->ranges_built) {
        return g_cur->ranges.data();
    }
    std::set<uint32_t> cps;
    if (g_code == "ja") {
        for (uint32_t c = 0x3040; c <= 0x30FF; c++) cps.insert(c); // kana
    }
    if (g_code == "ja" || g_code == "zh-CN" || g_code == "ko") {
        for (uint32_t c = 0x3000; c <= 0x303F; c++) cps.insert(c); // 、。「」
        for (uint32_t c = 0xFF01; c <= 0xFF5E; c++) cps.insert(c); // ！（）
    }
    for (const auto &kv : g_cur->table) {
        const unsigned char *p = (const unsigned char *)kv.second;
        while (*p) {
            uint32_t c;
            int n;
            if (*p < 0x80) { c = *p; n = 1; }
            else if ((*p & 0xE0) == 0xC0) { c = *p & 0x1F; n = 2; }
            else if ((*p & 0xF0) == 0xE0) { c = *p & 0x0F; n = 3; }
            else { c = *p & 0x07; n = 4; }
            int i = 1;
            for (; i < n && (p[i] & 0xC0) == 0x80; i++) {
                c = (c << 6) | (p[i] & 0x3F);
            }
            p += i;
            if ((c >= 0x2E80 && c < 0xE000) ||
                (c >= 0xFF00 && c <= 0xFFEF)) { // CJK and the like
                cps.insert(c);
            }
        }
    }
    auto &r = g_cur->ranges;
    for (auto it = cps.begin(); it != cps.end();) {
        uint32_t lo = *it, hi = lo;
        for (++it; it != cps.end() && *it == hi + 1; ++it) {
            hi = *it;
        }
        r.push_back((uint16_t)lo);
        r.push_back((uint16_t)hi);
    }
    r.push_back(0);
    g_cur->ranges_built = true;
    return r.data();
}

std::string XuiLanguageFontPath(int *font_no)
{
    struct Candidate { const char *file; int index; };
    char script = XuiLanguageScript();
    std::vector<Candidate> list;
    *font_no = 0;
#ifdef _WIN32
    if (script == 'j') list = { { "YuGothM.ttc", 0 }, { "meiryo.ttc", 0 },
                               { "msgothic.ttc", 0 } };
    if (script == 'c') list = { { "msyh.ttc", 0 }, { "msyh.ttf", 0 },
                               { "simhei.ttf", 0 }, { "simsun.ttc", 0 } };
    if (script == 'k') list = { { "malgun.ttf", 0 }, { "gulim.ttc", 0 } };
    const char *windir = getenv("WINDIR");
    std::string dir = std::string(windir && *windir ? windir : "C:\\Windows") +
                      "\\Fonts\\";
#else
    // Noto Sans CJK collections: 0 JP, 1 KR, 2 SC
    int noto = script == 'j' ? 0 : script == 'k' ? 1 : 2;
    std::string dir;
    if (script) {
        list = { { "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", noto },
                 { "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", noto },
                 { "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", noto },
                 { "/System/Library/Fonts/Hiragino Sans GB.ttc", 0 } };
    }
#endif
    for (const auto &c : list) {
        std::string path = dir + c.file;
        FILE *f = fopen(path.c_str(), "rb");
        if (f) {
            fclose(f);
            *font_no = c.index;
            return path;
        }
    }
    return "";
}

static const XuiLanguage *FindLanguage(const char *code)
{
    for (const auto &l : kLanguages) {
        if (!strcmp(l.code, code)) {
            return &l;
        }
    }
    return nullptr;
}

std::string XuiLanguageItemName(int i)
{
    if (i < 0 || i >= (int)kLanguages.size()) {
        return "";
    }
    const XuiLanguage &l = kLanguages[i];
    if (!*l.code) {
        const XuiLanguage *sys = FindLanguage(XuiSystemLanguage());
        return std::string(tr("Windows language")) + " (" +
               (sys ? sys->native : "English") + ")";
    }
    if (!strcmp(l.native, l.english)) {
        return l.native;
    }
    return std::string(l.native) + "  -  " + l.english;
}

int XuiLanguageSettingIndex()
{
    const char *setting = g_config.display.ui.language;
    if (!setting) {
        setting = "";
    }
    for (int i = 0; i < (int)kLanguages.size(); i++) {
        if (!strcmp(kLanguages[i].code, setting)) {
            return i;
        }
    }
    return 0;
}

void XuiLanguageChoose(int i)
{
    if (i < 0 || i >= (int)kLanguages.size()) {
        return;
    }
    xemu_settings_set_string(&g_config.display.ui.language, kLanguages[i].code);
}

const char *XuiLanguageLabel()
{
    static std::string label;
    label = tr("Language");
    if (strcmp(XuiLanguageCode(), "en") != 0 && label != "Language") {
        label += " / Language";
    }
    return label.c_str();
}

float XuiLanguageCoverage(const char *code)
{
    if (!strcmp(code, "en") || !*code) {
        return 1.0f;
    }
    const XuiLangTable *t = FindTable(code);
    if (!t || kTrTotalStrings <= 0) {
        return 0;
    }
    return std::min(1.0f, (float)t->count / (float)kTrTotalStrings);
}
