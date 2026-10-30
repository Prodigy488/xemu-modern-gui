//
// xemu User Interface - game folders as discs
//
// Lays a folder out as an XDVDFS disc (xiso.cc) and writes it as a disc image
// in xemu's folder_discs directory, the way an XISO tool would. The drive
// then loads that image like any other, so a folder plays exactly like the
// same game as an XISO. The image is kept and reused until the folder's
// files change.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "../xemu-folder-disc.h"
#include "folder-disc.hh"

#include <glib.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#include "../xemu-notifications.h"
#include "../xemu-settings.h"
#include "game-library-scan.hh"
#include "i18n.hh"
#include "misc.hh"
#include "xiso.hh"

namespace fs = std::filesystem;

namespace
{
constexpr int kMaxDepth = 24;

FILE *OpenHost(const std::string &path, const char *mode)
{
#ifdef _WIN32
    std::wstring m(mode, mode + strlen(mode));
    return _wfopen(fs::u8path(path).wstring().c_str(), m.c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

bool SeekHost(FILE *f, uint64_t off)
{
#ifdef _WIN32
    return _fseeki64(f, (__int64)off, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)off, SEEK_SET) == 0;
#endif
}

void Mix(uint64_t &h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 1099511628211ull; // FNV-1a
    }
}

// Reads the folder into a tree; `sig` ends up identifying its names, sizes
// and modification times
bool BuildTree(const fs::path &dir, XisoNode &node, int depth, uint64_t &sig,
               std::string *err)
{
    if (depth > kMaxDepth) {
        *err = "Folders are nested too deeply";
        return false;
    }
    std::error_code ec;
    for (auto &de : fs::directory_iterator(dir, ec)) {
        std::error_code fec;
        XisoNode kid;
        kid.name = de.path().filename().u8string();
        Mix(sig, kid.name.data(), kid.name.size());
        if (de.is_directory(fec)) {
            kid.dir = true;
            if (!BuildTree(de.path(), kid, depth + 1, sig, err)) {
                return false;
            }
        } else if (de.is_regular_file(fec)) {
            kid.size = (uint64_t)de.file_size(fec);
            if (fec) {
                continue;
            }
            kid.source = de.path().u8string();
            auto t = de.last_write_time(fec);
            int64_t tt = fec ? 0 : (int64_t)t.time_since_epoch().count();
            Mix(sig, &kid.size, sizeof(kid.size));
            Mix(sig, &tt, sizeof(tt));
        } else {
            continue; // links to nowhere, devices...
        }
        node.kids.push_back(std::move(kid));
    }
    if (ec) {
        *err = "Can't read the folder: " + ec.message();
        return false;
    }
    return true;
}

std::string IsoPathFor(const std::string &folder)
{
    uint64_t h = 1469598103934665603ull;
    Mix(h, folder.data(), folder.size());
    const char *base = xemu_settings_get_base_path();
    char name[40];
    snprintf(name, sizeof(name), "%016llx.iso", (unsigned long long)h);
    return (fs::u8path(std::string(base ? base : "")) / "folder_discs" / name)
        .u8string();
}

std::string ReadSmall(const std::string &path)
{
    FILE *f = OpenHost(path, "rb");
    if (!f) {
        return "";
    }
    char buf[64] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return std::string(buf, n);
}

// The tree's signature, without laying anything out
bool FolderSignature(const std::string &folder, std::string *sig_out,
                     std::string *err)
{
    XisoNode root;
    root.dir = true;
    uint64_t sig = 1469598103934665603ull;
    if (!BuildTree(fs::u8path(folder), root, 0, sig, err)) {
        return false;
    }
    char s[32];
    snprintf(s, sizeof(s), "%016llx", (unsigned long long)sig);
    *sig_out = s;
    return true;
}

bool IsReady(const std::string &folder, std::string *iso, std::string *why)
{
    std::error_code ec;
    *iso = IsoPathFor(folder);
    if (!fs::exists(fs::u8path(*iso), ec)) {
        *why = "Its disc image hasn't been prepared yet";
        return false;
    }
    std::string sig, err;
    if (!FolderSignature(folder, &sig, &err)) {
        *why = err;
        return false;
    }
    if (ReadSmall(*iso + ".sig") != sig) {
        *why = "The folder changed since its disc image was prepared";
        return false;
    }
    return true;
}

struct Job {
    std::string folder, title, iso;
    std::function<void()> done;
    std::thread th;
    std::atomic<bool> finished{ false };
    std::atomic<uint64_t> copied{ 0 };
    uint64_t total = 0;
    std::string err; // set before `finished`
    int shown = 0;   // progress notices given, in quarters
};

std::unique_ptr<Job> g_job;

void Run(Job *j)
{
    std::error_code ec;
    XisoNode root;
    root.dir = true;
    uint64_t sigv = 1469598103934665603ull;
    std::string err;
    if (!BuildTree(fs::u8path(j->folder), root, 0, sigv, &err)) {
        j->err = err;
        j->finished = true;
        return;
    }
    char sigs[32];
    snprintf(sigs, sizeof(sigs), "%016llx", (unsigned long long)sigv);
    XisoPlan plan;
    if (!XisoLayout(root, &plan, &err)) {
        j->err = err;
        j->finished = true;
        return;
    }
    j->total = std::max<uint64_t>(1, plan.data_bytes);

    fs::path iso = fs::u8path(j->iso);
    fs::create_directories(iso.parent_path(), ec);
    fs::space_info sp = fs::space(iso.parent_path(), ec);
    if (!ec && sp.available < plan.image_size + (64ull << 20)) {
        j->err = tr("Not enough free disk space for the game's disc image");
        j->finished = true;
        return;
    }
    // A stale image or its signature must not look valid while we rewrite it
    fs::remove(fs::u8path(j->iso + ".sig"), ec);
    std::string part = j->iso + ".part";
    FILE *out = OpenHost(part, "wb+");
    if (!out) {
        j->err = std::string(tr("Can't write")) + " \"" + part + "\"";
        j->finished = true;
        return;
    }
    bool ok = true;
    for (auto &b : plan.blobs) {
        ok = ok && SeekHost(out, b.first) &&
             fwrite(b.second.data(), 1, b.second.size(), out) == b.second.size();
    }
    // Full size up front (the end of the image is padding)
    ok = ok && SeekHost(out, plan.image_size - 1) && fputc(0, out) != EOF;

    std::vector<uint8_t> buf(1 << 20);
    for (const XisoNode *f : plan.files) {
        if (!ok) {
            break;
        }
        FILE *in = OpenHost(f->source, "rb");
        if (!in) {
            j->err = std::string(tr("Can't read")) + " \"" + f->source + "\"";
            ok = false;
            break;
        }
        ok = SeekHost(out, (uint64_t)f->sector * kXisoSectorSize);
        uint64_t left = f->size;
        while (ok && left) {
            size_t want = (size_t)std::min<uint64_t>(left, buf.size());
            size_t n = fread(buf.data(), 1, want, in);
            if (n != want) {
                j->err = std::string(tr("Can't read")) + " \"" + f->source + "\"";
                ok = false;
                break;
            }
            if (fwrite(buf.data(), 1, n, out) != n) {
                ok = false;
                break;
            }
            left -= n;
            j->copied += n;
        }
        fclose(in);
    }
    if (fclose(out) != 0) {
        ok = false;
    }
    if (!ok) {
        if (j->err.empty()) {
            j->err = tr("Couldn't write the disc image (is the disk full?)");
        }
        fs::remove(fs::u8path(part), ec);
        j->finished = true;
        return;
    }
    fs::remove(iso, ec);
    fs::rename(fs::u8path(part), iso, ec);
    if (ec) {
        j->err = tr("Couldn't write the disc image (is the disk full?)");
        fs::remove(fs::u8path(part), ec);
        j->finished = true;
        return;
    }
    FILE *sf = OpenHost(j->iso + ".sig", "wb");
    if (sf) {
        fwrite(sigs, 1, strlen(sigs), sf);
        fclose(sf);
    }
    j->finished = true;
}
} // namespace

