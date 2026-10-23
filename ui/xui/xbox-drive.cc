//
// xemu User Interface - Xbox hard drive access
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "xbox-drive.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../thirdparty/libfatx/fatx.h"

// libfatx internals used for free space and flushing (fatx_internal.h is
// C-only, so these are declared here)
extern "C" {
int fatx_flush_fat_cache(struct fatx_fs *fs);
int fatx_read_fat(struct fatx_fs *fs, size_t index, uint32_t *entry);
int fatx_get_fat_entry_type(struct fatx_fs *fs, uint32_t entry);
}
static const int kFatxClusterAvailable = 0; // FATX_CLUSTER_AVAILABLE

// ---- Raw image files -----------------------------------------------------

namespace
{
class FileDiskIO : public XboxDiskIO
{
public:
    explicit FileDiskIO(FILE *f) : m_f(f)
    {
    }
    ~FileDiskIO() override
    {
        fclose(m_f);
    }
    bool Seek(uint64_t off)
    {
#ifdef _WIN32
        return _fseeki64(m_f, (int64_t)off, SEEK_SET) == 0;
#else
        return fseeko(m_f, (off_t)off, SEEK_SET) == 0;
#endif
    }
    bool Read(uint64_t off, void *buf, size_t len) override
    {
        return Seek(off) && fread(buf, 1, len, m_f) == len;
    }
    bool Write(uint64_t off, const void *buf, size_t len) override
    {
        return Seek(off) && fwrite(buf, 1, len, m_f) == len;
    }
    uint64_t Size() override
    {
#ifdef _WIN32
        _fseeki64(m_f, 0, SEEK_END);
        return (uint64_t)_ftelli64(m_f);
#else
        fseeko(m_f, 0, SEEK_END);
        return (uint64_t)ftello(m_f);
#endif
    }
    void Flush() override
    {
        fflush(m_f);
    }

private:
    FILE *m_f;
};

int IoRead(void *opaque, uint64_t offset, void *buf, size_t len)
{
    return static_cast<XboxDiskIO *>(opaque)->Read(offset, buf, len) ? 0 : -1;
}

int IoWrite(void *opaque, uint64_t offset, const void *buf, size_t len)
{
    return static_cast<XboxDiskIO *>(opaque)->Write(offset, buf, len) ? 0 : -1;
}

} // namespace

std::unique_ptr<XboxDiskIO> XboxDiskOpenFile(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "r+b");
    if (!f) {
        return nullptr;
    }
    return std::make_unique<FileDiskIO>(f);
}

// ---- Names and paths -----------------------------------------------------

bool XboxValidName(const std::string &name, std::string *why)
{
    if (name.empty() || name == "." || name == "..") {
        *why = "Enter a name";
        return false;
    }
    if (name.size() > kXboxMaxName) {
        *why = "Too long for the Xbox (42 characters at most)";
        return false;
    }
    for (unsigned char c : name) {
        if (c < 0x20 || c > 0x7e || strchr("<>:\"/\\|?*", c)) {
            *why = "The Xbox can't use the character '";
            if (c >= 0x20 && c <= 0x7e) {
                *why += (char)c;
            } else {
                *why += "?";
            }
            *why += "' (letters, numbers, spaces and simple symbols only)";
            return false;
        }
    }
    return true;
}

std::string XboxJoin(const std::string &dir, const std::string &name)
{
    if (dir.empty() || dir == "/") {
        return "/" + name;
    }
    return dir + "/" + name;
}

std::string XboxParent(const std::string &path)
{
    size_t p = path.find_last_of('/');
    if (p == std::string::npos || p == 0) {
        return "/";
    }
    return path.substr(0, p);
}

// ---- Drive ---------------------------------------------------------------

struct XboxDrive::Vol {
    XboxVolume info;
    struct fatx_fs fs;
};

XboxDrive::XboxDrive() = default;

XboxDrive::~XboxDrive()
{
    Close();
}

