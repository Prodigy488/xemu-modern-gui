//
// xemu User Interface - Game library disc scanning
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "game-library-scan.hh"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace {

const size_t kSectorSize = 2048;
const char kXdvdfsMagic[] = "MICROSOFT*XBOX*MEDIA";
const size_t kXdvdfsMagicLen = 20;

// Where the XDVDFS game partition can start inside an image:
// plain XISO, XGD3, XGD2 and XGD1 (Redump-style full disc dumps).
const uint64_t kPartitionOffsets[] = { 0, 0x2080000, 0xFD90000, 0x18300000 };

uint16_t Le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
uint32_t Le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

class ImageFile {
public:
    explicit ImageFile(const std::string &path)
        : f(std::filesystem::u8path(path), std::ios::binary)
    {
        if (f) {
            f.seekg(0, std::ios::end);
            size = (uint64_t)f.tellg();
        }
    }
    bool Ok() const { return (bool)f; }
    uint64_t Size() const { return size; }
    bool Read(uint64_t off, size_t len, std::vector<uint8_t> &buf)
    {
        if (off + len > size) {
            return false;
        }
        buf.resize(len);
        f.clear();
        f.seekg((std::streamoff)off);
        f.read((char *)buf.data(), (std::streamsize)len);
        return (size_t)f.gcount() == len;
    }

private:
    std::ifstream f;
    uint64_t size = 0;
};

bool NameEquals(const uint8_t *name, size_t len, const char *want)
{
    if (len != strlen(want)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (tolower(name[i]) != tolower((unsigned char)want[i])) {
            return false;
        }
    }
    return true;
}

// Walk the XDVDFS directory binary tree looking for `want`.
bool FindInDirectory(const std::vector<uint8_t> &dir, const char *want,
                     uint32_t &sector, uint32_t &size)
{
    std::vector<uint32_t> stack = { 0 };
    std::unordered_set<uint32_t> visited;
    while (!stack.empty()) {
        uint32_t pos = stack.back() * 4;
        stack.pop_back();
        if (!visited.insert(pos).second || pos + 14 > dir.size()) {
            continue;
        }
        const uint8_t *e = &dir[pos];
        uint16_t left = Le16(e), right = Le16(e + 2);
        if (left == 0xFFFF && right == 0xFFFF) {
            continue; // padding
        }
        uint8_t name_len = e[13];
        if (pos + 14 + name_len <= dir.size() &&
            NameEquals(e + 14, name_len, want)) {
            sector = Le32(e + 4);
            size = Le32(e + 8);
            return true;
        }
        if (left && left != 0xFFFF) {
            stack.push_back(left);
        }
        if (right && right != 0xFFFF) {
            stack.push_back(right);
        }
    }
    return false;
}

std::string Utf16ToUtf8(const uint8_t *p, size_t max_chars)
{
    std::string out;
    for (size_t i = 0; i < max_chars; i++) {
        uint32_t c = Le16(p + i * 2);
        if (c == 0) {
            break;
        }
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < max_chars) {
            uint32_t lo = Le16(p + (i + 1) * 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    // Trim trailing spaces some titles pad with
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

// --- Texture decoding -------------------------------------------------------

void Rgb565(uint16_t c, uint8_t out[3])
{
    out[0] = ((c >> 11) & 0x1F) * 255 / 31;
    out[1] = ((c >> 5) & 0x3F) * 255 / 63;
    out[2] = (c & 0x1F) * 255 / 31;
}

// Decode the color part of a DXT block into 16 RGBA pixels.
void DecodeDxtColor(const uint8_t *b, bool dxt1, uint8_t px[16][4])
{
    uint16_t c0 = Le16(b), c1 = Le16(b + 2);
    uint8_t pal[4][4];
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (!dxt1 || c0 > c1) {
        for (int i = 0; i < 3; i++) {
            pal[2][i] = (2 * pal[0][i] + pal[1][i]) / 3;
            pal[3][i] = (pal[0][i] + 2 * pal[1][i]) / 3;
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++) {
            pal[2][i] = (pal[0][i] + pal[1][i]) / 2;
            pal[3][i] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
    uint32_t idx = Le32(b + 4);
    for (int i = 0; i < 16; i++) {
        memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
    }
}

void DecodeDxt5Alpha(const uint8_t *b, uint8_t px[16][4])
{
    uint8_t a[8];
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1]) {
        for (int i = 1; i < 7; i++) {
            a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
        }
    } else {
        for (int i = 1; i < 5; i++) {
            a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
        }
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) {
        bits |= (uint64_t)b[2 + i] << (8 * i);
    }
    for (int i = 0; i < 16; i++) {
        px[i][3] = a[(bits >> (3 * i)) & 7];
    }
}

bool DecodeDxt(const uint8_t *data, size_t len, int w, int h, int fmt,
               std::vector<uint8_t> &rgba)
{
    size_t block_bytes = (fmt == 1) ? 8 : 16;
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    if ((size_t)bw * bh * block_bytes > len) {
        return false;
    }
    rgba.assign((size_t)w * h * 4, 0);
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            const uint8_t *b = data + ((size_t)by * bw + bx) * block_bytes;
            uint8_t px[16][4];
            if (fmt == 1) {
                DecodeDxtColor(b, true, px);
            } else {
                DecodeDxtColor(b + 8, false, px);
                if (fmt == 3) {
                    for (int i = 0; i < 16; i++) {
                        px[i][3] = ((b[i / 2] >> (4 * (i & 1))) & 0xF) * 17;
                    }
                } else {
                    DecodeDxt5Alpha(b, px);
                }
            }
            for (int i = 0; i < 16; i++) {
                int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) {
                    memcpy(&rgba[((size_t)y * w + x) * 4], px[i], 4);
                }
            }
        }
    }
    return true;
}

// Xbox texture swizzle (same mask scheme as hw/xbox/nv2a/pgraph/swizzle.c)
void SwizzleMasks(unsigned w, unsigned h, uint32_t &mx, uint32_t &my)
{
    mx = my = 0;
    uint32_t bit = 1, mask_bit = 1;
    bool done;
    do {
        done = true;
        if (bit < w) {
            mx |= mask_bit;
            mask_bit <<= 1;
            done = false;
        }
        if (bit < h) {
            my |= mask_bit;
            mask_bit <<= 1;
            done = false;
        }
        bit <<= 1;
    } while (!done);
}

uint32_t Deposit(uint32_t v, uint32_t mask)
{
    uint32_t r = 0;
    for (uint32_t m = 1; mask && v; m <<= 1) {
        if (mask & m) {
            if (v & 1) {
                r |= m;
            }
            v >>= 1;
            mask &= ~m;
        }
    }
    return r;
}

bool DecodeArgb8(const uint8_t *data, size_t len, int w, int h, int pitch,
                 bool swizzled, bool has_alpha, std::vector<uint8_t> &rgba)
{
    if (swizzled) {
        if ((size_t)w * h * 4 > len) {
            return false;
        }
    } else if ((size_t)pitch * (h - 1) + (size_t)w * 4 > len) {
        return false;
    }
    uint32_t mx = 0, my = 0;
    if (swizzled) {
        SwizzleMasks(w, h, mx, my);
    }
    rgba.resize((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t src = swizzled ? (size_t)(Deposit(x, mx) | Deposit(y, my)) * 4
                                  : (size_t)y * pitch + (size_t)x * 4;
            uint8_t *d = &rgba[((size_t)y * w + x) * 4];
            d[0] = data[src + 2];
            d[1] = data[src + 1];
            d[2] = data[src + 0];
            d[3] = has_alpha ? data[src + 3] : 255;
        }
    }
    return true;
}

} // namespace

