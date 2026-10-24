//
// xemu User Interface - Game patches
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "game-patches.hh"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace {

bool ReadFile(const std::string &path, std::vector<uint8_t> &out,
              size_t max_size = 256u * 1024 * 1024)
{
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) {
        return false;
    }
    f.seekg(0, std::ios::end);
    std::streamoff len = f.tellg();
    if (len < 0 || (uint64_t)len > max_size) {
        return false;
    }
    out.resize((size_t)len);
    f.seekg(0);
    f.read((char *)out.data(), len);
    return (bool)f || len == 0;
}

bool ReadAt(std::ifstream &f, uint64_t size, uint64_t off, uint8_t *dst,
            size_t len)
{
    memset(dst, 0, len);
    if (off >= size) {
        return true;
    }
    size_t n = (size_t)std::min<uint64_t>(len, size - off);
    f.clear();
    f.seekg((std::streamoff)off);
    f.read((char *)dst, (std::streamsize)n);
    return (size_t)f.gcount() == n;
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return s;
}

void PutBe16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xFF;
}

void PutBe32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (v >> (24 - 8 * i)) & 0xFF;
    }
}

void PutBe64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (v >> (56 - 8 * i)) & 0xFF;
    }
}

uint32_t GetLe32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

} // namespace

uint32_t Crc32(const uint8_t *data, size_t len)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

bool ApplyIpsPatch(std::vector<uint8_t> &data, const std::vector<uint8_t> &ips,
                   std::string &error)
{
    if (ips.size() < 8 || memcmp(ips.data(), "PATCH", 5) != 0) {
        error = "not an IPS patch";
        return false;
    }
    size_t p = 5;
    auto need = [&](size_t n) { return p + n <= ips.size(); };
    const size_t max_size = 64u * 1024 * 1024;
    for (;;) {
        if (!need(3)) {
            error = "truncated IPS patch";
            return false;
        }
        if (memcmp(&ips[p], "EOF", 3) == 0) {
            p += 3;
            if (need(3)) { // optional truncation extension
                size_t trunc = ((size_t)ips[p] << 16) | (ips[p + 1] << 8) |
                               ips[p + 2];
                if (trunc < data.size()) {
                    data.resize(trunc);
                }
            }
            return true;
        }
        size_t off = ((size_t)ips[p] << 16) | (ips[p + 1] << 8) | ips[p + 2];
        p += 3;
        if (!need(2)) {
            error = "truncated IPS patch";
            return false;
        }
        size_t len = (ips[p] << 8) | ips[p + 1];
        p += 2;
        if (len == 0) { // RLE record
            if (!need(3)) {
                error = "truncated IPS patch";
                return false;
            }
            size_t rle = (ips[p] << 8) | ips[p + 1];
            uint8_t value = ips[p + 2];
            p += 3;
            if (off + rle > max_size) {
                error = "IPS patch writes past 64 MB";
                return false;
            }
            if (off + rle > data.size()) {
                data.resize(off + rle, 0);
            }
            memset(&data[off], value, rle);
        } else {
            if (!need(len)) {
                error = "truncated IPS patch";
                return false;
            }
            if (off + len > max_size) {
                error = "IPS patch writes past 64 MB";
                return false;
            }
            if (off + len > data.size()) {
                data.resize(off + len, 0);
            }
            memcpy(&data[off], &ips[p], len);
            p += len;
        }
    }
}

