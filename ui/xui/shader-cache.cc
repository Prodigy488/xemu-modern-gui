//
// xemu User Interface - shader cache per game
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "shader-cache.hh"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <vector>

#include "../xemu-settings.h"
#include "../xemu-shader-cache.h"

namespace fs = std::filesystem;

namespace
{
std::mutex g_mutex;
uint32_t g_title = 0;
std::set<std::string> g_seen;        // noted for g_title this session
std::vector<std::string> g_pending;  // not yet written
std::atomic<bool> g_discard_pipelines{ false };

fs::path Base()
{
    return fs::u8path(xemu_settings_get_base_path());
}

fs::path ListPath(uint32_t title_id)
{
    char name[32];
    snprintf(name, sizeof(name), "%08X.txt", title_id);
    return Base() / "shader_cache_titles" / name;
}

// Only files inside xemu's own shader cache folders are ever deleted
bool InCacheDir(const fs::path &p)
{
    fs::path rel = p.lexically_normal().lexically_relative(Base().lexically_normal());
    if (rel.empty() || *rel.begin() == "..") {
        return false;
    }
    std::string top = rel.begin()->u8string();
    return top == "shaders" || top == "vk_spirv_cache";
}

std::vector<std::string> ReadList(uint32_t title_id)
{
    std::vector<std::string> out;
    std::ifstream in(ListPath(title_id));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    return out;
}

void FlushLocked()
{
    if (g_pending.empty() || !g_title) {
        g_pending.clear();
        return;
    }
    std::error_code ec;
    fs::create_directories(ListPath(g_title).parent_path(), ec);
    std::ofstream o(ListPath(g_title), std::ios::app);
    for (auto &p : g_pending) {
        o << p << "\n";
    }
    g_pending.clear();
}

int RemoveFiles(const std::vector<std::string> &rels)
{
    int n = 0;
    std::error_code ec;
    for (auto &r : rels) {
        fs::path p = Base() / fs::u8path(r);
        if (InCacheDir(p) && fs::remove(p, ec)) {
            n++;
        }
    }
    return n;
}

void DropPipelineCache()
{
    std::error_code ec;
    g_discard_pipelines = true;
    fs::remove(Base() / "vk_pipeline_cache.bin", ec);
}
} // namespace

extern "C" void xemu_shader_cache_note(const char *path)
{
    if (!path) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_title) {
        return;
    }
    fs::path p = fs::u8path(path).lexically_normal();
    fs::path rel = p.lexically_relative(Base().lexically_normal());
    std::string key = (rel.empty() || *rel.begin() == "..") ? p.generic_u8string()
                                                             : rel.generic_u8string();
    if (g_seen.insert(key).second) {
        g_pending.push_back(key);
    }
}

extern "C" bool xemu_shader_cache_pipelines_discarded(void)
{
    return g_discard_pipelines;
}

void ShaderCacheSetTitle(uint32_t title_id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (title_id == g_title) {
        return;
    }
    FlushLocked();
    g_title = title_id;
    g_seen.clear();
    for (auto &p : ReadList(title_id)) {
        g_seen.insert(p);
    }
}

void ShaderCacheFlush(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FlushLocked();
}

int ShaderCacheCountFor(uint32_t title_id)
{
    ShaderCacheFlush();
    std::error_code ec;
    int n = 0;
    for (auto &r : ReadList(title_id)) {
        n += fs::exists(Base() / fs::u8path(r), ec) ? 1 : 0;
    }
    return n;
}

int ShaderCacheClearFor(uint32_t title_id, std::string *msg)
{
    std::vector<std::string> list;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        FlushLocked();
        list = ReadList(title_id);
        if (title_id == g_title) {
            g_seen.clear();
        }
    }
    int n = RemoveFiles(list);
    std::error_code ec;
    fs::remove(ListPath(title_id), ec);
    DropPipelineCache();
    char b[256];
    snprintf(b, sizeof(b),
             "Removed %d shader file%s for this game and the Vulkan pipeline "
             "cache (shared by all games, rebuilt as you play). Restart xemu "
             "before playing it again so nothing old stays in memory.",
             n, n == 1 ? "" : "s");
    *msg = b;
    return n;
}

int ShaderCacheClearAll(std::string *msg)
{
    std::error_code ec;
    int n = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.clear();
        g_seen.clear();
    }
    for (const char *d : { "shaders", "vk_spirv_cache", "shader_cache_titles" }) {
        n += (int)fs::remove_all(Base() / d, ec);
    }
    fs::remove(Base() / "shader_cache_list", ec);
    DropPipelineCache();
    *msg = "Removed every cached shader for every game. Restart xemu so "
           "nothing old stays in memory; games stutter a little the first "
           "time while shaders are rebuilt.";
    return n;
}