bool XboxDrive::Open(std::unique_ptr<XboxDiskIO> io, std::string *err)
{
    Close();
    if (!io) {
        if (err->empty()) {
            *err = "No hard disk image";
        }
        return false;
    }
    m_io = std::move(io);
    uint64_t disk_size = m_io->Size();

    static const struct {
        char letter;
        const char *label;
    } parts[] = {
        { 'C', "System" }, { 'E', "Data" },   { 'F', "Extended" },
        { 'X', "Cache 1" }, { 'Y', "Cache 2" }, { 'Z', "Cache 3" },
    };
    for (auto &p : parts) {
        uint64_t offset, size;
        if (fatx_drive_to_offset_size((char)(p.letter - 'A' + 'a'), &offset,
                                      &size)) {
            continue;
        }
        if (size == (uint64_t)-1) { // F: the rest of the disk
            if (disk_size <= offset + (64ull << 20)) {
                continue;
            }
            size = (disk_size - offset) & ~511ull;
        }
        if (offset + size > disk_size) {
            continue;
        }
        auto v = std::make_unique<Vol>();
        memset(&v->fs, 0, sizeof(v->fs));
        v->fs.io_opaque = m_io.get();
        v->fs.io_read = IoRead;
        v->fs.io_write = IoWrite;
        if (fatx_open_device(&v->fs, "xbox-hdd", offset, size, 512,
                             FATX_READ_FROM_SUPERBLOCK)) {
            continue; // not formatted
        }
        v->info.letter = p.letter;
        v->info.label = p.label;
        v->info.size = size;
        m_volumes.push_back(v->info);
        m_vols.push_back(std::move(v));
    }
    if (m_vols.empty()) {
        Close();
        *err = "No Xbox partitions found on the hard disk image";
        return false;
    }
    return true;
}

void XboxDrive::Close()
{
    for (auto &v : m_vols) {
        fatx_close_device(&v->fs);
        free(v->fs.fat_cache.data); // not freed by libfatx
        v->fs.fat_cache.data = nullptr;
    }
    m_vols.clear();
    m_volumes.clear();
    if (m_io) {
        m_io->Flush();
    }
    m_io.reset();
}

XboxDrive::Vol *XboxDrive::Find(char letter)
{
    for (auto &v : m_vols) {
        if (v->info.letter == letter) {
            return v.get();
        }
    }
    return nullptr;
}

const XboxVolume *XboxDrive::Volume(char letter) const
{
    for (auto &v : m_volumes) {
        if (v.letter == letter) {
            return &v;
        }
    }
    return nullptr;
}

static void Invalidate(std::vector<XboxVolume> &vols, char letter)
{
    for (auto &v : vols) {
        if (v.letter == letter) {
            v.free_known = false;
        }
    }
}

bool XboxDrive::List(char vol, const std::string &path,
                     std::vector<XboxFileInfo> *out, std::string *err)
{
    out->clear();
    Vol *v = Find(vol);
    if (!v) {
        *err = "Drive not found";
        return false;
    }
    struct fatx_dir dir;
    if (fatx_open_dir(&v->fs, path.c_str(), &dir)) {
        *err = "Folder not found";
        return false;
    }
    bool ok = true;
    for (int guard = 0; guard < 1 << 20; guard++) {
        struct fatx_dirent entry, *result = nullptr;
        struct fatx_attr attr;
        int st = fatx_read_dir(&v->fs, &dir, &entry, &attr, &result);
        if (st == FATX_STATUS_SUCCESS) {
            XboxFileInfo fi;
            fi.name = entry.filename;
            fi.dir = (attr.attributes & FATX_ATTR_DIRECTORY) != 0;
            fi.size = fi.dir ? 0 : attr.file_size;
            fi.modified = fatx_ts_to_time_t(&attr.modified);
            out->push_back(std::move(fi));
        } else if (st == FATX_STATUS_END_OF_DIR) {
            break;
        } else if (st != FATX_STATUS_FILE_DELETED) {
            *err = "Couldn't read the folder (the drive may be damaged)";
            ok = false;
            break;
        }
        if (fatx_next_dir_entry(&v->fs, &dir)) {
            break;
        }
    }
    fatx_close_dir(&v->fs, &dir);
    return ok;
}

bool XboxDrive::Stat(char vol, const std::string &path, XboxFileInfo *out)
{
    Vol *v = Find(vol);
    struct fatx_attr attr;
    if (!v || fatx_get_attr(&v->fs, path.c_str(), &attr)) {
        return false;
    }
    out->name = attr.filename;
    out->dir = (attr.attributes & FATX_ATTR_DIRECTORY) != 0;
    out->size = out->dir ? 0 : attr.file_size;
    out->modified = fatx_ts_to_time_t(&attr.modified);
    return true;
}

static bool CheckLeaf(const std::string &path, std::string *err)
{
    std::string name = path.substr(path.find_last_of('/') + 1);
    return XboxValidName(name, err);
}