bool DecodeXprImage(const uint8_t *data, size_t len, int &w, int &h,
                    std::vector<uint8_t> &rgba)
{
    if (len < 32 || memcmp(data, "XPR0", 4) != 0) {
        return false;
    }
    uint32_t header_size = Le32(data + 8);
    if (header_size < 32 || header_size > len) {
        return false;
    }
    // First resource: D3DTexture {Common, Data, Lock, Format, Size}
    const uint8_t *res = data + 12;
    uint32_t tex_data = Le32(res + 4);
    uint32_t format = Le32(res + 12);
    uint32_t size = Le32(res + 16);

    int color = (format >> 8) & 0xFF;
    int pitch = 0;
    if (size) {
        w = (size & 0xFFF) + 1;
        h = ((size >> 12) & 0xFFF) + 1;
        pitch = (((size >> 24) & 0xFF) + 1) * 64;
    } else {
        w = 1 << ((format >> 20) & 0xF);
        h = 1 << ((format >> 24) & 0xF);
        pitch = w * 4;
    }
    if (w < 1 || h < 1 || w > 2048 || h > 2048) {
        return false;
    }
    size_t off = (size_t)header_size + tex_data;
    if (off >= len) {
        return false;
    }
    const uint8_t *pix = data + off;
    size_t pix_len = len - off;

    switch (color) {
    case 0x0C: return DecodeDxt(pix, pix_len, w, h, 1, rgba); // DXT1
    case 0x0E: return DecodeDxt(pix, pix_len, w, h, 3, rgba); // DXT2/3
    case 0x0F: return DecodeDxt(pix, pix_len, w, h, 5, rgba); // DXT4/5
    case 0x06: return DecodeArgb8(pix, pix_len, w, h, 0, true, true, rgba);
    case 0x07: return DecodeArgb8(pix, pix_len, w, h, 0, true, false, rgba);
    case 0x12: return DecodeArgb8(pix, pix_len, w, h, pitch, false, true, rgba);
    case 0x1E: return DecodeArgb8(pix, pix_len, w, h, pitch, false, false, rgba);
    default: return false;
    }
}

