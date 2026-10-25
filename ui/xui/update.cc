//
// xemu User Interface
//
// Copyright (C) 2020-2025 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "common.hh"
#include "qemu/http.h"
#include "update.hh"
#include "update-logic.hh"
#include "viewport-manager.hh"
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL_filesystem.h>
#include "util/miniz/miniz.h"
#include "xemu-version.h"
#include <nlohmann/json.hpp>
#include <cctype>
#include <string>
#include <vector>

using json = nlohmann::json;

// This unofficial build updates from its own releases, so updating never
// replaces it with official xemu (which doesn't have its features)
const char *releases_url = "https://api.github.com/repos/Prodigy488/xemu-modern-gui/releases/latest";
const char *const kModernGuiVersion = "2.0";

#if defined(__x86_64__)
#define PACKAGE_ARCH "x86_64"
#elif defined(__aarch64__)
#define PACKAGE_ARCH "arm64"
#else
#error Unhandled package arch
#endif

#define DPRINTF(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__);

AutoUpdateWindow update_window;

AutoUpdateWindow::AutoUpdateWindow()
{
    is_open = false;
}

void AutoUpdateWindow::CheckForUpdates()
{
    updater.check_for_update([this](){
        is_open |= updater.is_update_available();
    });
}

void AutoUpdateWindow::Draw()
{
    if (!is_open) return;
    ImGui::SetNextWindowContentSize(ImVec2(550.0f*g_viewport_mgr.m_scale, 0.0f));
    if (!ImGui::Begin(trid("Update"), &is_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    if (ImGui::IsWindowAppearing() && !updater.is_update_available()) {
        updater.check_for_update();
    }

    const char *status_msg[] = {
        "",
        tr("An error has occurred. Try again."),
        tr("Checking for update..."),
        tr("Downloading update..."),
        tr("Update successful! Restart to launch the updated version.")
    };
    const char *available_msg[] = {
        tr("Update availability unknown."),
        tr("This version of xemu Modern GUI is up to date."),
        tr("An updated version of xemu Modern GUI is available!"),
    };

    if (updater.get_status() == UPDATER_IDLE) {
        ImGui::Text("%s", available_msg[updater.get_update_availability()]);
    } else {
        ImGui::Text("%s", status_msg[updater.get_status()]);
    }

    if (updater.is_update_available()) {
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));

        ImGui::Text(tr("Current version: %s (xemu %s)"), kModernGuiVersion, xemu_version);
        ImGui::Text(tr("Latest version: %s"), updater.get_release_version().c_str());

        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));

        if (ImGui::SmallButton(tr("Release notes..."))) {
            SDL_OpenURL(updater.get_release_url().c_str());
        }
    }

    if (updater.is_updating()) {
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
        ImGui::ProgressBar(updater.get_update_progress_percentage()/100.0f,
                           ImVec2(-1.0f, 0.0f));
    }

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().WindowPadding.y));

    float w = (130)*g_viewport_mgr.m_scale;
    float bw = w + (10)*g_viewport_mgr.m_scale;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth()-bw);

    if (updater.is_checking_for_update() || updater.is_updating()) {
        if (ImGui::Button(tr("Cancel"), ImVec2(w, 0))) {
            updater.cancel();
        }
    } else {
        if (updater.is_pending_restart()) {
            if (ImGui::Button(tr("Restart"), ImVec2(w, 0))) {
                updater.restart_to_updated();
            }
        } else if (updater.is_update_available()) {
            if (ImGui::Button(tr("Update"), ImVec2(w, 0))) {
                updater.update();
            }
        } else {
            if (ImGui::Button(tr("Check for Update"), ImVec2(w, 0))) {
                updater.check_for_update();
            }
        }
    }

    ImGui::End();
}

Updater::Updater()
{
    m_status = UPDATER_IDLE;
    m_update_availability = UPDATE_AVAILABILITY_UNKNOWN;
    m_update_percentage = 0;
    m_release_version = "Unknown";
    m_should_cancel = false;
    m_thread_started = false;
}

void Updater::check_for_update(UpdaterCallback on_complete)
{
    if (m_status == UPDATER_IDLE || m_status == UPDATER_ERROR) {
        if (m_thread_started) {
            qemu_thread_join(&m_thread); // the last one has finished
        }
        m_on_complete = on_complete;
        m_status = UPDATER_CHECKING_FOR_UPDATE;
        m_thread_started = true;
        qemu_thread_create(&m_thread, "update_worker",
                           &Updater::checker_thread_worker_func,
                           this, QEMU_THREAD_JOINABLE);
    }
}

void *Updater::checker_thread_worker_func(void *updater)
{
    ((Updater *)updater)->check_for_update_internal();
    return NULL;
}

void Updater::check_for_update_internal()
{
    g_autoptr(GByteArray) data = g_byte_array_new();
    int res = http_get(releases_url, data, NULL, NULL);

    if (m_should_cancel) {
        m_should_cancel = false;
        m_status = UPDATER_IDLE;
        goto finished;
    } else if (res != 200) {
        m_status = UPDATER_ERROR;
        goto finished;
    }

    {
        UpdateRelease rel;
        std::string err;
        if (!UpdateParseRelease(std::string((const char *)data->data, data->len),
                                PACKAGE_ARCH, &rel, &err)) {
            DPRINTF("Update check: %s\n", err.c_str());
            m_status = UPDATER_ERROR;
            goto finished;
        }
        m_release_version = rel.version;
        m_release_url = rel.url.empty()
            ? "https://github.com/Prodigy488/xemu-modern-gui/releases/latest"
            : rel.url;
        m_release_package_url = rel.package_url;
        m_update_availability = UpdateIsNewer(m_release_version, kModernGuiVersion)
                                    ? UPDATE_AVAILABLE
                                    : UPDATE_NOT_AVAILABLE;
    }

    m_status = UPDATER_IDLE;
finished:
    if (m_on_complete) {
        m_on_complete();
    }
}

