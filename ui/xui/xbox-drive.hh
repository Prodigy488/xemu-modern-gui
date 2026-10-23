//
// xemu User Interface - Xbox hard drive access
//
// Reads and writes files on the emulated Xbox hard drive's FATX partitions
// (C, E, F, X, Y, Z) using libfatx. The disk itself is reached through an
// XboxDiskIO: in xemu that is QEMU's block layer (so qcow2 images work), in
// tests a raw image file.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

class XboxDiskIO
{
public:
    virtual ~XboxDiskIO() = default;
    virtual bool Read(uint64_t offset, void *buf, size_t len) = 0;
    virtual bool Write(uint64_t offset, const void *buf, size_t len) = 0;
    virtual uint64_t Size() = 0;
    virtual void Flush()
    {
    }
};

// A raw disk image file (used by tests)
std::unique_ptr<XboxDiskIO> XboxDiskOpenFile(const std::string &path);

struct XboxVolume {
    char letter;          // 'C', 'E', ...
    const char *label;    // "System", "Data", ...
    uint64_t size = 0;    // bytes
    uint64_t free = 0;    // bytes, valid when free_known
    bool free_known = false;
};

struct XboxFileInfo {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
    time_t modified = 0;
};

// Longest file name FATX can store
constexpr size_t kXboxMaxName = 42;

class XboxDrive
{
public:
    XboxDrive();
    ~XboxDrive();

    // Find the FATX partitions on the disk
    bool Open(std::unique_ptr<XboxDiskIO> io, std::string *err);
    void Close();
    bool IsOpen() const
    {
        return m_io != nullptr;
    }
    const std::vector<XboxVolume> &Volumes() const
    {
        return m_volumes;
    }
    const XboxVolume *Volume(char letter) const;

    // Paths use '/' and start at the partition root: "/", "/Apps/Foo"
    bool List(char vol, const std::string &path, std::vector<XboxFileInfo> *out,
              std::string *err);
    bool Stat(char vol, const std::string &path, XboxFileInfo *out);
    bool MakeDir(char vol, const std::string &path, std::string *err);
    // Create (or empty) a file for writing
    bool NewFile(char vol, const std::string &path, std::string *err);
    bool WriteAt(char vol, const std::string &path, uint64_t offset,
                 const void *buf, size_t len, std::string *err);
    // Bytes read, or -1
    int64_t ReadAt(char vol, const std::string &path, uint64_t offset, void *buf,
                   size_t len);
    bool RemoveFile(char vol, const std::string &path, std::string *err);
    bool DeleteEmptyDir(char vol, const std::string &path, std::string *err);
    bool Rename(char vol, const std::string &from, const std::string &to,
                std::string *err);

    // Count free space (scans the FAT; cached until the next write)
    void UpdateFreeSpace(char vol);
    // Write cached FAT changes to the disk
    void Flush();

private:
    struct Vol;
    Vol *Find(char letter);
    std::unique_ptr<XboxDiskIO> m_io;
    std::vector<std::unique_ptr<Vol>> m_vols;
    std::vector<XboxVolume> m_volumes;
};

// Is name a valid FATX file name? If not, *why says why.
bool XboxValidName(const std::string &name, std::string *why);

// Join "/a" + "b" -> "/a/b"
std::string XboxJoin(const std::string &dir, const std::string &name);
// Parent of "/a/b" -> "/a"; of "/a" -> "/"
std::string XboxParent(const std::string &path);