bool ApplyBpsPatch(const std::vector<uint8_t> &source,
                   const std::vector<uint8_t> &bps,
                   std::vector<uint8_t> &target, std::string &error)
{
    if (bps.size() < 4 + 12 || memcmp(bps.data(), "BPS1", 4) != 0) {
        error = "not a BPS patch";
        return false;
    }
    const size_t end = bps.size() - 12;
    if (Crc32(bps.data(), bps.size() - 4) != GetLe32(&bps[bps.size() - 4])) {
        error = "BPS patch file is corrupt (checksum mismatch)";
        return false;
    }
    if (Crc32(source.data(), source.size()) != GetLe32(&bps[end])) {
        error = "this BPS patch was made for a different version of the game";
        return false;
    }
    size_t p = 4;
    bool bad = false;
    auto varint = [&]() -> uint64_t {
        uint64_t data = 0, shift = 1;
        for (int i = 0; i < 10; i++) {
            if (p >= end) {
                bad = true;
                return 0;
            }
            uint8_t x = bps[p++];
            data += (x & 0x7F) * shift;
            if (x & 0x80) {
                return data;
            }
            shift <<= 7;
            data += shift;
        }
        bad = true;
        return 0;
    };
    uint64_t source_size = varint();
    uint64_t target_size = varint();
    uint64_t meta_size = varint();
    if (bad || source_size != source.size() || target_size > 64u * 1024 * 1024 ||
        meta_size > end - p) {
        error = "invalid BPS patch header";
        return false;
    }
    p += (size_t)meta_size;

    target.assign((size_t)target_size, 0);
    size_t out = 0;
    int64_t source_rel = 0, target_rel = 0;
    while (p < end) {
        uint64_t data = varint();
        if (bad) {
            break;
        }
        uint64_t len = (data >> 2) + 1;
        if (out + len > target.size()) {
            error = "BPS patch writes past the target size";
            return false;
        }
        switch (data & 3) {
        case 0: // SourceRead
            if (out + len > source.size()) {
                error = "invalid BPS patch (source read)";
                return false;
            }
            memcpy(&target[out], &source[out], (size_t)len);
            out += (size_t)len;
            break;
        case 1: // TargetRead
            if (p + len > end) {
                error = "truncated BPS patch";
                return false;
            }
            memcpy(&target[out], &bps[p], (size_t)len);
            p += (size_t)len;
            out += (size_t)len;
            break;
        case 2: { // SourceCopy
            uint64_t d = varint();
            source_rel += (d & 1 ? -1 : 1) * (int64_t)(d >> 1);
            if (bad || source_rel < 0 ||
                (uint64_t)source_rel + len > source.size()) {
                error = "invalid BPS patch (source copy)";
                return false;
            }
            memcpy(&target[out], &source[(size_t)source_rel], (size_t)len);
            source_rel += len;
            out += (size_t)len;
            break;
        }
        case 3: { // TargetCopy (may overlap; copy byte by byte)
            uint64_t d = varint();
            target_rel += (d & 1 ? -1 : 1) * (int64_t)(d >> 1);
            if (bad || target_rel < 0 || (uint64_t)target_rel >= out) {
                error = "invalid BPS patch (target copy)";
                return false;
            }
            for (uint64_t i = 0; i < len; i++) {
                target[out++] = target[(size_t)target_rel++];
            }
            break;
        }
        }
    }
    if (bad || out != target.size()) {
        error = "truncated BPS patch";
        return false;
    }
    if (Crc32(target.data(), target.size()) != GetLe32(&bps[end + 4])) {
        error = "BPS patch produced the wrong result (target checksum mismatch)";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Jay's Magic Patch (.jmp)
// ---------------------------------------------------------------------------

namespace {

std::string Trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) {
        return "";
    }
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

int HexVal(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

// "AABB CC" -> bytes. False on odd length or non-hex characters.
bool ParseHexBytes(const std::string &s, std::vector<uint8_t> &out)
{
    out.clear();
    int hi = -1;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            continue;
        }
        int v = HexVal(c);
        if (v < 0) {
            return false;
        }
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back((uint8_t)(hi << 4 | v));
            hi = -1;
        }
    }
    return hi < 0 && !out.empty();
}

bool ParseHexNumber(std::string s, uint64_t &v)
{
    s = Trim(s);
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s = s.substr(2);
    }
    if (s.empty() || s.size() > 16) {
        return false;
    }
    v = 0;
    for (char c : s) {
        int d = HexVal(c);
        if (d < 0) {
            return false;
        }
        v = v << 4 | (uint64_t)d;
    }
    return true;
}

bool IsHex8(const std::string &s)
{
    if (s.size() != 8) {
        return false;
    }
    for (char c : s) {
        if (HexVal(c) < 0) {
            return false;
        }
    }
    return true;
}

