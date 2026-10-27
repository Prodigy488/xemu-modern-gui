//
// xemu User Interface - achievement triggers and the address finder's search
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "ach-memory.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using json = nlohmann::json;

namespace {

const size_t kMaxConds = 16;
const int kMaxHold = 100;
const size_t kPage = 4096;

// "0x1F", "1F" or a plain number; strings only
bool ParseHex(const std::string &s, uint32_t *out)
{
    size_t i = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 2 : 0;
    if (i >= s.size() || s.size() - i > 8) {
        return false;
    }
    uint32_t v = 0;
    for (; i < s.size(); i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) {
            return false;
        }
        v = v * 16 + (uint32_t)d;
    }
    *out = v;
    return true;
}

// A number written as a JSON number, or as text ("12", "-3.5", "0x10")
bool ParseNumber(const json &j, double *out)
{
    if (j.is_number()) {
        double v = j.get<double>();
        if (!std::isfinite(v)) {
            return false;
        }
        *out = v;
        return true;
    }
    if (j.is_string()) {
        std::string s = j.get<std::string>();
        if (s.empty() || s.size() > 24) {
            return false;
        }
        uint32_t h;
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && ParseHex(s, &h)) {
            *out = h;
            return true;
        }
        char *end = nullptr;
        double v = strtod(s.c_str(), &end);
        if (end && *end == 0 && std::isfinite(v)) {
            *out = v;
            return true;
        }
    }
    return false;
}

std::string Hex8(uint32_t v)
{
    char b[16];
    snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}

bool IsFloat(AchSize s) { return s == kAchF32; }

// Raw value of `size` bytes at p
uint32_t RawAt(AchSize s, const uint8_t *p)
{
    switch (AchSizeBytes(s)) {
    case 1: return p[0];
    case 2: return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    default: return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                    ((uint32_t)p[3] << 24);
    }
}

bool Compare(AchOp op, double a, double b, bool is_float)
{
    if (is_float && (op == kAchEq || op == kAchNe)) {
        bool same = std::fabs(a - b) <= 0.001;
        return op == kAchEq ? same : !same;
    }
    switch (op) {
    case kAchEq: return a == b;
    case kAchNe: return a != b;
    case kAchLt: return a < b;
    case kAchLe: return a <= b;
    case kAchGt: return a > b;
    case kAchGe: return a >= b;
    }
    return false;
}

} // namespace

uint32_t AchSizeBytes(AchSize s)
{
    switch (s) {
    case kAchU8: case kAchI8: return 1;
    case kAchU16: case kAchI16: return 2;
    default: return 4;
    }
}

static const char *kSizeNames[] = { "u8", "u16", "u32", "i8", "i16", "i32", "f32" };
static const char *kOpNames[] = { "==", "!=", "<", "<=", ">", ">=" };

const char *AchSizeName(AchSize s) { return kSizeNames[s]; }
const char *AchOpName(AchOp o) { return kOpNames[o]; }

bool AchSizeFromName(const std::string &n, AchSize *out)
{
    for (int i = 0; i < 7; i++) {
        if (n == kSizeNames[i]) {
            *out = (AchSize)i;
            return true;
        }
    }
    return false;
}

bool AchOpFromName(const std::string &n, AchOp *out)
{
    for (int i = 0; i < 6; i++) {
        if (n == kOpNames[i]) {
            *out = (AchOp)i;
            return true;
        }
    }
    return false;
}

double AchRawToValue(AchSize s, uint32_t raw)
{
    switch (s) {
    case kAchI8: return (double)(int8_t)raw;
    case kAchI16: return (double)(int16_t)raw;
    case kAchI32: return (double)(int32_t)raw;
    case kAchF32: {
        float f;
        memcpy(&f, &raw, 4);
        return (double)f;
    }
    default: return (double)raw;
    }
}

uint32_t AchValueToRaw(AchSize s, double v)
{
    if (s == kAchF32) {
        float f = (float)v;
        uint32_t r;
        memcpy(&r, &f, 4);
        return r;
    }
    long long n = (long long)std::llround(std::max(-2147483648.0, std::min(4294967295.0, v)));
    return (uint32_t)n;
}