bool XboxDrive::MakeDir(char vol, const std::string &path, std::string *err)
{
    Vol *v = Find(vol);
    if (!v || !CheckLeaf(path, err)) {
        if (!v) {
            *err = "Drive not found";
        }
        return false;
    }
    XboxFileInfo fi;
    if (Stat(vol, path, &fi)) {
        if (fi.dir) {
            return true;
        }
        *err = "A file with that name already exists";
        return false;
    }
    Invalidate(m_volumes, vol);
    if (fatx_mkdir(&v->fs, path.c_str())) {
        *err = "Couldn't create the folder (drive full?)";
        return false;
    }
    fatx_flush_fat_cache(&v->fs);
    return true;
}

bool XboxDrive::NewFile(char vol, const std::string &path, std::string *err)
{
    Vol *v = Find(vol);
    if (!v) {
        *err = "Drive not found";
        return false;
    }
    if (!CheckLeaf(path, err)) {
        return false;
    }
    Invalidate(m_volumes, vol);
    XboxFileInfo fi;
    if (Stat(vol, path, &fi)) {
        if (fi.dir) {
            *err = "A folder with that name already exists";
            return false;
        }
        if (fatx_truncate(&v->fs, path.c_str(), 0)) {
            *err = "Couldn't replace the existing file";
            return false;
        }
        return true;
    }
    if (fatx_mknod(&v->fs, path.c_str())) {
        *err = "Couldn't create the file (drive full?)";
        return false;
    }
    return true;
}

bool XboxDrive::WriteAt(char vol, const std::string &path, uint64_t offset,
                        const void *buf, size_t len, std::string *err)
{
    Vol *v = Find(vol);
    if (!v) {
        *err = "Drive not found";
        return false;
    }
    if (len == 0) {
        return true;
    }
    int n = fatx_write(&v->fs, path.c_str(), (off_t)offset, len, buf);
    if (n != (int)len) {
        *err = "Couldn't write the file (drive full?)";
        return false;
    }
    return true;
}

int64_t XboxDrive::ReadAt(char vol, const std::string &path, uint64_t offset,
                          void *buf, size_t len)
{
    Vol *v = Find(vol);
    if (!v) {
        return -1;
    }
    int n = fatx_read(&v->fs, path.c_str(), (off_t)offset, len, buf);
    return n < 0 ? -1 : n;
}

bool XboxDrive::RemoveFile(char vol, const std::string &path, std::string *err)
{
    Vol *v = Find(vol);
    Invalidate(m_volumes, vol);
    if (!v || fatx_unlink(&v->fs, path.c_str())) {
        *err = "Couldn't delete the file";
        return false;
    }
    fatx_flush_fat_cache(&v->fs);
    return true;
}

bool XboxDrive::DeleteEmptyDir(char vol, const std::string &path,
                               std::string *err)
{
    Vol *v = Find(vol);
    Invalidate(m_volumes, vol);
    if (!v || fatx_rmdir(&v->fs, path.c_str())) {
        *err = "Couldn't delete the folder";
        return false;
    }
    fatx_flush_fat_cache(&v->fs);
    return true;
}

bool XboxDrive::Rename(char vol, const std::string &from, const std::string &to,
                       std::string *err)
{
    Vol *v = Find(vol);
    if (!v) {
        *err = "Drive not found";
        return false;
    }
    if (!CheckLeaf(to, err)) {
        return false;
    }
    XboxFileInfo fi;
    if (Stat(vol, to, &fi)) {
        *err = "Something with that name already exists";
        return false;
    }
    if (fatx_rename(&v->fs, from.c_str(), to.c_str(), false, true)) {
        *err = "Couldn't rename";
        return false;
    }
    fatx_flush_fat_cache(&v->fs);
    return true;
}

void XboxDrive::UpdateFreeSpace(char vol)
{
    Vol *v = Find(vol);
    if (!v) {
        return;
    }
    uint64_t free_clusters = 0;
    for (size_t i = 1; i < v->fs.num_clusters; i++) {
        uint32_t e;
        if (fatx_read_fat(&v->fs, i, &e)) {
            return;
        }
        if (fatx_get_fat_entry_type(&v->fs, e) == kFatxClusterAvailable) {
            free_clusters++;
        }
    }
    for (auto &info : m_volumes) {
        if (info.letter == vol) {
            info.free = free_clusters * v->fs.bytes_per_cluster;
            info.free_known = true;
        }
    }
}

void XboxDrive::Flush()
{
    for (auto &v : m_vols) {
        fatx_flush_fat_cache(&v->fs);
    }
    if (m_io) {
        m_io->Flush();
    }
}