// "FinalF.xbe - 16:9" -> "finalf.xbe"; "" if the comment names no XBE
std::string CommentXbe(const std::string &comment)
{
    std::string l = Lower(Trim(comment));
    size_t start = l.find_first_not_of("\"'");
    if (start == std::string::npos) {
        return "";
    }
    size_t end = l.find_first_of(" \t\"'", start);
    std::string token = l.substr(start, end == std::string::npos
                                             ? std::string::npos
                                             : end - start);
    if (token.size() <= 4 ||
        token.compare(token.size() - 4, 4, ".xbe") != 0) {
        return "";
    }
    std::string rest =
        end == std::string::npos ? "" : Trim(l.substr(end));
    while (!rest.empty() && (rest[0] == '"' || rest[0] == '\'')) {
        rest = Trim(rest.substr(1));
    }
    return (!rest.empty() && rest[0] == '-') ? token : "";
}

} // namespace

bool ParseJmp(const std::string &text, JmpPatch &out, std::string &error)
{
    out = JmpPatch();
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : text) {
            if (c == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else if (c != '\r') {
                cur += c;
            }
        }
        lines.push_back(cur);
    }
    if (!lines.empty() && lines[0].size() >= 3 &&
        (uint8_t)lines[0][0] == 0xEF && (uint8_t)lines[0][1] == 0xBB &&
        (uint8_t)lines[0][2] == 0xBF) {
        lines[0] = lines[0].substr(3); // UTF-8 BOM
    }

    size_t i = 0;
    // File header: "#Jay's Magic Patcher ..." and key=value lines
    bool saw_header = false;
    for (; i < lines.size(); i++) {
        std::string t = Trim(lines[i]);
        if (t.empty()) {
            continue;
        }
        if (!saw_header && t[0] == '#' &&
            Lower(t).find("magic patcher") != std::string::npos) {
            saw_header = true;
            // '#"Loader.xbe" - Jay's Magic Patcher': the file is for that XBE
            size_t q1 = t.find('"');
            size_t q2 = q1 == std::string::npos ? q1 : t.find('"', q1 + 1);
            if (q2 != std::string::npos) {
                std::string target = Lower(t.substr(q1 + 1, q2 - q1 - 1));
                if (target.size() > 4 &&
                    target.compare(target.size() - 4, 4, ".xbe") == 0 &&
                    target != "default.xbe") {
                    out.other_target = true;
                }
            }
            continue;
        }
        size_t eq = t.find('=');
        if (t[0] == '#' || eq == std::string::npos) {
            break;
        }
        std::string key = Lower(Trim(t.substr(0, eq)));
        std::string val = Trim(t.substr(eq + 1));
        if (key == "game-title") {
            out.game_title = val;
        } else if (key == "region") {
            out.region = val;
        } else if (key == "author") {
            out.author = val;
        } else if (key == "notes") {
            for (size_t p; (p = val.find("<br>")) != std::string::npos;) {
                val.replace(p, 4, "\n");
            }
            out.notes = Trim(val);
        } else if (key == "version") {
            out.version = val;
            // "5553001D (US-029) {C39268AA} {094E0DE2}"
            std::string word;
            bool in_brace = false;
            for (size_t k = 0; k <= val.size(); k++) {
                char c = k < val.size() ? val[k] : ' ';
                if (c == '{' || c == '}' || c == ' ' || c == '(' || c == ')' ||
                    c == ',' || c == '\t') {
                    if (IsHex8(word)) {
                        uint64_t v = 0;
                        ParseHexNumber(word, v);
                        if (in_brace) {
                            out.crcs.push_back((uint32_t)v);
                        } else if (!out.title_id) {
                            out.title_id = (uint32_t)v;
                        }
                    }
                    word.clear();
                    if (c == '{') {
                        in_brace = true;
                    } else if (c == '}') {
                        in_brace = false;
                    }
                } else {
                    word += c;
                }
            }
        }
    }
    if (!saw_header) {
        error = "not a Magic Patch (.jmp) file";
        return false;
    }

    // Patch records
    JmpOption *opt = NULL;
    std::string group_key;
    std::vector<uint8_t> pending_find;
    bool have_find = false;
    for (; i < lines.size(); i++) {
        std::string t = Trim(lines[i]);
        if (t.empty()) {
            continue;
        }
        char where[64];
        snprintf(where, sizeof(where), "line %d: ", (int)(i + 1));
        if (t[0] == '#') {
            if (have_find) {
                error = std::string(where) + "find value without a replacement";
                return false;
            }
            bool off = t.find("[-]") != std::string::npos;
            std::string name = t.substr(1);
            for (size_t p; (p = name.find("[-]")) != std::string::npos;) {
                name.erase(p, 3);
            }
            name = Trim(name);
            std::string key = Lower(name);
            if (!opt || key != group_key) {
                out.options.push_back(JmpOption());
                opt = &out.options.back();
                opt->name = name.empty() ? "Patch" : name;
                opt->default_on = !off;
                std::string xbe = CommentXbe(name);
                opt->other_xbe = !xbe.empty() && xbe != "default.xbe";
                group_key = key;
            }
            continue;
        }
        if (!opt) {
            error = std::string(where) + "patch data before the first # comment";
            return false;
        }
        JmpRecord r;
        size_t colon = t.find(':');
        if (Lower(t.substr(0, 7)) == "append:") {
            r.kind = JmpRecord::Append;
            if (!ParseHexBytes(t.substr(7), r.data)) {
                error = std::string(where) + "invalid APPEND data";
                return false;
            }
        } else if (colon != std::string::npos) {
            r.kind = JmpRecord::Offset;
            if (!ParseHexNumber(t.substr(0, colon), r.offset) ||
                !ParseHexBytes(t.substr(colon + 1), r.data)) {
                error = std::string(where) + "invalid offset record";
                return false;
            }
        } else {
            std::vector<uint8_t> bytes;
            if (!ParseHexBytes(t, bytes)) {
                error = std::string(where) + "invalid hex value";
                return false;
            }
            if (!have_find) {
                pending_find = std::move(bytes);
                have_find = true;
                continue;
            }
            r.kind = JmpRecord::Replace;
            r.find = std::move(pending_find);
            r.data = std::move(bytes);
            have_find = false;
            if (r.find.size() != r.data.size()) {
                error = std::string(where) +
                        "find and replace values have different lengths";
                return false;
            }
        }
        opt->records.push_back(std::move(r));
    }
    if (have_find) {
        error = "the last find value has no replacement";
        return false;
    }
    // Drop comment-only options (notes inside the record list)
    out.options.erase(std::remove_if(out.options.begin(), out.options.end(),
                                     [](const JmpOption &o) {
                                         return o.records.empty();
                                     }),
                      out.options.end());
    if (out.options.empty()) {
        error = "the patch contains no changes";
        return false;
    }
    return true;
}