void Updater::update()
{
    if (m_status == UPDATER_IDLE || m_status == UPDATER_ERROR) {
        if (m_thread_started) {
            qemu_thread_join(&m_thread);
        }
        m_status = UPDATER_UPDATING;
        m_thread_started = true;
        qemu_thread_create(&m_thread, "update_worker",
                           &Updater::update_thread_worker_func,
                           this, QEMU_THREAD_JOINABLE);
    }
}

void *Updater::update_thread_worker_func(void *updater)
{
    ((Updater *)updater)->update_internal();
    return NULL;
}

int Updater::progress_cb(http_progress_cb_info *info)
{
    if (info->dltotal == 0) {
        m_update_percentage = 0;
    } else {
        m_update_percentage = info->dlnow*100/info->dltotal;
    }

    return m_should_cancel;
}

void Updater::update_internal()
{
    g_autoptr(GByteArray) data = g_byte_array_new();

    http_progress_cb_info progress_info;
    progress_info.userptr = this;
    progress_info.progress = [](http_progress_cb_info *info) {
        return static_cast<Updater *>(info->userptr)->progress_cb(info);
    };

    int res = http_get(m_release_package_url.c_str(), data, &progress_info, NULL);

    if (m_should_cancel) {
        m_should_cancel = false;
        m_status = UPDATER_IDLE;
        return;
    } else if (res != 200) {
        m_status = UPDATER_ERROR;
        return;
    }

    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_mem(&zip, data->data, data->len, 0)) {
        DPRINTF("mz_zip_reader_init_mem failed\n");
        m_status = UPDATER_ERROR;
        return;
    }

    mz_uint num_files = mz_zip_reader_get_num_files(&zip);
    for (mz_uint file_idx = 0; file_idx < num_files; file_idx++) {
        mz_zip_archive_file_stat fstat;
        if (!mz_zip_reader_file_stat(&zip, file_idx, &fstat)) {
            DPRINTF("mz_zip_reader_file_stat failed for file #%d\n", file_idx);
            goto errored;
        }

        if (fstat.m_filename[strlen(fstat.m_filename)-1] == '/') {
            /* FIXME: mkdirs */
            DPRINTF("FIXME: subdirs not handled yet\n");
            goto errored;
        }

        // Wide paths: the folder may have characters outside the ANSI
        // code page
        char *dst_path = g_strdup_printf("%s%s", SDL_GetBasePath(), fstat.m_filename);
        DPRINTF("extracting %s to %s\n", fstat.m_filename, dst_path);
        gunichar2 *dst_w = g_utf8_to_utf16(dst_path, -1, NULL, NULL, NULL);

        if (!strcmp(fstat.m_filename, "xemu.exe")) {
            // We cannot overwrite current executable, but we can move it
            char *renamed_path = g_strdup_printf("%s%s", SDL_GetBasePath(), "xemu-previous.exe");
            gunichar2 *renamed_w = g_utf8_to_utf16(renamed_path, -1, NULL, NULL, NULL);
            if (dst_w && renamed_w) {
                MoveFileExW((LPCWSTR)dst_w, (LPCWSTR)renamed_w,
                            MOVEFILE_REPLACE_EXISTING);
            }
            g_free(renamed_w);
            g_free(renamed_path);
        }

        size_t out_size = 0;
        void *out_data = mz_zip_reader_extract_to_heap(&zip, file_idx, &out_size, 0);
        FILE *out = dst_w ? _wfopen((const wchar_t *)dst_w, L"wb") : NULL;
        bool written = out_data && out &&
                       (out_size == 0 || fwrite(out_data, 1, out_size, out) == out_size);
        if (out) {
            written = (fclose(out) == 0) && written;
        }
        mz_free(out_data);
        g_free(dst_w);
        if (!written) {
            DPRINTF("Couldn't write %s\n", dst_path);
            g_free(dst_path);
            goto errored;
        }

        g_free(dst_path);
    }

    m_status = UPDATER_UPDATE_SUCCESSFUL;
    goto cleanup_zip;
errored:
    m_status = UPDATER_ERROR;
cleanup_zip:
    mz_zip_reader_end(&zip);
}

void Updater::restart_to_updated()
{
    // Start the new xemu.exe with the original command line (properly
    // quoted, unlike _execv with paths containing spaces), then exit
    char *target_exec = g_strdup_printf("%s%s", SDL_GetBasePath(), "xemu.exe");
    DPRINTF("Restarting to updated executable %s\n", target_exec);
    gunichar2 *exe_w = g_utf8_to_utf16(target_exec, -1, NULL, NULL, NULL);
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (exe_w && CreateProcessW((LPCWSTR)exe_w, &cmd[0], NULL, NULL, FALSE, 0,
                                NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        g_free(exe_w);
        g_free(target_exec);
        exit(0);
    }
    DPRINTF("Launching updated executable failed\n");
    g_free(exe_w);
    g_free(target_exec);
    exit(1);
}
