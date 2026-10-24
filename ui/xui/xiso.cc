//
// xemu User Interface - Xbox disc image (XISO / XDVDFS) layout
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "xiso.hh"

#include <algorithm>
#include <cstring>

// XDVDFS in brief: sector 32 holds the volume descriptor, which points at
// the root directory table. A directory table is a binary search tree of
// entries (left and right child offsets in 4-byte units, first entry is the
// root) ordered by XisoCompareNames; entries are 4-byte aligned, never cross
// a sector, and unused bytes are 0xFF. Each entry gives the start sector and
// size of a file, or of a folder's own table.

namespace
{
const char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr uint32_t kVolumeSector = 32;
constexpr uint8_t kAttrDir = 0x10, kAttrFile = 0x20;
constexpr uint64_t kImageModulus = 0x10000;

void Put16(std::vector<uint8_t> &b, size_t at, uint16_t v)
{
    b[at] = (uint8_t)v;
    b[at + 1] = (uint8_t)(v >> 8);
}

void Put32(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        b[at + i] = (uint8_t)(v >> (8 * i));
    }
}

uint32_t Sectors(uint64_t bytes)
{
    return (uint32_t)((bytes + kXisoSectorSize - 1) / kXisoSectorSize);
}

size_t EntrySize(const XisoNode &n)
{
    return (14 + n.name.size() + 3) & ~(size_t)3;
}

// A folder's table being built: kids sorted, the tree, entry offsets
struct Table {
    XisoNode *dir = nullptr;
    std::vector<XisoNode *> kids; // sorted
    std::vector<int> left, right; // child index or -1
    std::vector<uint32_t> offset; // bytes from the table start
    uint32_t bytes = 0;           // whole sectors
    int root = -1;
};

int Build(Table &t, int lo, int hi)
{
    if (lo > hi) {
        return -1;
    }
    int mid = (lo + hi) / 2;
    t.left[mid] = Build(t, lo, mid - 1);
    t.right[mid] = Build(t, mid + 1, hi);
    return mid;
}

// Pre-order, so the tree's root comes first at offset 0
void PlaceEntries(Table &t, int i, uint32_t &pos)
{
    if (i < 0) {
        return;
    }
    size_t sz = EntrySize(*t.kids[i]);
    if (pos % kXisoSectorSize + sz > kXisoSectorSize) {
        pos = (pos / kXisoSectorSize + 1) * kXisoSectorSize;
    }
    t.offset[i] = pos;
    pos += (uint32_t)sz;
    PlaceEntries(t, t.left[i], pos);
    PlaceEntries(t, t.right[i], pos);
}

bool PrepareTables(XisoNode &dir, std::vector<Table> &tables, std::string *err)
{
    Table t;
    t.dir = &dir;
    for (auto &k : dir.kids) {
        if (k.name.empty() || k.name.size() > 255) {
            *err = "\"" + k.name + "\": unusable name for a disc image";
            return false;
        }
        if (!k.dir && k.size > 0xFFFFFFFFull) {
            *err = "\"" + k.name + "\" is larger than 4 GB";
            return false;
        }
        t.kids.push_back(&k);
    }
    std::sort(t.kids.begin(), t.kids.end(), [](XisoNode *a, XisoNode *b) {
        return XisoCompareNames(a->name, b->name) < 0;
    });
    for (size_t i = 1; i < t.kids.size(); i++) {
        if (XisoCompareNames(t.kids[i - 1]->name, t.kids[i]->name) == 0) {
            *err = "\"" + t.kids[i]->name + "\" appears twice (names ignore case)";
            return false;
        }
    }
    size_t n = t.kids.size();
    t.left.assign(n, -1);
    t.right.assign(n, -1);
    t.offset.assign(n, 0);
    uint32_t pos = 0;
    if (n) {
        t.root = Build(t, 0, (int)n - 1);
        PlaceEntries(t, t.root, pos);
    }
    // Offsets are stored in 4-byte units in 16 bits
    if (pos / 4 > 0xFFFF) {
        *err = "Too many files in one folder for a disc image";
        return false;
    }
    t.bytes = std::max<uint32_t>(1, Sectors(pos)) * kXisoSectorSize;
    tables.push_back(t);
    for (auto &k : dir.kids) {
        if (k.dir && !PrepareTables(k, tables, err)) {
            return false;
        }
    }
    return true;
}

void CollectFiles(XisoNode &dir, std::vector<XisoNode *> &out)
{
    for (auto &k : dir.kids) {
        if (!k.dir) {
            out.push_back(&k);
        }
    }
    for (auto &k : dir.kids) {
        if (k.dir) {
            CollectFiles(k, out);
        }
    }
}
} // namespace

int XisoCompareNames(const std::string &a, const std::string &b)
{
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'a' && x <= 'z') {
            x -= 32;
        }
        if (y >= 'a' && y <= 'z') {
            y -= 32;
        }
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

bool XisoLayout(XisoNode &root, XisoPlan *plan, std::string *err)
{
    *plan = XisoPlan();
    root.dir = true;
    std::vector<Table> tables;
    if (!PrepareTables(root, tables, err)) {
        return false;
    }
    // Directory tables first, right after the volume descriptor, then files
    uint32_t sector = kVolumeSector + 1;
    for (auto &t : tables) {
        t.dir->sector = sector;
        t.dir->size = t.bytes;
        sector += t.bytes / kXisoSectorSize;
    }
    std::vector<XisoNode *> files;
    CollectFiles(root, files);
    for (XisoNode *f : files) {
        f->sector = sector;
        sector += Sectors(f->size);
        if (f->size) {
            plan->files.push_back(f);
            plan->data_bytes += f->size;
        }
    }
    uint64_t end = (uint64_t)sector * kXisoSectorSize;
    plan->image_size = (end + kImageModulus - 1) / kImageModulus * kImageModulus;

    // Volume descriptor
    std::vector<uint8_t> vd(kXisoSectorSize, 0);
    memcpy(vd.data(), kMagic, 20);
    Put32(vd, 20, root.sector);
    Put32(vd, 24, (uint32_t)root.size);
    // 0x1C: creation time (FILETIME), left at zero
    memcpy(vd.data() + 0x7EC, kMagic, 20);
    plan->blobs.push_back({ (uint64_t)kVolumeSector * kXisoSectorSize, vd });

    for (auto &t : tables) {
        std::vector<uint8_t> b(t.bytes, 0xFF);
        for (size_t i = 0; i < t.kids.size(); i++) {
            const XisoNode &k = *t.kids[i];
            size_t at = t.offset[i];
            Put16(b, at, t.left[i] < 0 ? 0 : (uint16_t)(t.offset[t.left[i]] / 4));
            Put16(b, at + 2, t.right[i] < 0 ? 0 : (uint16_t)(t.offset[t.right[i]] / 4));
            Put32(b, at + 4, k.sector);
            Put32(b, at + 8, (uint32_t)k.size);
            b[at + 12] = k.dir ? kAttrDir : kAttrFile;
            b[at + 13] = (uint8_t)k.name.size();
            memcpy(&b[at + 14], k.name.data(), k.name.size());
        }
        plan->blobs.push_back({ (uint64_t)t.dir->sector * kXisoSectorSize, std::move(b) });
    }
    return true;
}