std::vector<bool> JmpOptionStates(const JmpPatch &patch,
                                  const std::vector<int> &toggled)
{
    std::vector<bool> on;
    for (auto &o : patch.options) {
        on.push_back(o.default_on);
    }
    for (int t : toggled) {
        if (t >= 0 && (size_t)t < on.size()) {
            on[t] = !on[t];
        }
    }
    return on;
}

bool ApplyJmp(std::vector<uint8_t> &xbe, const JmpPatch &patch,
              const std::vector<bool> &option_on, std::string &error)
{
    if (patch.other_target) {
        error = "this patch is for another XBE on the disc, not default.xbe";
        return false;
    }
    for (size_t k = 0; k < patch.options.size(); k++) {
        const JmpOption &o = patch.options[k];
        if (o.other_xbe || k >= option_on.size() || !option_on[k]) {
            continue;
        }
        for (auto &r : o.records) {
            switch (r.kind) {
            case JmpRecord::Replace: {
                auto it = std::search(xbe.begin(), xbe.end(), r.find.begin(),
                                      r.find.end());
                if (it == xbe.end()) {
                    error = "\"" + o.name +
                            "\": the code it changes wasn't found (the patch "
                            "may be for a different version of the game)";
                    return false;
                }
                std::copy(r.data.begin(), r.data.end(), it);
                break;
            }
            case JmpRecord::Offset:
                if (r.offset > xbe.size() ||
                    r.data.size() > xbe.size() - r.offset) {
                    error = "\"" + o.name +
                            "\": writes past the end of default.xbe (the "
                            "patch may be for a different version of the game)";
                    return false;
                }
                std::copy(r.data.begin(), r.data.end(),
                          xbe.begin() + (ptrdiff_t)r.offset);
                break;
            case JmpRecord::Append:
                xbe.insert(xbe.end(), r.data.begin(), r.data.end());
                break;
            }
        }
    }
    return true;
}