bool AchCondParse(const json &j, AchCond &c)
{
    if (!j.is_object()) {
        return false;
    }
    AchCond r;
    // address
    if (!j.contains("addr")) {
        return false;
    }
    if (j["addr"].is_string()) {
        if (!ParseHex(j["addr"].get<std::string>(), &r.addr)) {
            return false;
        }
    } else if (j["addr"].is_number_unsigned()) {
        uint64_t a = j["addr"].get<uint64_t>();
        if (a > 0xFFFFFFFFull) {
            return false;
        }
        r.addr = (uint32_t)a;
    } else {
        return false;
    }
    if (r.addr < 0x1000 || r.addr > 0xFFFFFFF0u) {
        return false;
    }
    // size
    if (j.contains("size")) {
        if (!j["size"].is_string() || !AchSizeFromName(j["size"].get<std::string>(), &r.size)) {
            return false;
        }
    }
    if (r.addr + AchSizeBytes(r.size) < r.addr) {
        return false;
    }
    // mask (flags)
    if (j.contains("mask")) {
        if (j["mask"].is_string()) {
            if (!ParseHex(j["mask"].get<std::string>(), &r.mask)) {
                return false;
            }
        } else if (j["mask"].is_number_unsigned() && j["mask"].get<uint64_t>() <= 0xFFFFFFFFull) {
            r.mask = (uint32_t)j["mask"].get<uint64_t>();
        } else {
            return false;
        }
        if (IsFloat(r.size) || r.mask == 0) {
            return false;
        }
    }
    // comparison
    if (j.contains("op")) {
        if (!j["op"].is_string() || !AchOpFromName(j["op"].get<std::string>(), &r.op)) {
            return false;
        }
    }
    if (!j.contains("value")) {
        return false;
    }
    if (j["value"].is_string() && j["value"].get<std::string>() == "prev") {
        r.vs_prev = true;
    } else if (!ParseNumber(j["value"], &r.value)) {
        return false;
    }
    c = r;
    return true;
}

json AchCondToJson(const AchCond &c)
{
    json j = json::object();
    j["addr"] = Hex8(c.addr);
    j["size"] = AchSizeName(c.size);
    if (c.mask != 0xFFFFFFFFu) {
        j["mask"] = Hex8(c.mask);
    }
    j["op"] = AchOpName(c.op);
    if (c.vs_prev) {
        j["value"] = "prev";
    } else if (IsFloat(c.size) || c.value != std::floor(c.value)) {
        j["value"] = c.value;
    } else {
        j["value"] = (long long)c.value;
    }
    return j;
}

bool AchTriggerParse(const json &j, AchTrigger &out)
{
    AchTrigger t;
    const json *all = nullptr, *any = nullptr;
    if (j.is_array()) {
        all = &j;
    } else if (j.is_object()) {
        all = j.contains("all") ? &j["all"] : nullptr;
        any = j.contains("any") ? &j["any"] : nullptr;
        if (j.contains("hold")) {
            if (!j["hold"].is_number_integer()) {
                return false;
            }
            long long h = j["hold"].get<long long>();
            t.hold = (int)std::max(1ll, std::min((long long)kMaxHold, h));
        }
        if (j.contains("builds")) {
            if (!j["builds"].is_array()) {
                return false;
            }
            for (auto &b : j["builds"]) {
                uint32_t v;
                if (b.is_string() && ParseHex(b.get<std::string>(), &v)) {
                    if (t.builds.size() < 16) {
                        t.builds.push_back(v);
                    }
                }
            }
        }
    } else {
        return false;
    }
    auto read = [&](const json *list, std::vector<AchCond> &dst) {
        if (!list) {
            return true;
        }
        if (!list->is_array() || list->size() > kMaxConds) {
            return false;
        }
        for (auto &e : *list) {
            AchCond c;
            if (!AchCondParse(e, c)) {
                return false;
            }
            dst.push_back(c);
        }
        return true;
    };
    if (!read(all, t.all) || !read(any, t.any)) {
        return false;
    }
    if (t.all.empty() && t.any.empty()) {
        return false;
    }
    out = t;
    return true;
}

