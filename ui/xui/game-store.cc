//
// xemu User Interface - homebrew store (XboxHomebrew.store by Team Resurgent)
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "game-store.hh"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

extern "C" {
#include "util/miniz/miniz.h"
}

using json = nlohmann::json;

const char *const kStoreApiBase = "https://api.xboxhomebrew.store";

namespace
{
std::string Str(const json &o, const char *key)
{
    auto it = o.find(key);
    if (it == o.end() || it->is_null()) {
        return "";
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    if (it->is_number_integer()) {
        return std::to_string(it->get<long long>());
    }
    return "";
}

uint64_t U64(const json &o, const char *key)
{
    auto it = o.find(key);
    if (it == o.end()) {
        return 0;
    }
    if (it->is_number_unsigned() || it->is_number_integer()) {
        long long v = it->get<long long>();
        return v > 0 ? (uint64_t)v : 0;
    }
    if (it->is_string()) {
        return strtoull(it->get<std::string>().c_str(), nullptr, 10);
    }
    return 0;
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// The array itself, or the first array found under a common wrapper key
const json *ArrayOf(const json &j)
{
    if (j.is_array()) {
        return &j;
    }
    if (j.is_object()) {
        for (const char *k : { "items", "apps", "versions", "data", "results" }) {
            auto it = j.find(k);
            if (it != j.end() && it->is_array()) {
                return &*it;
            }
        }
    }
    return nullptr;
}
} // namespace

bool ParseStoreCategories(const std::string &text, std::vector<StoreCategory> *out)
{
    out->clear();
    json j = json::parse(text, nullptr, false);
    const json *a = ArrayOf(j);
    if (!a) {
        return false;
    }
    for (auto &c : *a) {
        if (!c.is_object() || Str(c, "name").empty()) {
            continue;
        }
        StoreCategory cat;
        cat.name = Str(c, "name");
        cat.count = (int)U64(c, "count");
        out->push_back(cat);
    }
    return true;
}

bool ParseStoreApps(const std::string &text, std::vector<StoreApp> *out)
{
    out->clear();
    json j = json::parse(text, nullptr, false);
    const json *a = ArrayOf(j);
    if (!a) {
        return false;
    }
    for (auto &o : *a) {
        if (!o.is_object() || Str(o, "id").empty()) {
            continue;
        }
        StoreApp app;
        app.id = Str(o, "id");
        app.name = Str(o, "name");
        app.author = Str(o, "author");
        app.category = Str(o, "category");
        app.description = Str(o, "description");
        app.latest_version = Str(o, "latest_version");
        app.state = (int)U64(o, "state");
        out->push_back(std::move(app));
    }
    return true;
}

bool ParseStoreVersions(const std::string &text, std::vector<StoreVersion> *out)
{
    out->clear();
    json j = json::parse(text, nullptr, false);
    const json *a = ArrayOf(j);
    if (!a) {
        return false;
    }
    for (auto &o : *a) {
        if (!o.is_object() || Str(o, "id").empty()) {
            continue;
        }
        StoreVersion v;
        v.id = Str(o, "id");
        v.version = Str(o, "version");
        v.release_date = Str(o, "release_date");
        v.changelog = Str(o, "changelog");
        v.folder_name = Str(o, "folder_name");
        v.title_id = Str(o, "title_id");
        v.region = Str(o, "region");
        v.size = U64(o, "size");
        auto f = o.find("download_files");
        if (f != o.end() && f->is_array()) {
            for (auto &e : *f) {
                if (e.is_string()) {
                    v.files.push_back(e.get<std::string>());
                } else if (e.is_object()) {
                    std::string n = Str(e, "name");
                    if (n.empty()) n = Str(e, "file_name");
                    if (n.empty()) n = Str(e, "filename");
                    v.files.push_back(n);
                }
            }
        }
        out->push_back(std::move(v));
    }
    return true;
}

bool StoreCategoryAllowed(const std::string &category)
{
    std::string c = Lower(category);
    return c != "firmware" && c != "bios" && c != "dlc" && c != "saves";
}

bool StoreAppAllowed(const StoreApp &app)
{
    if (!StoreCategoryAllowed(app.category)) {
        return false;
    }
    // Microsoft's own dashboards are its copyrighted software
    if (Lower(app.category) == "dashboards") {
        std::string who = Lower(app.author + " " + app.name);
        if (who.find("microsoft") != std::string::npos) {
            return false;
        }
    }
    return true;
}

std::string StoreUrlEncode(const std::string &s)
{
    std::string out;
    char b[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            snprintf(b, sizeof(b), "%%%02X", c);
            out += b;
        }
    }
    return out;
}

std::string StoreUrlCategories()
{
    return std::string(kStoreApiBase) + "/api/categories";
}

std::string StoreUrlApps(int offset, int count, const std::string &category,
                         const std::string &search)
{
    std::string u = std::string(kStoreApiBase) + "/api/apps?offset=" +
                    std::to_string(offset) + "&count=" + std::to_string(count);
    if (!category.empty()) {
        u += "&category=" + StoreUrlEncode(category);
    }
    if (!search.empty()) {
        u += "&name=" + StoreUrlEncode(search);
    }
    return u;
}

std::string StoreUrlVersions(const std::string &app_id)
{
    return std::string(kStoreApiBase) + "/api/apps/" + StoreUrlEncode(app_id) +
           "/versions";
}

std::string StoreUrlCover(const std::string &app_id, int width, int height)
{
    return std::string(kStoreApiBase) + "/api/Cover/" + StoreUrlEncode(app_id) +
           "?width=" + std::to_string(width) + "&height=" + std::to_string(height);
}

std::string StoreUrlScreenshot(const std::string &app_id, int width, int height)
{
    return std::string(kStoreApiBase) + "/api/Screenshot/" +
           StoreUrlEncode(app_id) + "?width=" + std::to_string(width) +
           "&height=" + std::to_string(height);
}

std::string StoreUrlDownload(const std::string &version_id, int file_index)
{
    return std::string(kStoreApiBase) + "/api/Download/" +
           StoreUrlEncode(version_id) + "?fileIndex=" + std::to_string(file_index);
}

std::string StoreDriveConfirmUrl(const std::string &html)
{
    // <form id="download-form" action="https://drive.usercontent.google.com/download">
    //   <input type="hidden" name="id" value="..."> ... name="confirm", "uuid"
    size_t form = html.find("download-form");
    if (form == std::string::npos ||
        html.find("drive.usercontent.google.com") == std::string::npos) {
        return "";
    }
    size_t form_start = html.rfind("<form", form);
    size_t form_end = html.find("</form>", form);
    if (form_start == std::string::npos || form_end == std::string::npos) {
        return "";
    }
    std::string f = html.substr(form_start, form_end - form_start);
    auto attr = [](const std::string &tag, const char *name) {
        std::string key = std::string(name) + "=\"";
        size_t p = tag.find(key);
        if (p == std::string::npos) {
            return std::string();
        }
        p += key.size();
        size_t e = tag.find('"', p);
        return e == std::string::npos ? std::string() : tag.substr(p, e - p);
    };
    std::string action = attr(f.substr(0, f.find('>')), "action");
    if (action.rfind("https://drive.usercontent.google.com/", 0) != 0) {
        return "";
    }
    std::string q;
    size_t p = 0;
    while ((p = f.find("<input", p)) != std::string::npos) {
        size_t e = f.find('>', p);
        std::string tag = f.substr(p, e == std::string::npos ? std::string::npos : e - p);
        std::string name = attr(tag, "name"), value = attr(tag, "value");
        if (!name.empty()) {
            q += (q.empty() ? "?" : "&") + StoreUrlEncode(name) + "=" +
                 StoreUrlEncode(value);
        }
        p = e == std::string::npos ? f.size() : e;
    }
    return q.empty() ? "" : action + q;
}

std::string StoreSafeFolderName(const std::string &name)
{
    std::string out;
    for (unsigned char c : name) {
        if (c < 0x20 || c > 0x7e || strchr("<>:\"/\\|?*", c)) {
            continue;
        }
        out += (char)c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    while (!out.empty() && out[0] == ' ') {
        out.erase(0, 1);
    }
    if (out.size() > 42) {
        out.resize(42);
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
            out.pop_back();
        }
    }
    return out.empty() ? "App" : out;
}

namespace fs = std::filesystem;

namespace
{
size_t WriteToStream(void *opaque, mz_uint64 ofs, const void *buf, size_t n)
{
    (void)ofs;
    auto *o = static_cast<std::ofstream *>(opaque);
    o->write((const char *)buf, (std::streamsize)n);
    return o->good() ? n : 0;
}
} // namespace

bool StoreExtractZip(const std::string &zip_path, const std::string &dest_dir,
                     std::string *err)
{
    std::ifstream in(fs::u8path(zip_path), std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), {});
    if (data.size() < 4 || data.compare(0, 2, "PK") != 0) {
        *err = "The download isn't a zip file";
        return false;
    }
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) {
        *err = "The zip file is damaged";
        return false;
    }
    std::error_code ec;
    fs::path dest = fs::u8path(dest_dir);
    fs::create_directories(dest, ec);
    bool ok = true;
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n && ok; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            continue;
        }
        std::string name = st.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');
        fs::path rel = fs::u8path(name).lexically_normal();
        if (name.empty() || rel.is_absolute() || name[0] == '/' ||
            (!rel.empty() && *rel.begin() == "..") || name.find(':') != std::string::npos) {
            *err = "The zip file contains an unsafe path: " + name;
            ok = false;
            break;
        }
        fs::path out = dest / rel;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            fs::create_directories(out, ec);
            continue;
        }
        fs::create_directories(out.parent_path(), ec);
        std::ofstream o(out, std::ios::binary | std::ios::trunc);
        if (!o || !mz_zip_reader_extract_to_callback(&zip, i, WriteToStream, &o, 0)) {
            *err = "Couldn't unpack " + name;
            ok = false;
        }
    }
    mz_zip_reader_end(&zip);
    return ok;
}

std::string StoreAppRoot(const std::string &dir)
{
    std::error_code ec;
    fs::path only;
    int count = 0;
    for (auto &e : fs::directory_iterator(fs::u8path(dir), ec)) {
        count++;
        only = e.path();
    }
    if (count == 1 && fs::is_directory(only, ec)) {
        return only.u8string();
    }
    return dir;
}