JmpFit CheckJmp(const JmpPatch &patch, const std::vector<uint8_t> &stock_xbe,
                uint32_t title_id)
{
    if (patch.title_id && title_id && patch.title_id != title_id) {
        return JmpFit::OtherGame;
    }
    bool any_default_xbe = false;
    for (auto &o : patch.options) {
        any_default_xbe |= !o.other_xbe;
    }
    if (patch.other_target || !any_default_xbe) {
        return JmpFit::Unsupported;
    }
    if (!patch.crcs.empty()) {
        uint32_t crc = Crc32(stock_xbe.data(), stock_xbe.size());
        for (uint32_t c : patch.crcs) {
            if (c == crc) {
                return JmpFit::Works;
            }
        }
        return JmpFit::OtherVersion;
    }
    // No checksum: try every option on a copy
    std::vector<uint8_t> copy = stock_xbe;
    std::vector<bool> all(patch.options.size(), true);
    std::string err;
    if (!ApplyJmp(copy, patch, all, err)) {
        return JmpFit::OtherVersion;
    }
    for (auto &o : patch.options) {
        for (auto &r : o.records) {
            if (!o.other_xbe && r.kind == JmpRecord::Replace) {
                return JmpFit::Works;
            }
        }
    }
    return JmpFit::Unverified;
}

const char *JmpFitLabel(JmpFit fit)
{
    switch (fit) {
    case JmpFit::Works:
        return "Works with your copy";
    case JmpFit::Unverified:
        return "Can't be verified";
    case JmpFit::OtherVersion:
        return "Made for another version";
    case JmpFit::OtherGame:
        return "Made for another game";
    case JmpFit::Unsupported:
        return "Not supported";
    }
    return "";
}

bool ApplyPatchFiles(const std::vector<uint8_t> &original,
                     const std::vector<std::string> &patch_paths,
                     std::vector<uint8_t> &patched, std::string &error)
{
    std::vector<PatchSpec> specs;
    for (auto &p : patch_paths) {
        specs.push_back({ p, {} });
    }
    return ApplyPatchFiles(original, specs, patched, error);
}

bool ApplyPatchFiles(const std::vector<uint8_t> &original,
                     const std::vector<PatchSpec> &patches,
                     std::vector<uint8_t> &patched, std::string &error)
{
    patched = original;
    for (auto &spec : patches) {
        const std::string &path = spec.path;
        std::string name = fs::u8path(path).filename().u8string();
        std::string ext = Lower(fs::u8path(path).extension().u8string());
        std::vector<uint8_t> file;
        if (!ReadFile(path, file)) {
            error = name + ": cannot read the file";
            return false;
        }
        std::string err;
        if (ext == ".xbe") {
            if (file.size() < 0x178 || memcmp(file.data(), "XBEH", 4) != 0) {
                error = name + ": not a valid XBE file";
                return false;
            }
            patched = std::move(file);
        } else if (ext == ".ips") {
            if (!ApplyIpsPatch(patched, file, err)) {
                error = name + ": " + err;
                return false;
            }
        } else if (ext == ".bps") {
            std::vector<uint8_t> out;
            if (!ApplyBpsPatch(patched, file, out, err)) {
                error = name + ": " + err;
                return false;
            }
            patched = std::move(out);
        } else if (ext == ".jmp") {
            JmpPatch jmp;
            if (!ParseJmp(std::string(file.begin(), file.end()), jmp, err)) {
                error = name + ": " + err;
                return false;
            }
            // Checksums are of the unmodified XBE the patch was made for
            if (!jmp.crcs.empty()) {
                uint32_t crc = Crc32(original.data(), original.size());
                if (std::find(jmp.crcs.begin(), jmp.crcs.end(), crc) ==
                    jmp.crcs.end()) {
                    error = name + ": made for a different version or region "
                                   "of this game";
                    return false;
                }
            }
            if (!ApplyJmp(patched, jmp, JmpOptionStates(jmp, spec.toggled),
                          err)) {
                error = name + ": " + err;
                return false;
            }
        } else {
            error = name +
                    ": unsupported patch type (use .xbe, .ips, .bps or .jmp)";
            return false;
        }
    }
    if (patched.size() < 0x178 || memcmp(patched.data(), "XBEH", 4) != 0) {
        error = "the patched default.xbe is not a valid XBE";
        return false;
    }
    return true;
}