json AchTriggerToJson(const AchTrigger &t)
{
    json j = json::object();
    if (!t.all.empty()) {
        j["all"] = json::array();
        for (auto &c : t.all) {
            j["all"].push_back(AchCondToJson(c));
        }
    }
    if (!t.any.empty()) {
        j["any"] = json::array();
        for (auto &c : t.any) {
            j["any"].push_back(AchCondToJson(c));
        }
    }
    j["hold"] = t.hold;
    if (!t.builds.empty()) {
        j["builds"] = json::array();
        for (uint32_t b : t.builds) {
            j["builds"].push_back(Hex8(b));
        }
    }
    return j;
}

std::string AchCondText(const AchCond &c)
{
    static const char *kWhat[] = { "1 byte", "2 bytes", "4 bytes", "1 byte", "2 bytes",
                                   "4 bytes", "decimal number" };
    static const char *kIs[] = { "is", "is not", "is below", "is at most", "is above",
                                 "is at least" };
    char b[160];
    std::string val;
    if (c.vs_prev) {
        val = "the last value";
    } else {
        char v[40];
        if (IsFloat(c.size) || c.value != std::floor(c.value)) {
            snprintf(v, sizeof(v), "%g", c.value);
        } else {
            snprintf(v, sizeof(v), "%lld", (long long)c.value);
        }
        val = v;
    }
    snprintf(b, sizeof(b), "%s at %s %s %s", kWhat[c.size], Hex8(c.addr).c_str(), kIs[c.op],
             val.c_str());
    return b;
}

bool AchReadCond(const AchCond &c, AchReadFn fn, void *user, double *out)
{
    uint8_t buf[4] = { 0, 0, 0, 0 };
    if (!fn || !fn(c.addr, buf, AchSizeBytes(c.size), user)) {
        return false;
    }
    uint32_t raw = RawAt(c.size, buf);
    if (!IsFloat(c.size) && c.mask != 0xFFFFFFFFu) {
        // Looking at some bits of a flag word
        *out = (double)(raw & c.mask);
        return true;
    }
    *out = AchRawToValue(c.size, raw);
    return true;
}

void AchTriggerReset(AchTrigger &t)
{
    t.streak = 0;
    for (auto *v : { &t.all, &t.any }) {
        for (auto &c : *v) {
            c.have_prev = false;
            c.latched = false;
            c.prev = 0;
        }
    }
}

namespace {
// One condition's answer for this check
bool CondHolds(AchCond &c, AchReadFn fn, void *user)
{
    double cur;
    if (!AchReadCond(c, fn, user, &cur)) {
        return c.vs_prev ? c.latched : false;
    }
    bool ok;
    if (c.vs_prev) {
        // changed / grew / shrank since the last check; once seen it stays
        // true for the rest of the game
        if (c.have_prev && Compare(c.op, cur, c.prev, IsFloat(c.size))) {
            c.latched = true;
        }
        c.prev = cur;
        c.have_prev = true;
        ok = c.latched;
    } else {
        ok = Compare(c.op, cur, c.value, IsFloat(c.size));
    }
    return ok;
}
} // namespace

bool AchTriggerTick(AchTrigger &t, AchReadFn fn, void *user)
{
    bool ok = true;
    for (auto &c : t.all) {
        ok &= CondHolds(c, fn, user); // no early exit: "previous" must keep up
    }
    if (!t.any.empty()) {
        bool one = false;
        for (auto &c : t.any) {
            one |= CondHolds(c, fn, user);
        }
        ok &= one;
    }
    t.streak = ok ? std::min(t.streak + 1, 1000000) : 0;
    return t.streak >= t.hold;
}

// ---------------------------------------------------------------------------
// Searching
// ---------------------------------------------------------------------------

void AchSearch::Reset()
{
    m_active = false;
    m_snap_mode = false;
    m_snap_pages.clear();
    m_snap.clear();
    m_snap.shrink_to_fit();
    m_hits.clear();
    m_hits.shrink_to_fit();
}

