//
// xemu User Interface - game folders as discs
//
// Lays a folder out as an XDVDFS disc (xiso.cc) and writes the plan the
// "xplan" block driver reads: the volume descriptor and directory tables, and
// which host file backs each file's sectors. Nothing is copied.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "../xemu-folder-disc.h"

#include <glib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>

#include "../xemu-settings.h"
#include "game-library-scan.hh"
#include "xiso.hh"

namespace fs = std::filesystem;

namespace
{
constexpr int kMaxDepth = 24;

bool BuildTree(const fs::path &dir, XisoNode &node, int depth, std::string *err)
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
        if (de.is_directory(fec)) {
            kid.dir = true;
            if (!BuildTree(de.path(), kid, depth + 1, err)) {
                return false;
            }
        } else if (de.is_regular_file(fec)) {
            kid.size = (uint64_t)de.file_size(fec);
            if (fec) {
                continue;
            }
            kid.source = de.path().u8string();
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

void Put32(std::string &o, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        o.push_back((char)(v >> (8 * i)));
    }
}

void Put64(std::string &o, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        o.push_back((char)(v >> (8 * i)));
    }
}
} // namespace

extern "C" char *xemu_folder_disc_prepare(const char *folder, char **err_out)
{
    std::string err;
    auto fail = [&](const std::string &m) -> char * {
        if (err_out) {
            *err_out = g_strdup(m.c_str());
        }
        return nullptr;
    };

    fs::path dir = fs::u8path(folder);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return fail("The game folder is gone");
    }
    if (FindDefaultXbeInFolder(folder).empty()) {
        return fail("The folder has no default.xbe");
    }

    XisoNode root;
    root.dir = true;
    if (!BuildTree(dir, root, 0, &err)) {
        return fail(err);
    }
    XisoPlan plan;
    if (!XisoLayout(root, &plan, &err)) {
        return fail(err);
    }

    std::string out("XPLAN001", 8);
    Put64(out, plan.image_size);
    Put32(out, (uint32_t)plan.blobs.size());
    for (auto &b : plan.blobs) {
        Put64(out, b.first);
        Put32(out, (uint32_t)b.second.size());
        out.append((const char *)b.second.data(), b.second.size());
    }
    // Files with data, in disc order (XisoLayout hands them over that way)
    std::vector<const XisoNode *> files;
    for (const XisoNode *f : plan.files) {
        if (f->size && !f->source.empty() && f->source.size() < 0xFFFF) {
            files.push_back(f);
        }
    }
    std::stable_sort(files.begin(), files.end(),
                     [](const XisoNode *a, const XisoNode *b) {
                         return a->sector < b->sector;
                     });
    Put32(out, (uint32_t)files.size());
    for (const XisoNode *f : files) {
        Put64(out, (uint64_t)f->sector * kXisoSectorSize);
        Put64(out, f->size);
        out.push_back((char)(f->source.size() & 0xFF));
        out.push_back((char)(f->source.size() >> 8));
        out += f->source;
    }

    const char *base = xemu_settings_get_base_path();
    fs::path plan_dir = fs::u8path(std::string(base ? base : "")) / "folder_discs";
    fs::create_directories(plan_dir, ec);
    char name[40];
    snprintf(name, sizeof(name), "%016llx.xplan",
             (unsigned long long)std::hash<std::string>()(folder));
    fs::path plan_path = plan_dir / name;
    gchar *plan_utf8 = g_strdup(plan_path.u8string().c_str());
    GError *gerr = nullptr;
    if (!g_file_set_contents(plan_utf8, out.data(), (gssize)out.size(), &gerr)) {
        std::string m = std::string("Can't write the disc plan: ") +
                        (gerr ? gerr->message : "unknown error");
        g_clear_error(&gerr);
        g_free(plan_utf8);
        return fail(m);
    }
    char *result = g_strdup_printf("xplan:%s", plan_utf8);
    g_free(plan_utf8);
    return result;
}