bool WriteQcow2Overlay(
    const std::string &overlay_path, const std::string &backing_path,
    uint64_t virtual_size,
    const std::vector<std::pair<uint64_t, std::vector<uint8_t>>> &writes,
    std::string &error)
{
    const uint32_t cluster_bits = 16;
    const uint64_t C = 1ull << cluster_bits;
    const uint64_t l2_entries = C / 8;
    const uint64_t l2_coverage = C * l2_entries;
    const uint64_t QCOW_OFLAG_COPIED = 1ull << 63;

    std::ifstream backing(fs::u8path(backing_path), std::ios::binary);
    if (!backing) {
        error = "cannot open the disc image";
        return false;
    }
    backing.seekg(0, std::ios::end);
    uint64_t backing_size = (uint64_t)backing.tellg();

    // Full contents of every cluster the writes touch: original data from
    // the backing image with the writes applied on top.
    std::map<uint64_t, std::vector<uint8_t>> clusters;
    for (auto &[off, bytes] : writes) {
        if (bytes.empty()) {
            continue;
        }
        if (off + bytes.size() > virtual_size) {
            error = "patch data lies outside the disc image";
            return false;
        }
        for (uint64_t c = off / C; c <= (off + bytes.size() - 1) / C; c++) {
            auto it = clusters.find(c);
            if (it == clusters.end()) {
                std::vector<uint8_t> data(C);
                if (!ReadAt(backing, backing_size, c * C, data.data(), C)) {
                    error = "cannot read the disc image";
                    return false;
                }
                it = clusters.emplace(c, std::move(data)).first;
            }
            uint64_t start = std::max(off, c * C);
            uint64_t stop = std::min(off + bytes.size(), (c + 1) * C);
            memcpy(&it->second[start - c * C], &bytes[start - off],
                   stop - start);
        }
    }

    uint64_t l1_size = (virtual_size + l2_coverage - 1) / l2_coverage;
    uint64_t l1_clusters = std::max<uint64_t>(1, (l1_size * 8 + C - 1) / C);
    std::map<uint64_t, uint64_t> l2_offsets; // l1 index -> l2 table offset
    for (auto &[c, data] : clusters) {
        l2_offsets[c / l2_entries] = 0;
    }

    // Layout: header | refcount table | refcount block | L1 | L2s | data
    uint64_t next = 3 + l1_clusters;
    for (auto &[idx, l2off] : l2_offsets) {
        l2off = (next++) * C;
    }
    std::map<uint64_t, uint64_t> data_offsets; // cluster -> file offset
    for (auto &[c, data] : clusters) {
        data_offsets[c] = (next++) * C;
    }
    uint64_t total_clusters = next;
    if (total_clusters > C / 2) {
        error = "patch is too large for the overlay";
        return false;
    }

    std::vector<uint8_t> file(total_clusters * C, 0);
    uint8_t *h = file.data();
    PutBe32(h + 0, 0x514649fbu); // "QFI\xfb"
    PutBe32(h + 4, 2);           // version
    // Header extensions follow the 72-byte header; backing name after them
    uint8_t *ext = h + 72;
    PutBe32(ext + 0, 0xE2792ACAu); // backing file format name
    PutBe32(ext + 4, 3);
    memcpy(ext + 8, "raw", 3);
    PutBe32(ext + 16, 0); // end of extensions
    PutBe32(ext + 20, 0);
    uint64_t backing_name_off = 72 + 24;
    if (backing_path.size() > 1023 ||
        backing_name_off + backing_path.size() > C) {
        error = "disc image path is too long";
        return false;
    }
    memcpy(h + backing_name_off, backing_path.data(), backing_path.size());
    PutBe64(h + 8, backing_name_off);
    PutBe32(h + 16, (uint32_t)backing_path.size());
    PutBe32(h + 20, cluster_bits);
    PutBe64(h + 24, virtual_size);
    PutBe32(h + 32, 0); // no encryption
    PutBe32(h + 36, (uint32_t)l1_size);
    PutBe64(h + 40, 3 * C);  // L1 table offset
    PutBe64(h + 48, 1 * C);  // refcount table offset
    PutBe32(h + 56, 1);      // refcount table clusters
    PutBe32(h + 60, 0);      // snapshots
    PutBe64(h + 64, 0);

    PutBe64(file.data() + 1 * C, 2 * C); // refcount table -> block
    for (uint64_t i = 0; i < total_clusters; i++) {
        PutBe16(file.data() + 2 * C + i * 2, 1);
    }
    for (auto &[idx, l2off] : l2_offsets) {
        PutBe64(file.data() + 3 * C + idx * 8, l2off | QCOW_OFLAG_COPIED);
    }
    for (auto &[c, data] : clusters) {
        uint64_t l2off = l2_offsets[c / l2_entries];
        PutBe64(file.data() + l2off + (c % l2_entries) * 8,
                data_offsets[c] | QCOW_OFLAG_COPIED);
        memcpy(file.data() + data_offsets[c], data.data(), C);
    }

    std::error_code ec;
    fs::path out = fs::u8path(overlay_path);
    fs::create_directories(out.parent_path(), ec);
    fs::path tmp = out;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write((const char *)file.data(), (std::streamsize)file.size());
        if (!f) {
            error = "cannot write the overlay file";
            return false;
        }
    }
    fs::remove(out, ec);
    fs::rename(tmp, out, ec);
    if (ec) {
        error = "cannot write the overlay file";
        return false;
    }
    return true;
}