namespace {
bool Fits(AchScanKind kind, AchSize s, uint32_t prev_raw, uint32_t cur_raw, double value)
{
    double a = AchRawToValue(s, prev_raw), b = AchRawToValue(s, cur_raw);
    if (IsFloat(s) && (std::isnan(a) || std::isnan(b))) {
        return false;
    }
    switch (kind) {
    case kScanEqual: return Compare(kAchEq, b, value, IsFloat(s));
    case kScanChanged: return prev_raw != cur_raw;
    case kScanUnchanged: return prev_raw == cur_raw;
    case kScanIncreased: return b > a;
    case kScanDecreased: return b < a;
    }
    return false;
}
} // namespace

long AchSearch::First(AchSize size, bool unknown, double value,
                      const std::vector<uint32_t> &pages, AchReadFn fn, void *user)
{
    Reset();
    m_size = size;
    const uint32_t step = AchSizeBytes(size);
    std::vector<uint8_t> buf(kPage);
    size_t readable = 0;
    bool overflow = false;
    if (unknown) {
        m_snap.reserve(pages.size() * kPage);
    }
    const uint32_t want = AchValueToRaw(size, value);
    for (uint32_t base : pages) {
        if (!fn(base, buf.data(), kPage, user)) {
            continue;
        }
        readable++;
        if (unknown) {
            m_snap_pages.push_back(base);
            m_snap.insert(m_snap.end(), buf.begin(), buf.end());
            continue;
        }
        for (uint32_t o = 0; o + step <= kPage; o += step) {
            uint32_t raw = RawAt(size, &buf[o]);
            bool hit = IsFloat(size) ? Compare(kAchEq, AchRawToValue(size, raw), value, true)
                                     : raw == want;
            if (hit) {
                if (m_hits.size() >= kMaxHits) {
                    overflow = true;
                    break;
                }
                m_hits.push_back({ base + o, raw });
            }
        }
        if (overflow) {
            break;
        }
    }
    if (readable == 0) {
        Reset();
        return -2;
    }
    if (overflow) {
        Reset();
        return -1;
    }
    m_active = true;
    m_snap_mode = unknown;
    return unknown ? (long)(readable * (kPage / step)) : (long)m_hits.size();
}

long AchSearch::Next(AchScanKind kind, double value, AchReadFn fn, void *user)
{
    if (!m_active) {
        return -2;
    }
    const AchSize size = m_size;
    const uint32_t step = AchSizeBytes(size);
    std::vector<uint8_t> buf(kPage);

    if (m_snap_mode) {
        // Compare every remembered address with the memory now
        std::vector<AchHit> found;
        bool overflow = false;
        size_t readable = 0;
        for (size_t p = 0; p < m_snap_pages.size(); p++) {
            uint8_t *old = &m_snap[p * kPage];
            if (!fn(m_snap_pages[p], buf.data(), kPage, user)) {
                continue;
            }
            readable++;
            if (!overflow) {
                for (uint32_t o = 0; o + step <= kPage; o += step) {
                    uint32_t cur = RawAt(size, &buf[o]);
                    if (Fits(kind, size, RawAt(size, &old[o]), cur, value)) {
                        if (found.size() >= kMaxHits) {
                            overflow = true;
                            break;
                        }
                        found.push_back({ m_snap_pages[p] + o, cur });
                    }
                }
            }
            memcpy(old, buf.data(), kPage); // the next compare starts from now
        }
        if (readable == 0) {
            return -2;
        }
        if (overflow) {
            return -1; // still remembering, from this moment
        }
        m_hits.swap(found);
        m_snap_mode = false;
        m_snap_pages.clear();
        m_snap.clear();
        m_snap.shrink_to_fit();
        return (long)m_hits.size();
    }

    // A list of addresses: look at each one again, a page at a time
    std::vector<AchHit> keep;
    keep.reserve(m_hits.size());
    uint32_t page = 0xFFFFFFFFu;
    bool have = false;
    for (const AchHit &h : m_hits) {
        uint32_t base = h.addr & ~(uint32_t)(kPage - 1);
        if (!have || base != page) {
            page = base;
            have = fn(base, buf.data(), kPage, user);
        }
        if (!have) {
            continue;
        }
        uint32_t cur = RawAt(size, &buf[h.addr - base]);
        if (Fits(kind, size, h.last, cur, value)) {
            keep.push_back({ h.addr, cur });
        }
    }
    m_hits.swap(keep);
    return (long)m_hits.size();
}
