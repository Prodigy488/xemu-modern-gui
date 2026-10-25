//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
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
#include "game-library.hh"
#include "misc.hh"
#include "notifications.hh"
#include "snapshot-manager.hh"
#include "viewport-manager.hh"
#include "xemu-hud.h"

#include <ctime>

SnapshotManager g_snapshot_mgr;

SnapshotManager::SnapshotManager()
{
    m_snapshots = NULL;
    m_extra_data = NULL;
    m_load_failed = false;
    m_open_pending = false;
    m_snapshots_len = 0;
}

SnapshotManager::~SnapshotManager()
{
    g_free(m_snapshots);
    g_free(m_extra_data);
    xemu_snapshots_mark_dirty();
}

void SnapshotManager::Refresh()
{
    Error *err = NULL;

    if (!m_load_failed) {
        m_snapshots_len = xemu_snapshots_list(&m_snapshots, &m_extra_data, &err);
    }

    if (err) {
        m_load_failed = true;
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
        m_snapshots_len = 0;
    }
}

void SnapshotManager::LoadSnapshotChecked(const char *name)
{
    Refresh();

    for (int i = 0; i < m_snapshots_len; i++) {
        if (!strcmp(m_snapshots[i].name, name)) {
            // Made before changes to the Xbox hard drive? Loading it would
            // put the drive back the way it was then
            auto changes = GameLibraryDriveChangesSince(m_snapshots[i].date_sec);
            if (!changes.empty()) {
                m_drive_changes.clear();
                for (auto &c : changes) {
                    char when[64] = "";
                    time_t t = (time_t)c.time;
                    struct tm *tm = localtime(&t);
                    if (tm) {
                        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);
                    }
                    m_drive_changes.push_back(std::string(when) + "  " + c.what);
                }
                m_drive_load_name = name;
                m_drive_disc_path = m_extra_data[i].disc_path ? m_extra_data[i].disc_path : "";
                m_drive_open_pending = true;
                return;
            }
            break;
        }
    }
    LoadSnapshotCheckedDisc(name);
}

void SnapshotManager::LoadSnapshotCheckedDisc(const char *name)
{
    Refresh();

    XemuSnapshotData *data = NULL;
    for (int i = 0; i < m_snapshots_len; i++) {
        if (!strcmp(m_snapshots[i].name, name)) {
            data = &m_extra_data[i];
            break;
        }
    }

    if (data == NULL) {
        return;
    }

    char *current_disc_path = xemu_get_currently_loaded_disc_path();
    if (data->disc_path && (!current_disc_path || strcmp(current_disc_path, data->disc_path))) {
        if (current_disc_path) {
            m_current_disc_path = current_disc_path;
        } else {
            m_current_disc_path.clear();
        }
        m_target_disc_path = data->disc_path;
        m_pending_load_name = name;
        m_open_pending = true;
    } else {
        if (!data->disc_path) {
            xemu_eject_disc(NULL);
        }
        LoadSnapshot(name);
    }

    if (current_disc_path) {
        g_free(current_disc_path);
    }
}

void SnapshotManager::LoadSnapshot(const char *name)
{
    Error *err = NULL;

    xemu_snapshots_load(name, &err);

    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
    }
}

void SnapshotManager::Draw()
{
    DrawSnapshotDiscLoadDialog();
    DrawDriveChangesDialog();
}

void SnapshotManager::DrawDriveChangesDialog()
{
    if (m_drive_open_pending) {
        ImGui::OpenPopup(trid("Load an older save state?"));
        m_drive_open_pending = false;
    }
    ImGui::SetNextWindowSize(ImVec2(560 * g_viewport_mgr.m_scale, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(trid("Load an older save state?"), NULL,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    float wrap = 540 * g_viewport_mgr.m_scale;
    ImGui::PushTextWrapPos(wrap);
    ImGui::TextUnformatted(tr("A save state puts the whole Xbox back the way it was, "
                              "hard drive included. This one was made before these "
                              "changes to the Xbox hard drive, so loading it would "
                              "undo them:"));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Indent();
    for (auto &c : m_drive_changes) {
        ImGui::BulletText("%s", c.c_str());
    }
    ImGui::Unindent();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(wrap);
    ImGui::TextDisabled("%s", tr("Homebrew installed since then would have to be "
                                 "installed again. Game saves made since then in "
                                 "other games are also undone."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 8 * g_viewport_mgr.m_scale));
    if (!m_drive_disc_path.empty()) {
        if (ImGui::Button(tr("Start the game without it"))) {
            // Boots the game fresh; the hard drive stays as it is
            GameLibraryStartFresh(m_drive_disc_path.c_str());
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemTooltip("%s", tr("Keeps everything on the hard drive"));
        ImGui::SameLine();
    }
    if (ImGui::Button(tr("Load anyway"))) {
        std::string name = m_drive_load_name;
        ImGui::CloseCurrentPopup();
        LoadSnapshotCheckedDisc(name.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void SnapshotManager::DrawSnapshotDiscLoadDialog()
{
    if (m_open_pending) {
        ImGui::OpenPopup(trid("DVD Drive Image"));
        m_open_pending = false;
    }

    if (!ImGui::BeginPopupModal(trid("DVD Drive Image"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::Text(tr("The DVD drive disc image mounted when the snapshot was created does not appear to be loaded:"));
    ImGui::Spacing();
    ImGui::Indent();
    ImGui::Text(tr("Current Image: %s"), m_current_disc_path.length() ? m_current_disc_path.c_str() : tr("(None)"));
    ImGui::Text(tr("Expected Image: %s"), m_target_disc_path.length() ? m_target_disc_path.c_str() : tr("(None)"));
    ImGui::Unindent();
    ImGui::Spacing();
    ImGui::Text(tr("Would you like to load it now?"));
    
    ImGui::Dummy(ImVec2(0,16));

    if (ImGui::Button(tr("Yes"), ImVec2(120, 0))) {
        xemu_eject_disc(NULL);
        
        Error *err = NULL;
        xemu_load_disc(m_target_disc_path.c_str(), &err);
        if (err) {
            xemu_queue_error_message(error_get_pretty(err));
            error_free(err);
        } else {
            LoadSnapshot(m_pending_load_name.c_str());
        }

        ImGui::CloseCurrentPopup();
    }
    
    ImGui::SameLine();
    if (ImGui::Button(tr("No"), ImVec2(120, 0))) {
        LoadSnapshot(m_pending_load_name.c_str());
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"), ImVec2(120, 0))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}
