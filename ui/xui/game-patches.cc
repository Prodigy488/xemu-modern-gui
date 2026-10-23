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

bool ApplyPatchFiles(const std::vector<uint8_t> &original,
                     const std::vector<std::string> &patch_paths,
                     std::vector<uint8_t> &patched, std::string &error)
{
    patched = original;
    for (auto &path : patch_paths) {
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
        } else {
            error = name + ": unsupported patch type (use .xbe, .ips or .bps)";
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

bool BuildPatchedOverlay(const std::string &image_path,
                         const std::vector<std::string> &patch_paths,
                         const std::string &overlay_path, std::string &error)
{
    XbeLocation loc;
    if (!LocateDefaultXbe(image_path, loc, error)) {
        return false;
    }
    std::vector<uint8_t> original(loc.xbe_size);
    {
        std::ifstream f(fs::u8path(image_path), std::ios::binary);
        if (!f || !ReadAt(f, loc.image_size, loc.xbe_offset, original.data(),
                          original.size())) {
            error = "cannot read default.xbe from the disc image";
            return false;
        }
    }
    std::vector<uint8_t> patched;
    if (!ApplyPatchFiles(original, patch_paths, patched, error)) {
        return false;
    }

    // The patched XBE has to fit in the sectors the original occupies
    uint64_t room = (loc.xbe_size + 2047ull) / 2048 * 2048;
    room = std::min<uint64_t>(room, loc.image_size - loc.xbe_offset);
    if (patched.size() > room) {
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "the patched default.xbe is %zu bytes, but the disc only "
                 "has room for %llu bytes",
                 patched.size(), (unsigned long long)room);
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