bool ReadDiscTitleInfo(const std::string &path, DiscTitleInfo &out)
{
    out = DiscTitleInfo();
    ImageFile img(path);
    if (!img.Ok()) {
        out.error = "Cannot open file";
        return false;
    }

    // Locate the XDVDFS partition
    std::vector<uint8_t> buf;
    uint64_t base = 0;
    bool found = false;
    for (uint64_t off : kPartitionOffsets) {
        if (img.Read(off + 32 * kSectorSize, kSectorSize, buf) &&
            memcmp(buf.data(), kXdvdfsMagic, kXdvdfsMagicLen) == 0) {
            base = off;
            found = true;
            break;
        }
    }
    if (!found) {
        out.error = "Not an Xbox disc image";
        return false;
    }

    uint32_t root_sector = Le32(&buf[20]);
    uint32_t root_size = Le32(&buf[24]);
    if (root_size == 0 || root_size > 64 * 1024 * 1024) {
        out.error = "Bad root directory";
        return false;
    }
    std::vector<uint8_t> dir;
    if (!img.Read(base + (uint64_t)root_sector * kSectorSize, root_size, dir)) {
        out.error = "Cannot read root directory";
        return false;
    }

    uint32_t xbe_sector, xbe_size;
    if (!FindInDirectory(dir, "default.xbe", xbe_sector, xbe_size)) {
        out.error = "No default.xbe";
        return false;
    }
    uint64_t xbe_off = base + (uint64_t)xbe_sector * kSectorSize;

    // XBE image header
    std::vector<uint8_t> hdr;
    if (!img.Read(xbe_off, std::min<uint32_t>(xbe_size, 0x1000), hdr) ||
        memcmp(hdr.data(), "XBEH", 4) != 0 || hdr.size() < 0x178) {
        out.error = "Invalid XBE";
        return false;
    }
    uint32_t base_addr = Le32(&hdr[0x104]);
    uint32_t headers_size = Le32(&hdr[0x108]);
    if (headers_size < 0x178 || headers_size > xbe_size ||
        headers_size > 16 * 1024 * 1024) {
        out.error = "Invalid XBE headers";
        return false;
    }
    if (!img.Read(xbe_off, headers_size, hdr)) {
        out.error = "Cannot read XBE headers";
        return false;
    }

    // Certificate: title ID, name, region, disc, version
    uint32_t cert_addr = Le32(&hdr[0x118]);
    uint64_t cert = (uint64_t)cert_addr - base_addr;
    if (cert_addr < base_addr || cert + 0xB0 > hdr.size()) {
        out.error = "Invalid XBE certificate";
        return false;
    }
    const uint8_t *c = &hdr[cert];
    out.title_id = Le32(c + 0x08);
    out.title_name = Utf16ToUtf8(c + 0x0C, 40);
    out.region = Le32(c + 0xA0);
    out.disc_number = Le32(c + 0xA8);
    out.version = Le32(c + 0xAC);
    out.ok = true;

    // Title image from the $$XTIMAGE section (optional)
    uint32_t num_sections = Le32(&hdr[0x11C]);
    uint32_t sections_addr = Le32(&hdr[0x120]);
    uint64_t sec = (uint64_t)sections_addr - base_addr;
    if (sections_addr >= base_addr && num_sections < 1024 &&
        sec + (uint64_t)num_sections * 56 <= hdr.size()) {
        for (uint32_t i = 0; i < num_sections; i++) {
            const uint8_t *s = &hdr[sec + i * 56];
            uint32_t raw_addr = Le32(s + 12);
            uint32_t raw_size = Le32(s + 16);
            uint32_t name_addr = Le32(s + 20);
            if (name_addr < base_addr) {
                continue;
            }
            uint64_t name_off = (uint64_t)name_addr - base_addr;
            const char want[] = "$$XTIMAGE";
            if (name_off + sizeof(want) > hdr.size() ||
                memcmp(&hdr[name_off], want, sizeof(want)) != 0) {
                continue;
            }
            std::vector<uint8_t> xpr;
            if (raw_size && raw_size < 8 * 1024 * 1024 &&
                (uint64_t)raw_addr + raw_size <= xbe_size &&
                img.Read(xbe_off + raw_addr, raw_size, xpr)) {
                DecodeXprImage(xpr.data(), xpr.size(), out.icon_w, out.icon_h,
                               out.icon_rgba);
                if (out.icon_rgba.empty()) {
                    out.icon_w = out.icon_h = 0;
                }
            }
            break;
        }
    }
    return true;
}

std::string FormatTitleId(uint32_t title_id)
{
    char a = (char)((title_id >> 24) & 0xFF), b = (char)((title_id >> 16) & 0xFF);
    char buf[32];
    if (isalnum((unsigned char)a) && isalnum((unsigned char)b)) {
        snprintf(buf, sizeof(buf), "%c%c-%03u", a, b, title_id & 0xFFFF);
    } else {
        snprintf(buf, sizeof(buf), "%08X", title_id);
    }
    return buf;
}

std::string FormatRegion(uint32_t region)
{
    region &= 0x7;
    if (region == 0x7) {
        return "Region free";
    }
    std::string s;
    if (region & 0x1) {
        s += "NTSC-U";
    }
    if (region & 0x2) {
        s += s.empty() ? "NTSC-J" : " / NTSC-J";
    }
    if (region & 0x4) {
        s += s.empty() ? "PAL" : " / PAL";
    }
    return s.empty() ? "Unknown" : s;
}