bool FolderDiscNeedsPrepare(const std::string &folder)
{
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(folder), ec)) {
        return false;
    }
    std::string iso, why;
    return !IsReady(folder, &iso, &why);
}

void FolderDiscPrepare(const std::string &folder, const std::string &title,
                       std::function<void()> done)
{
    if (g_job) {
        xemu_queue_error_message(tr("Another game folder is still being prepared"));
        return;
    }
    if (FindDefaultXbeInFolder(folder).empty()) {
        xemu_queue_error_message(tr("The folder has no default.xbe"));
        return;
    }
    g_job = std::make_unique<Job>();
    g_job->folder = folder;
    g_job->title = title;
    g_job->iso = IsoPathFor(folder);
    g_job->done = std::move(done);
    std::string msg = string_format(
        tr("Preparing %s (first time only, this can take a few minutes)..."),
        title.c_str());
    xemu_queue_notification(msg.c_str());
    Job *j = g_job.get();
    g_job->th = std::thread([j] { Run(j); });
}

void FolderDiscPump()
{
    if (!g_job) {
        return;
    }
    Job *j = g_job.get();
    if (!j->finished) {
        int q = j->total ? (int)(j->copied * 4 / j->total) : 0;
        if (q > j->shown && q < 4) {
            j->shown = q;
            std::string msg = string_format(tr("Preparing %s: %d%%"),
                                            j->title.c_str(), q * 25);
            xemu_queue_notification(msg.c_str());
        }
        return;
    }
    j->th.join();
    std::unique_ptr<Job> job = std::move(g_job);
    if (!job->err.empty()) {
        std::string msg = string_format(tr("Couldn't prepare %s:"),
                                        job->title.c_str()) + " " + job->err;
        xemu_queue_error_message(msg.c_str());
        return;
    }
    if (job->done) {
        job->done();
    }
}

extern "C" char *xemu_folder_disc_ready(const char *folder, char **err_out)
{
    std::string iso, why;
    if (!IsReady(folder, &iso, &why)) {
        if (err_out) {
            *err_out = g_strdup(why.c_str());
        }
        return nullptr;
    }
    return g_strdup(iso.c_str());
}