namespace {
bool ReadXbeAt(const std::string &image_path, const XbeLocation &loc,
               std::vector<uint8_t> &xbe, std::string &error)
{
    xbe.assign(loc.xbe_size, 0);
    std::ifstream f(fs::u8path(image_path), std::ios::binary);
    if (!f || !ReadAt(f, loc.image_size, loc.xbe_offset, xbe.data(),
                      xbe.size())) {
        error = "cannot read default.xbe from the disc image";
        return false;
    }
    return true;
}
} // namespace

bool ReadDefaultXbe(const std::string &image_path, std::vector<uint8_t> &xbe,
                    std::string &error)
{
    XbeLocation loc;
    return LocateDefaultXbe(image_path, loc, error) &&
           ReadXbeAt(image_path, loc, xbe, error);
}

bool BuildPatchedOverlay(const std::string &image_path,
                         const std::vector<std::string> &patch_paths,
                         const std::string &overlay_path, std::string &error)
{
    std::vector<PatchSpec> specs;
    for (auto &p : patch_paths) {
        specs.push_back({ p, {} });
    }
    return BuildPatchedOverlay(image_path, specs, overlay_path, error);
}

bool BuildPatchedOverlay(const std::string &image_path,
                         const std::vector<PatchSpec> &patches,
                         const std::string &overlay_path, std::string &error)
{
    XbeLocation loc;
    if (!LocateDefaultXbe(image_path, loc, error)) {
        return false;
    }
    std::vector<uint8_t> original;
    if (!ReadXbeAt(image_path, loc, original, error)) {
        return false;
    }
    std::vector<uint8_t> patched;
    if (!ApplyPatchFiles(original, patches, patched, error)) {
        return false;
    }

    // The patched XBE has to fit in the sectors the original occupies
    uint64_t room = (loc.xbe_size + 2047ull) / 2048 * 2048;
    room = std::min<uint64_t>(room, loc.image_size - loc.xbe_offset);
    if (patched.size() > room) {
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "the patched default.xbe is %llu bytes, but the disc only "
                 "has room for %llu bytes",
                 (unsigned long long)patched.size(), (unsigned long long)room);
        error = buf;
        return false;
    }

    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> writes;
    writes.push_back({ loc.xbe_offset, patched });
    if (patched.size() != loc.xbe_size) {
        uint32_t n = (uint32_t)patched.size();
        writes.push_back({ loc.size_field_offset,
                           { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16),
                             (uint8_t)(n >> 24) } });
    }
    std::string abs_image = fs::absolute(fs::u8path(image_path)).u8string();
    return WriteQcow2Overlay(overlay_path, abs_image, loc.image_size, writes,
                             error);
}
